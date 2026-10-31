// adc2can - sensor inputs (2x ADS1115), switched 5 V rails (2x TPS4H160A), monitors and fault handling.
//
// ADS1115: single-shot, +/-4.096 V FSR (15 MOhm differential input impedance - no measurable load on the 10k/11k
// divider), 860 SPS. Both chips convert the same AIN index together: 4 steps x ~1.3 ms = every channel ~180 Hz.
// Rails: EN = S3 GPIO; ST# (open drain, low = over-current / over-temperature) -> TCA9554 inputs (internal pull-ups).
// THER is tied low, so the TPS4H160A retries on its own after a thermal trip; FAULT_LATCH switches the rail off.
// The sensor bus is BUCK_OUT through an SS54: on USB power alone there is no 12 V, no buck and so no sensor supply.
#include "adc2can_defs.h"
#include <Wire.h>
#include <math.h>

// ---------------------------------------------------------------- I2C helpers
static bool wr16(uint8_t addr, uint8_t reg, uint16_t v) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(v >> 8);
  Wire.write(v & 0xFF);
  return Wire.endTransmission() == 0;
}
static bool wr8(uint8_t addr, uint8_t reg, uint8_t v) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}
static bool rd16(uint8_t addr, uint8_t reg, int16_t &out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, 2) != 2) return false;
  out = (int16_t)((Wire.read() << 8) | Wire.read());
  return true;
}
static bool rd8(uint8_t addr, uint8_t reg, uint8_t &out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, 1) != 1) return false;
  out = Wire.read();
  return true;
}

// ---------------------------------------------------------------- ADS1115
static const float ADS_LSB = 4.096f / 32768.0f;
static uint8_t adsStep = 0;       // AIN index being converted on both chips
static uint32_t adsStartUs = 0;
static bool adsBusy = false;
static float raw[NUM_CH];         // unsmoothed connector volts

static uint16_t adsConfig(uint8_t ain) {
  return (1u << 15)                    // OS: start a single conversion
         | ((0x4u + ain) << 12)        // MUX: AINx vs GND
         | (0x1u << 9)                 // PGA +/-4.096 V
         | (1u << 8)                   // single-shot
         | (0x7u << 5)                 // 860 SPS
         | 0x3u;                       // comparator off
}

static void adsStart() {
  live.adcOk[0] = wr16(ADS_ADDR_LO, 0x01, adsConfig(adsStep));
  live.adcOk[1] = wr16(ADS_ADDR_HI, 0x01, adsConfig(adsStep));
  adsStartUs = micros();
  adsBusy = true;
}

static void adsCollect() {
  const uint8_t addr[2] = {ADS_ADDR_LO, ADS_ADDR_HI};
  for (uint8_t c = 0; c < 2; c++) {
    int16_t code;
    const uint8_t ch = c * 4 + adsStep;
    if (live.adcOk[c] && rd16(addr[c], 0x00, code)) {
      raw[ch] = (code < 0 ? 0 : code) * ADS_LSB * DIV_IN * cfg.ch[ch].trim;
    } else {
      live.adcOk[c] = false;
      raw[ch] = NAN;
    }
  }
  adsStep = (adsStep + 1) & 3;
  adsBusy = false;
}

// ---------------------------------------------------------------- conversion
static float tableLookup(const ChCfg &c, float v) {
  uint8_t n = 0;
  while (n < TABLE_PTS && isfinite(c.tblV[n]) && isfinite(c.tblVal[n])) n++;
  if (n == 0) return NAN;
  if (n == 1 || v <= c.tblV[0]) return c.tblVal[0];
  for (uint8_t k = 1; k < n; k++) {
    if (v <= c.tblV[k]) {
      const float span = c.tblV[k] - c.tblV[k - 1];
      const float f = span > 0 ? (v - c.tblV[k - 1]) / span : 0;
      return c.tblVal[k - 1] + f * (c.tblVal[k] - c.tblVal[k - 1]);
    }
  }
  return c.tblVal[n - 1];
}

// NTC to its own rail through R_PUn: the 21k input divider sits in parallel with the sensor
static float ntcTemp(const ChCfg &c, float v, float rail) {
  if (!(rail > 1.0f) || v <= 0.001f || v >= rail - 0.001f || c.ntcPullup <= 0 || c.ntcR25 <= 0 || c.ntcBeta <= 0) return NAN;
  const float rMeas = c.ntcPullup * v / (rail - v);
  const float g = 1.0f / rMeas - 1.0f / 21000.0f;
  if (g <= 0) return NAN;
  const float r = 1.0f / g;
  const float invT = 1.0f / 298.15f + logf(r / c.ntcR25) / c.ntcBeta;
  return 1.0f / invT - 273.15f;
}

static float convert(uint8_t i, float v, float rail) {
  const ChCfg &c = cfg.ch[i];
  float vc = v;
  if (c.ratiometric && c.mode != MODE_NTC) {
    if (!(rail > 3.5f)) return NAN; // rail missing: a ratiometric sensor has no meaningful output
    vc = v * 5.0f / rail;
  }
  live.corrV[i] = vc;
  switch (c.mode) {
    case MODE_TABLE: return tableLookup(c, vc);
    case MODE_NTC: return ntcTemp(c, v, rail);
    default: return c.scale * vc + c.offset;
  }
}

// ---------------------------------------------------------------- rails
static void driveRail(uint8_t i, bool on) {
  live.railEn[i] = on;
  digitalWrite(PIN_EN[i], on ? HIGH : LOW);
}

void railReset(uint8_t ch) {
  for (uint8_t i = 0; i < NUM_CH; i++) {
    if (ch != 0xFF && ch != i) continue;
    live.latched[i] = false;
    live.railFault[i] = false;
    live.faultSince[i] = 0;
  }
}

void applyTermination() {
  if (cfg.canTerm) {
    pinMode(PIN_CAN_TERM, OUTPUT);
    digitalWrite(PIN_CAN_TERM, LOW); // P-FET on: 120R in circuit
  } else {
    pinMode(PIN_CAN_TERM, INPUT);    // released: R_TGU holds the gate high = off (same as during boot)
  }
}

static float mv(uint8_t pin, float div) { return analogReadMilliVolts(pin) * 0.001f * div; }

static void railsTick() {
  const uint32_t now = millis();
  live.busV = mv(PIN_SBUS_MON, DIV_RAIL);
  live.vbat = mv(PIN_VBAT_MON, DIV_VBAT);
  live.no12v = live.vbat < cfg.no12vBelow;
  // 12 V pushed onto a sensor's 5 V pin back-feeds the sensor bus through that switch's body diode (the SS54 keeps it
  // off the buck output / logic 5 V). Switch every rail off so the other sensors don't see it; retry later.
  if (live.busV > cfg.busHighV) {
    if (!live.backfeed) live.backfeedMs = now;
    live.backfeed = true;
  } else if (live.backfeed && now - live.backfeedMs > cfg.backfeedRetryS * 1000UL) {
    live.backfeed = false;
  }
  uint8_t st = 0xFF;
  if (rd8(EXP_ADDR, 0x00, st)) { live.expOk = true; live.stFlags = st; }
  else live.expOk = false;
  for (uint8_t i = 0; i < NUM_CH; i++) {
    live.railV[i] = mv(PIN_RMON[i], DIV_RAIL);
    const bool want = cfg.ch[i].railOn && !live.backfeed && !live.latched[i];
    const bool wasOn = live.railEn[i];
    driveRail(i, want);
    bool bad = false;
    // railV was read before this pass drove the rail: a rail switched on just now still reads 0 V, so it is judged from
    // the next pass (with a 0 ms fault hold every rail would otherwise fault - and latch - at power-up / after back-feed)
    if (live.railEn[i] && wasOn && !live.no12v) {
      const bool stLow = live.expOk && !(st & (1 << i));
      bad = stLow || live.railV[i] < cfg.railLowV;
    }
    if (bad) {
      if (!live.faultSince[i]) live.faultSince[i] = now ? now : 1;
      if (now - live.faultSince[i] >= cfg.faultHoldMs) {
        live.railFault[i] = true;
        if (cfg.faultPolicy == FAULT_LATCH) { live.latched[i] = true; driveRail(i, false); }
      }
    } else if (!live.latched[i]) {
      live.faultSince[i] = 0;
      live.railFault[i] = false;
    }
  }
}

// ---------------------------------------------------------------- public
void ioBegin() {
  for (uint8_t i = 0; i < NUM_CH; i++) {
    pinMode(PIN_EN[i], OUTPUT);
    digitalWrite(PIN_EN[i], LOW);
    raw[i] = NAN;
    live.value[i] = NAN;
  }
  pinMode(PIN_DIAG_EN, OUTPUT);
  digitalWrite(PIN_DIAG_EN, HIGH);
  pinMode(PIN_EXP_INT, INPUT);
  pinMode(PIN_STAT_LED, OUTPUT);
  applyTermination();
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  wr8(EXP_ADDR, 0x02, 0x00); // TCA9554: no polarity inversion
  wr8(EXP_ADDR, 0x03, 0xFF); // all inputs (ST# flags)
  railsTick();
}

void ioLoop() {
  // ADS1115 state machine: 860 SPS = 1.16 ms conversion, give it 1.4 ms
  if (!adsBusy) adsStart();
  else if (micros() - adsStartUs >= 1400) adsCollect();

  static uint32_t tRail = 0, tConv = 0;
  const uint32_t now = millis();
  if (now - tRail >= 20 || digitalRead(PIN_EXP_INT) == LOW) {
    tRail = now;
    railsTick();
  }
  if (now - tConv >= 5) {
    tConv = now;
    for (uint8_t i = 0; i < NUM_CH; i++) {
      const ChCfg &c = cfg.ch[i];
      live.pinV[i] = raw[i];
      const bool railOk = live.railEn[i] && !live.railFault[i];
      float v = isfinite(raw[i]) ? convert(i, raw[i], live.railV[i]) : NAN;
      const bool needsRail = c.ratiometric || c.mode == MODE_NTC;
      live.valid[i] = isfinite(v) && (!needsRail || railOk);
      if (!live.valid[i]) continue;
      if (!isfinite(live.value[i]) || c.smoothing <= 1) live.value[i] = v;
      else live.value[i] += (v - live.value[i]) / c.smoothing;
    }
  }
  // status LED: slow blink = running, fast = any fault, solid off = no ADC
  bool fault = live.backfeed || !live.expOk;
  for (uint8_t i = 0; i < NUM_CH; i++) fault |= live.railFault[i];
  const bool adcs = live.adcOk[0] || live.adcOk[1];
  live.led = adcs && ((now / (fault ? 100 : 500)) & 1);
  digitalWrite(PIN_STAT_LED, live.led);
}
