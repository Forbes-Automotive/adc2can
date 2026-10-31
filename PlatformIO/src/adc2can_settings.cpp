// adc2can - settings (NVS via Preferences). Every field here is editable on the web UI.
#include "adc2can_defs.h"
#include <Preferences.h>

static const uint32_t MAGIC = 0xADC2CA02; // bump when Settings changes shape

void settingsDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = MAGIC;
  for (uint8_t i = 0; i < NUM_CH; i++) {
    ChCfg &c = cfg.ch[i];
    snprintf(c.name, sizeof(c.name), "ADC %u", i + 1);
    strlcpy(c.unit, "V", sizeof(c.unit));
    c.mode = MODE_LINEAR;
    c.railOn = 1;
    c.ratiometric = 1;
    c.smoothing = 4;
    c.decimals = 2;
    c.scale = 1.0f;
    c.offset = 0.0f;
    c.trim = 1.0f;
    for (uint8_t k = 0; k < TABLE_PTS; k++) { c.tblV[k] = NAN; c.tblVal[k] = NAN; }
    c.ntcR25 = 2000.0f;   // Bosch 0 280 130 026 style coolant / IAT NTC
    c.ntcBeta = 3500.0f;
    c.ntcPullup = 2200.0f; // R_PUn when fitted
    c.lowWarn = -1e9f;
    c.highWarn = 1e9f;
  }
  cfg.faultPolicy = FAULT_RETRY;
  cfg.faultHoldMs = 200;
  cfg.backfeedRetryS = 5;
  cfg.railLowV = 4.50f;
  cfg.busHighV = 5.80f;
  cfg.vbatLow = 11.0f;
  cfg.vbatHigh = 15.5f;
  cfg.no12vBelow = 7.0f;
  cfg.canKbps = 500;
  cfg.boardId = 0;
  cfg.canTerm = 0;
  cfg.canBaseId = 0x0AD2C000; // 29-bit: cannot collide with any VAG 11-bit frame on a shared bus
  cfg.txPeriodMs = 20;        // 50 Hz values
  cfg.statusPeriodMs = 100;
  uint64_t mac = ESP.getEfuseMac();
  snprintf(cfg.apSsid, sizeof(cfg.apSsid), "adc2can-%04X", (uint16_t)(mac >> 32));
  cfg.apPass[0] = 0; // open access point: no Forbes Automotive product ships with a WiFi password
  cfg.espNow = 1;    // 1.3: costs nothing, and lets a gauge with power only (no CAN) show the sensors
  // 1.4: every dial range on Auto (worked out from the sensor's scaling - dialRange in adc2can_can.cpp)
  for (uint8_t i = 0; i < NUM_CH; i++) {
    cfg.dialMin[i] = NAN;
    cfg.dialMax[i] = NAN;
  }
}

// 1.2 settings end where espNow starts (4-aligned, so the 1.2 blob is exactly that long): load it over the defaults and
// the 1.3 fields keep theirs. MAGIC stays the same for an append; bump it only when the old fields move.
static_assert(offsetof(Settings, espNow) % 4 == 0, "settings appended after a 4-aligned 1.2 layout");
// 1.4: the same again - a 1.3 blob ends where dialMin starts, and loads with every dial range on Auto
static_assert(offsetof(Settings, dialMin) % 4 == 0, "settings appended after a 4-aligned 1.3 layout");

void settingsLoad() {
  Preferences p;
  p.begin("adc2can", true);
  const size_t len = p.getBytesLength("cfg");
  settingsDefaults();
  bool ok = false;
  if (len == sizeof(cfg) || len == offsetof(Settings, espNow) || len == offsetof(Settings, dialMin)) {
    p.getBytes("cfg", &cfg, len);
    ok = cfg.magic == MAGIC;
  }
  p.end();
  if (!ok) settingsDefaults();
  else if (!strcmp(cfg.apPass, "adc2can123")) cfg.apPass[0] = 0; // the old firmware's default password: open it up
  if (cfg.espNow > 1) cfg.espNow = 1;
  if (cfg.canRelay >= CANN_R_MAX) cfg.canRelay = CANN_R_OFF;
}

void settingsSave() {
  Preferences p;
  p.begin("adc2can", false);
  p.putBytes("cfg", &cfg, sizeof(cfg));
  p.end();
}
