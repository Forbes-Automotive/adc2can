// adc2can - CAN (TWAI). 29-bit extended ids so it can share a car bus (and the can2gauge chain) without clashing with
// any 11-bit VAG / OBD frame. id = canBaseId + boardId * 16 + n   (default base 0x0AD2C000)
//
//  n=0  CH1 value | CH2 value      float32 big-endian, engineering units (name / unit come from n=5)
//  n=1  CH3 | CH4
//  n=2  CH5 | CH6
//  n=3  CH7 | CH8
//  n=4  status: [0] valid mask  [1] rail-on mask  [2] rail-fault mask  [3] out-of-range mask (outside low/high warn)
//               [4] flags: b0 sensor-bus back-feed, b1 VBAT low, b2 VBAT high, b3 ADS1115 #1 fail, b4 ADS1115 #2 fail,
//                   b5 TCA9554 fail, b6 no 12 V (USB power only)
//               [5] sensor bus /50 mV  [6..7] VBAT mV (uint16 BE)
//  n=5  descriptor (one per status period, cycles 8 ch x 3 parts): [0] = ch << 4 | part, [1..7] = 7 ASCII chars
//               part 0 = name chars 0-6, part 1 = name chars 7-13, part 2 = unit (7 chars). Unused chars = 0.
//  n=6  CH1-4 connector voltage, uint16 mV BE
//  n=7  CH5-8 connector voltage
//  n=8, 9, 12, 13: the config, shared and editable over CAN - see "config over CAN" below
// A value frame carries NaN for a channel that is not valid (rail off / fault / ADC fail), so a receiver can go stale.
// 1.3: every frame sent here also goes out over ESP-NOW, unchanged (adc2can_now.cpp, include/adcnow_proto.h).
#include "adc2can_defs.h"
#include "driver/twai.h"

static twai_timing_config_t timing(uint16_t kbps) {
  switch (kbps) {
    case 125: return TWAI_TIMING_CONFIG_125KBITS();
    case 250: return TWAI_TIMING_CONFIG_250KBITS();
    case 1000: return TWAI_TIMING_CONFIG_1MBITS();
    default: return TWAI_TIMING_CONFIG_500KBITS();
  }
}

// ---------------------------------------------------------------- config over CAN (protocol 2)
// Every channel's settings, so a can2gauge shows - and can edit - exactly what is set here:
//  n=8  config part, adc2can -> bus: [0] ch 0-7  [1] part  [2..7] payload (floats: f32 BE in [2..5])
//         0 [2] mode [3] decimals [4] flags (b0 rail on, b1 ratiometric) [5] smoothing
//         1 scale  2 offset  3 low warn  4 high warn  5 gain trim  6 NTC R25  7 NTC beta  8 NTC pull-up
//         9-16 table volts 0-7, 17-24 table values 0-7 (NaN = point unused)
//         25-27 name chars 0-5 / 6-11 / 12-14   28-29 unit chars 0-5 / 6
//         1.4: 30 dial min  31 dial max - the range in use (dialRange), with [6] = 1 when set by hand, 0 = Auto.
//         Set (n=13) with a number, or NaN for Auto. A gauge on 1.3's 30 parts never asks for them.
//  n=9  info, adc2can -> bus: [0..3] config revision u32 BE  [4] fw major  [5] fw minor  [6] protocol  [7] parts
//  n=12 request, gauge -> adc2can: [0] 1 = send the config of channel [1] (0xFF = all), 2 = clear a latched rail fault
//         of channel [1] (0xFF = all)
//  n=13 set, gauge -> adc2can: same layout as n=8. Applied, saved (debounced) and echoed back as n=8.
// The whole config is also cycled out slowly (one part per status period) and sent in full after every change.
static void putF(uint8_t *d, float v);
static void putF6(uint8_t *d, float v) { putF(d + 2, v); d[6] = d[7] = 0; }
static float getF6(const uint8_t *d) {
  const uint32_t u = ((uint32_t)d[2] << 24) | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 8) | d[5];
  float v;
  memcpy(&v, &u, 4);
  return v;
}

// ---------------------------------------------------------------- 1.4: the dial range
#define PLAIN_VOLTS_MAX 5.5f // the top of a plain-volts dial: 0-5 V sensors, a little headroom

// works out a dial range from a sensor's scaling: plain volts 0-5.5 V; linear over the sender's span (0.5-4.5 V
// ratiometric, else 0-5 V); a table's values; -20..150 for an NTC. Then rounded out to a tidy scale (steps of 1 / 2 /
// 2.5 / 5 / 10 / 20 x a power of ten, at most 8 of them), the one that adds the least to the sensor's span.
static void autoRange(const ChCfg &c, float &low, float &high) {
  // plain volts (linear, scale 1, offset 0 - the factory setting): the whole 0-5 V input
  if (c.mode == MODE_LINEAR && c.scale == 1.0f && c.offset == 0.0f) {
    low = 0.0f;
    high = PLAIN_VOLTS_MAX;
    return;
  }
  float lo = 0.0f;
  float hi = 1.0f;
  if (c.mode == MODE_TABLE && isfinite(c.tblV[0]) && isfinite(c.tblVal[0])) {
    lo = c.tblVal[0];
    hi = c.tblVal[0];
    for (uint8_t k = 1; k < TABLE_PTS; k++) {
      if (!isfinite(c.tblV[k]) || !isfinite(c.tblVal[k])) {
        break; // the rest of the table is unused
      }
      if (c.tblVal[k] < lo) lo = c.tblVal[k];
      if (c.tblVal[k] > hi) hi = c.tblVal[k];
    }
  } else if (c.mode == MODE_NTC) {
    lo = -20.0f;
    hi = 150.0f;
  } else {
    float bottomVolts = 0.0f; // the sender's span
    float topVolts = 5.0f;
    if (c.ratiometric) {
      bottomVolts = 0.5f;
      topVolts = 4.5f;
    }
    const float atBottom = bottomVolts * c.scale + c.offset;
    const float atTop = topVolts * c.scale + c.offset;
    lo = atBottom;
    hi = atTop;
    if (atTop < atBottom) { // a falling sensor (negative scale)
      lo = atTop;
      hi = atBottom;
    }
  }
  float span = hi - lo;
  if (span <= 0.0f) {
    span = 1.0f;
  }
  const float power = powf(10.0f, floorf(log10f(span / 5.0f)));
  const float multiples[] = {1.0f, 2.0f, 2.5f, 5.0f, 10.0f, 20.0f};
  bool found = false;
  for (uint8_t m = 0; m < 6; m++) {
    const float step = multiples[m] * power;
    if (span / step > 8.0f) {
      continue; // too many divisions
    }
    const float tryLow = floorf(lo / step + 1e-6f) * step;
    const float tryHigh = ceilf(hi / step - 1e-6f) * step;
    if (!found || tryHigh - tryLow < high - low - 1e-6f) {
      low = tryLow;
      high = tryHigh;
      found = true;
    }
  }
  if (!found || high <= low) {
    low = lo;
    high = lo + span;
  }
}

void dialRange(uint8_t ch, float &low, float &high) {
  autoRange(cfg.ch[ch], low, high);
  if (isfinite(cfg.dialMin[ch])) {
    low = cfg.dialMin[ch]; // set by hand
  }
  if (isfinite(cfg.dialMax[ch])) {
    high = cfg.dialMax[ch];
  }
  if (high <= low) { // a hand-set end on the wrong side: back to Auto
    autoRange(cfg.ch[ch], low, high);
  }
}

static void cfgPart(uint8_t ch, uint8_t part, uint8_t *d) {
  const ChCfg &c = cfg.ch[ch];
  memset(d, 0, 8);
  d[0] = ch;
  d[1] = part;
  if (part == 0) {
    d[2] = c.mode; d[3] = c.decimals; d[4] = (c.railOn ? 1 : 0) | (c.ratiometric ? 2 : 0); d[5] = c.smoothing;
  } else if (part <= 8) {
    const float v[] = {0, c.scale, c.offset, c.lowWarn, c.highWarn, c.trim, c.ntcR25, c.ntcBeta, c.ntcPullup};
    putF6(d, v[part]);
  } else if (part <= 16) putF6(d, c.tblV[part - 9]);
  else if (part <= 24) putF6(d, c.tblVal[part - 17]);
  else if (part == 30 || part == 31) { // 1.4: the dial range in use, [6] = 1 when set by hand
    float low = 0;
    float high = 0;
    dialRange(ch, low, high);
    if (part == 30) {
      putF6(d, low);
      d[6] = isfinite(cfg.dialMin[ch]) ? 1 : 0;
    } else {
      putF6(d, high);
      d[6] = isfinite(cfg.dialMax[ch]) ? 1 : 0;
    }
  } else {
    const char *s = part <= 27 ? c.name + (part - 25) * 6 : c.unit + (part - 28) * 6;
    const size_t room = part <= 27 ? sizeof(c.name) - (part - 25) * 6 : sizeof(c.unit) - (part - 28) * 6;
    for (uint8_t k = 0; k < 6 && k < room - 1 && s[k]; k++) d[2 + k] = s[k];
  }
}

static bool applyPart(uint8_t ch, uint8_t part, const uint8_t *d) {
  if (ch >= NUM_CH || part >= CFG_PARTS) return false;
  ChCfg &c = cfg.ch[ch];
  if (part == 0) {
    c.mode = d[2] <= MODE_NTC ? d[2] : c.mode;
    c.decimals = d[3] <= 4 ? d[3] : c.decimals;
    c.railOn = d[4] & 1;
    c.ratiometric = (d[4] >> 1) & 1;
    c.smoothing = constrain(d[5], 1, 250);
    return true;
  }
  if (part == 30 || part == 31) { // 1.4: Dial Min / Max - a number, or NaN for Auto
    const float v = getF6(d);
    if (part == 30) {
      cfg.dialMin[ch] = v;
    } else {
      cfg.dialMax[ch] = v;
    }
    return true;
  }
  if (part <= 24) {
    const float v = getF6(d);
    switch (part) {
      case 1: c.scale = v; break;
      case 2: c.offset = v; break;
      case 3: c.lowWarn = isfinite(v) ? v : -1e9f; break;
      case 4: c.highWarn = isfinite(v) ? v : 1e9f; break;
      case 5: c.trim = constrain(v, 0.8f, 1.2f); break;
      case 6: c.ntcR25 = v; break;
      case 7: c.ntcBeta = v; break;
      case 8: c.ntcPullup = v; break;
      default:
        if (part <= 16) c.tblV[part - 9] = v;
        else c.tblVal[part - 17] = v;
    }
    return true;
  }
  char *s = part <= 27 ? c.name + (part - 25) * 6 : c.unit + (part - 28) * 6;
  const size_t room = part <= 27 ? sizeof(c.name) - (part - 25) * 6 : sizeof(c.unit) - (part - 28) * 6;
  for (uint8_t k = 0; k < 6 && k < room - 1; k++) s[k] = d[2 + k];
  (part <= 27 ? c.name : c.unit)[part <= 27 ? sizeof(c.name) - 1 : sizeof(c.unit) - 1] = 0;
  return true;
}

#define TX_QUEUE 16
static uint8_t dumpMask = 0, dumpCh = 0, dumpPart = 0; // channels still to send in full
static uint32_t saveAt = 0;                             // debounced save after edits over CAN

void cfgChanged() {
  live.cfgRev++;
  dumpMask = 0xFF;
}

static bool send(uint8_t n, const uint8_t *d);
static uint32_t frameId(uint8_t n);
static void canRx(const twai_message_t &m) {
  if (!m.extd || m.data_length_code < 8) return;
  if (m.identifier == frameId(12) || m.identifier == frameId(13)) { live.canRx++; live.canRxMs = millis(); }
  if (m.identifier == frameId(12)) {
    if (m.data[0] == 1) dumpMask |= m.data[1] == 0xFF ? 0xFF : (1 << (m.data[1] & 7));
    else if (m.data[0] == 2) railReset(m.data[1]);
  } else if (m.identifier == frameId(13)) {
    if (applyPart(m.data[0], m.data[1], m.data)) {
      uint8_t d[8];
      cfgPart(m.data[0], m.data[1], d);
      send(8, d);          // echo: the gauge sees it took
      // 1.4: a scaling change moves an Auto dial range too: send the range in use again
      if (m.data[1] != 30 && m.data[1] != 31) {
        cfgPart(m.data[0], 30, d);
        send(8, d);
        cfgPart(m.data[0], 31, d);
        send(8, d);
      }
      live.cfgRev++;
      saveAt = millis() + 1000;
    }
  }
}

// up to `budget` frames of the pending full dump
// Only into free TX queue space, with room kept for a value + status burst (10 frames): canLoop() runs every few
// microseconds and the bus drains a frame every ~300 us at 500 kbit/s, so sending blindly would drop most of the 240
// frames (counted as send errors) and the gauge would only get the config from the slow background cycle. A part that
// does not fit now goes on a later pass.
static void sendDump(uint8_t budget) {
  if (!dumpMask) return;
  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK) return;
  int room = TX_QUEUE - 10 - (int)st.msgs_to_tx;
  while (dumpMask && budget-- && room-- > 0) {
    while (!(dumpMask & (1 << dumpCh))) dumpCh = (dumpCh + 1) % NUM_CH;
    uint8_t d[8];
    cfgPart(dumpCh, dumpPart, d);
    if (!send(8, d)) break;
    if (++dumpPart >= CFG_PARTS) {
      dumpPart = 0;
      dumpMask &= ~(1 << dumpCh);
      dumpCh = (dumpCh + 1) % NUM_CH;
    }
  }
}

static void sendInfo() {
  uint8_t d[8] = {0};
  d[0] = live.cfgRev >> 24; d[1] = live.cfgRev >> 16; d[2] = live.cfgRev >> 8; d[3] = live.cfgRev;
  d[4] = FW_MAJOR; d[5] = FW_MINOR; d[6] = CFG_PROTO; d[7] = CFG_PARTS;
  send(9, d);
  static uint16_t bg = 0; // the slow background cycle of every part (1.4: 8 x 32 = 256 parts, past a uint8_t)
  uint8_t e[8];
  cfgPart(bg / CFG_PARTS, bg % CFG_PARTS, e);
  send(8, e);
  bg = (bg + 1) % (NUM_CH * CFG_PARTS);
}

static void canStart() {
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)PIN_CAN_TX, (gpio_num_t)PIN_CAN_RX, TWAI_MODE_NORMAL);
  g.tx_queue_len = TX_QUEUE;
  g.rx_queue_len = 64; // the whole car bus comes in (the CAN frame relay): the RX task drains it
  twai_timing_config_t t = timing(cfg.canKbps);
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  live.canUp = twai_driver_install(&g, &t, &f) == ESP_OK && twai_start() == ESP_OK;
}

// RX task: every frame received goes to the CAN frame relay (adc2can_now.cpp, when on); config requests / edits from a
// gauge (n 12 / 13) go on to canLoop(), where cfg belongs.
static QueueHandle_t cfgRxQ = NULL;
static volatile bool rxParked = true; // not inside twai_receive: safe to uninstall the driver
static void rxTask(void *) {
  twai_message_t m;
  for (;;) {
    if (!live.canUp) {
      rxParked = true;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    rxParked = false;
    if (twai_receive(&m, pdMS_TO_TICKS(50)) != ESP_OK) continue;
    nowTap(m);
    if (m.extd && (m.identifier == frameId(12) || m.identifier == frameId(13))) xQueueSend(cfgRxQ, &m, 0);
  }
}

void canBegin() {
  live.cfgRev = esp_random(); // a new number every boot: gauges re-read the config after a power cycle
  canStart();
  cfgRxQ = xQueueCreate(8, sizeof(twai_message_t));
  xTaskCreatePinnedToCore(rxTask, "canRx", 3072, NULL, 5, NULL, 0);
}

void canRestart() {
  if (live.canUp) {
    live.canUp = false; // the RX task leaves twai_receive (50 ms timeout) and parks
    const uint32_t t0 = millis();
    while (!rxParked && millis() - t0 < 200) delay(5);
    twai_stop();
    twai_driver_uninstall();
  }
  canStart();
  dumpMask = 0xFF; // gauges on the new bit rate get the whole config
}

// the last frame sent at each offset, for the web UI's CAN tab
static uint8_t lastD[10][8];
static uint32_t lastN[10];
bool canLastFrame(uint8_t n, uint32_t &id, uint8_t *d, uint32_t &count) {
  if (n >= 10) return false;
  id = frameId(n);
  memcpy(d, lastD[n], 8);
  count = lastN[n];
  return count > 0;
}

static uint32_t frameId(uint8_t n) { return (cfg.canBaseId + (uint32_t)(cfg.boardId & 3) * 16 + n) & 0x1FFFFFFF; }

static bool send(uint8_t n, const uint8_t *d) {
  nowQueue(n, d); // 1.3: the same frame over ESP-NOW (adc2can_now.cpp) - whether or not a CAN bus is connected
  if (!live.canUp) return false;
  twai_message_t m = {};
  m.identifier = frameId(n);
  m.extd = 1;
  m.data_length_code = 8;
  memcpy(m.data, d, 8);
  if (n < 10) { memcpy(lastD[n], d, 8); lastN[n]++; }
  if (twai_transmit(&m, 0) == ESP_OK) { live.canTx++; return true; }
  live.canErr++;
  return false;
}

static void putF(uint8_t *d, float v) {
  uint32_t u;
  memcpy(&u, &v, 4);
  d[0] = u >> 24; d[1] = u >> 16; d[2] = u >> 8; d[3] = u;
}
static void put16(uint8_t *d, uint16_t v) { d[0] = v >> 8; d[1] = v & 0xFF; }
static uint16_t mv16(float v) { return !isfinite(v) || v < 0 ? 0 : (v > 65.535f ? 65535 : (uint16_t)(v * 1000.0f + 0.5f)); }

static void sendValues() {
  uint8_t d[8];
  for (uint8_t f = 0; f < 4; f++) {
    for (uint8_t k = 0; k < 2; k++) {
      const uint8_t i = f * 2 + k;
      putF(&d[k * 4], live.valid[i] ? live.value[i] : NAN);
    }
    send(f, d);
  }
  for (uint8_t f = 0; f < 2; f++) {
    for (uint8_t k = 0; k < 4; k++) put16(&d[k * 2], mv16(live.pinV[f * 4 + k]));
    send(6 + f, d);
  }
}

static void sendStatus() {
  uint8_t d[8] = {0};
  for (uint8_t i = 0; i < NUM_CH; i++) {
    if (live.valid[i]) d[0] |= 1 << i;
    if (live.railEn[i]) d[1] |= 1 << i;
    if (live.railFault[i]) d[2] |= 1 << i;
    if (live.valid[i] && (live.value[i] < cfg.ch[i].lowWarn || live.value[i] > cfg.ch[i].highWarn)) d[3] |= 1 << i;
  }
  d[4] = (live.backfeed ? 0x01 : 0) | (live.vbat < cfg.vbatLow ? 0x02 : 0) | (live.vbat > cfg.vbatHigh ? 0x04 : 0) |
         (live.adcOk[0] ? 0 : 0x08) | (live.adcOk[1] ? 0 : 0x10) | (live.expOk ? 0 : 0x20) | (live.no12v ? 0x40 : 0);
  const float bus = live.busV / 0.05f;
  d[5] = bus < 0 ? 0 : (bus > 255 ? 255 : (uint8_t)bus);
  put16(&d[6], mv16(live.vbat));
  send(4, d);

  static uint8_t desc = 0; // ch * 3 + part
  const uint8_t ch = desc / 3, part = desc % 3;
  uint8_t e[8] = {0};
  e[0] = (ch << 4) | part;
  const char *src = part == 2 ? cfg.ch[ch].unit : cfg.ch[ch].name + part * 7;
  const size_t lim = part == 2 ? sizeof(cfg.ch[ch].unit) : (part == 0 ? sizeof(cfg.ch[ch].name) : sizeof(cfg.ch[ch].name) - 7);
  for (uint8_t k = 0; k < 7 && k < lim && src[k]; k++) e[1 + k] = src[k];
  send(5, e);
  desc = (desc + 1) % (NUM_CH * 3);
}

void canLoop() {
  if (live.canUp) {
    twai_message_t m;
    while (xQueueReceive(cfgRxQ, &m, 0) == pdTRUE) canRx(m); // config requests / edits from a gauge (from the RX task)
    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK) {
      live.canState = (uint8_t)st.state;
      live.canTec = st.tx_error_counter > 255 ? 255 : st.tx_error_counter;
      live.canRec = st.rx_error_counter > 255 ? 255 : st.rx_error_counter;
      if (st.state == TWAI_STATE_BUS_OFF) twai_initiate_recovery();
      else if (st.state == TWAI_STATE_STOPPED) twai_start();
    }
  }
  // 1.3: the frames are built even without a CAN driver - ESP-NOW carries them too
  static uint32_t tV = 0, tS = 0;
  const uint32_t now = millis();
  if (now - tV >= cfg.txPeriodMs) { tV = now; sendValues(); }
  if (now - tS >= cfg.statusPeriodMs) { tS = now; sendStatus(); sendInfo(); }
  if (live.canUp) sendDump(4); // a few frames of a pending full config each pass (paced by the TWAI queue; over
                               // ESP-NOW alone the slow background cycle in sendInfo() delivers the config)
  if (saveAt && (int32_t)(now - saveAt) >= 0) { saveAt = 0; settingsSave(); }
  nowFlush(); // everything this pass sent: one ESP-NOW packet
}
