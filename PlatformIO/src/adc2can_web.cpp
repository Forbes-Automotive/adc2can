// adc2can - WiFi + web UI, on the shared Forbes Automotive modules (_SharedTheme v2): wifi_manager brings the access
// point, mDNS and the Home WiFi bridge up and serves the UI from LittleFS (data/, or the recovery page when it holds no
// usable UI); ota_manager does the updates (adc2can_ota.cpp). This file is the JSON API the UI (data/app.js) talks to.
//
//   GET  /api/status      live values, rails, power, CAN, WiFi (the UI polls it every 500 ms)
//   GET  /api/settings    every setting (also the settings backup)
//   POST /api/settings    any part of it: {"ch":[{"i":2,"name":"Oil"}], "can":{"kbps":500}, ...} (also a backup restore)
//   POST /api/action      {"action":"reset","ch":255} clear latched rail faults | "reboot" | "factory"
//   GET  /api/can/frames  the last frame sent at each offset (CAN tab)
//
// The async server runs on its own task: a POST is parsed into a pending copy of the settings and handed to loop(),
// which owns cfg (the I/O and CAN code read it there).
#include "adc2can_defs.h"
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ota_manager.h>
#include <wifi_manager.h>

AsyncWebServer web(80);

static SemaphoreHandle_t pendMx;
static Settings pend;           // settings posted from the UI, waiting for loop()
static bool pendDirty = false;
static volatile uint8_t pendAction = 0; // 1 reboot, 2 factory reset
static uint32_t actionAt = 0;
static char mdnsName[12];

// ---------------------------------------------------------------- JSON helpers
// Put a float into a JSON object under "key", rounded to dp decimal places. No reading (NaN) goes out as null.
// It takes the object and the key name separately on purpose: ArduinoJson only creates a key when you assign to it,
// so handing a helper obj["key"] (as up to 1.3.0) gave it nowhere to write and the value was silently left out.
static void putF(JsonObject obj, const char *key, float f, int dp = 4) {
  if (!isfinite(f)) {
    obj[key] = nullptr; // no reading
    return;
  }
  const float m = powf(10.0f, dp);
  obj[key] = roundf(f * m) / m;
}

// The same, adding the float as the next item of a JSON array (the table points)
static void addF(JsonArray arr, float f, int dp = 4) {
  if (!isfinite(f)) {
    arr.add(nullptr); // no reading
    return;
  }
  const float m = powf(10.0f, dp);
  arr.add(roundf(f * m) / m);
}
static float getF(JsonVariantConst v, float d) { return v.is<float>() || v.is<int>() ? v.as<float>() : d; }
static void getS(JsonVariantConst v, char *dst, size_t len) { if (v.is<const char *>()) strlcpy(dst, v.as<const char *>(), len); }
static bool has(JsonObjectConst o, const char *key) { // present, even as null (null = "none")
  for (JsonPairConst kv : o)
    if (kv.key() == key) return true;
  return false;
}

static void sendJson(AsyncWebServerRequest *r, int code, const JsonDocument &d) {
  String out;
  serializeJson(d, out);
  AsyncWebServerResponse *res = r->beginResponse(code, "application/json", out);
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}
static void sendStatus(AsyncWebServerRequest *r, int code, const char *msg) {
  JsonDocument d;
  d[code == 200 ? "status" : "error"] = msg;
  sendJson(r, code, d);
}

// collect a JSON body across chunks then call done(request, body) - as can2gauge_API.cpp
static void parseJSON(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total,
                      void (*done)(AsyncWebServerRequest *, const String &)) {
  String *body = (String *)request->_tempObject;
  if (index == 0) {
    body = new String();
    body->reserve(total);
    request->_tempObject = body;
  }
  body->concat((const char *)data, len);
  if (index + len == total) {
    done(request, *body);
    delete body;
    request->_tempObject = NULL;
  }
}

// ---------------------------------------------------------------- settings <-> JSON
static void chToJson(const ChCfg &c, JsonObject o) {
  o["name"] = c.name;
  o["unit"] = c.unit;
  o["mode"] = c.mode;
  o["railOn"] = c.railOn;
  o["ratiometric"] = c.ratiometric;
  o["smoothing"] = c.smoothing;
  o["decimals"] = c.decimals;
  putF(o, "scale", c.scale, 6);
  putF(o, "offset", c.offset, 6);
  putF(o, "trim", c.trim, 4);
  JsonArray t = o["table"].to<JsonArray>();
  for (uint8_t k = 0; k < TABLE_PTS && isfinite(c.tblV[k]) && isfinite(c.tblVal[k]); k++) {
    JsonArray p = t.add<JsonArray>();
    addF(p, c.tblV[k], 4);
    addF(p, c.tblVal[k], 4);
  }
  putF(o, "ntcR25", c.ntcR25, 1);
  putF(o, "ntcBeta", c.ntcBeta, 1);
  putF(o, "ntcPullup", c.ntcPullup, 1);
  putF(o, "lowWarn", c.lowWarn > -1e8f ? c.lowWarn : NAN);
  putF(o, "highWarn", c.highWarn < 1e8f ? c.highWarn : NAN);
}

static void chFromJson(ChCfg &c, JsonObjectConst o) {
  getS(o["name"], c.name, sizeof(c.name));
  getS(o["unit"], c.unit, sizeof(c.unit));
  if (o["mode"].is<int>()) c.mode = constrain(o["mode"].as<int>(), 0, 2);
  if (!o["railOn"].isNull()) c.railOn = o["railOn"].as<int>() ? 1 : 0;
  if (!o["ratiometric"].isNull()) c.ratiometric = o["ratiometric"].as<int>() ? 1 : 0;
  if (o["smoothing"].is<int>()) c.smoothing = constrain(o["smoothing"].as<int>(), 1, 250);
  if (o["decimals"].is<int>()) c.decimals = constrain(o["decimals"].as<int>(), 0, 4);
  c.scale = getF(o["scale"], c.scale);
  c.offset = getF(o["offset"], c.offset);
  c.trim = constrain(getF(o["trim"], c.trim), 0.8f, 1.2f);
  if (o["table"].is<JsonArrayConst>()) {
    for (uint8_t k = 0; k < TABLE_PTS; k++) { c.tblV[k] = NAN; c.tblVal[k] = NAN; }
    uint8_t n = 0;
    for (JsonArrayConst p : o["table"].as<JsonArrayConst>()) {
      if (n >= TABLE_PTS || p.size() != 2 || !(p[0].is<float>() || p[0].is<int>()) || !(p[1].is<float>() || p[1].is<int>())) continue;
      c.tblV[n] = p[0].as<float>();
      c.tblVal[n] = p[1].as<float>();
      n++;
    }
    // keep it ascending in volts so the lookup can walk it
    for (uint8_t a = 0; a < n; a++)
      for (uint8_t b = a + 1; b < n; b++)
        if (c.tblV[b] < c.tblV[a]) { float t = c.tblV[a]; c.tblV[a] = c.tblV[b]; c.tblV[b] = t; t = c.tblVal[a]; c.tblVal[a] = c.tblVal[b]; c.tblVal[b] = t; }
  }
  c.ntcR25 = getF(o["ntcR25"], c.ntcR25);
  c.ntcBeta = getF(o["ntcBeta"], c.ntcBeta);
  c.ntcPullup = getF(o["ntcPullup"], c.ntcPullup);
  if (o["lowWarn"].isNull() && has(o, "lowWarn")) c.lowWarn = -1e9f; // null = no warning
  else c.lowWarn = getF(o["lowWarn"], c.lowWarn);
  if (o["highWarn"].isNull() && has(o, "highWarn")) c.highWarn = 1e9f;
  else c.highWarn = getF(o["highWarn"], c.highWarn);
}

// 1.4: sensor i's dial range into its JSON: what's set (null = Auto) and the range in use (dialRange)
static void dialToJson(uint8_t i, JsonObject o) {
  putF(o, "dialMin", cfg.dialMin[i]); // null = Auto
  putF(o, "dialMax", cfg.dialMax[i]);
  float low = 0;
  float high = 0;
  dialRange(i, low, high);
  putF(o, "dialLo", low);  // the dial's bottom as shown (and sent to can2gauge)
  putF(o, "dialHi", high); // the dial's top
}

// 1.4: sensor i's Dial Min / Max from the web UI: a number, or null / blank for Auto
static void dialFromJson(Settings &s, uint8_t i, JsonObjectConst o) {
  if (has(o, "dialMin")) {
    s.dialMin[i] = getF(o["dialMin"], NAN); // null = Auto
  }
  if (has(o, "dialMax")) {
    s.dialMax[i] = getF(o["dialMax"], NAN);
  }
}

static void settingsToJson(JsonDocument &d) {
  d["version"] = FW_VERSION;
  JsonArray ch = d["ch"].to<JsonArray>();
  for (uint8_t i = 0; i < NUM_CH; i++) {
    JsonObject channel = ch.add<JsonObject>();
    chToJson(cfg.ch[i], channel);
    dialToJson(i, channel); // 1.4
  }
  JsonObject r = d["rails"].to<JsonObject>();
  r["faultPolicy"] = cfg.faultPolicy;
  r["faultHoldMs"] = cfg.faultHoldMs;
  r["backfeedRetryS"] = cfg.backfeedRetryS;
  putF(r, "railLowV", cfg.railLowV, 2);
  putF(r, "busHighV", cfg.busHighV, 2);
  JsonObject p = d["power"].to<JsonObject>();
  putF(p, "vbatLow", cfg.vbatLow, 2);
  putF(p, "vbatHigh", cfg.vbatHigh, 2);
  putF(p, "no12vBelow", cfg.no12vBelow, 2);
  JsonObject c = d["can"].to<JsonObject>();
  c["kbps"] = cfg.canKbps;
  c["boardId"] = cfg.boardId;
  c["term"] = cfg.canTerm;
  c["baseId"] = cfg.canBaseId;
  c["txPeriodMs"] = cfg.txPeriodMs;
  c["statusPeriodMs"] = cfg.statusPeriodMs;
  JsonObject w = d["wifi"].to<JsonObject>();
  w["apSsid"] = cfg.apSsid;
  w["apPass"] = cfg.apPass;
  w["espNow"] = cfg.espNow;
  w["canRelay"] = cfg.canRelay;
}

// -> "" or why it was refused
static const char *settingsFromJson(Settings &s, JsonObjectConst d) {
  if (d["ch"].is<JsonArrayConst>()) {
    uint8_t k = 0;
    for (JsonObjectConst o : d["ch"].as<JsonArrayConst>()) {
      const int i = o["i"].is<int>() ? o["i"].as<int>() : k;
      k++;
      if (i >= 0 && i < NUM_CH) {
        chFromJson(s.ch[i], o);
        dialFromJson(s, i, o); // 1.4
      }
    }
  }
  JsonObjectConst r = d["rails"];
  if (!r.isNull()) {
    if (r["faultPolicy"].is<int>()) s.faultPolicy = constrain(r["faultPolicy"].as<int>(), 0, 1);
    if (r["faultHoldMs"].is<int>()) s.faultHoldMs = constrain(r["faultHoldMs"].as<int>(), 0, 10000);
    if (r["backfeedRetryS"].is<int>()) s.backfeedRetryS = constrain(r["backfeedRetryS"].as<int>(), 1, 600);
    s.railLowV = constrain(getF(r["railLowV"], s.railLowV), 3.0f, 5.0f);
    s.busHighV = constrain(getF(r["busHighV"], s.busHighV), 5.2f, 12.0f);
  }
  JsonObjectConst p = d["power"];
  if (!p.isNull()) {
    s.vbatLow = getF(p["vbatLow"], s.vbatLow);
    s.vbatHigh = getF(p["vbatHigh"], s.vbatHigh);
    s.no12vBelow = constrain(getF(p["no12vBelow"], s.no12vBelow), 3.0f, 10.0f);
  }
  JsonObjectConst c = d["can"];
  if (!c.isNull()) {
    if (c["kbps"].is<int>()) {
      const int k = c["kbps"].as<int>();
      if (k != 125 && k != 250 && k != 500 && k != 1000) return "CAN speed must be 125, 250, 500 or 1000 kbit/s";
      s.canKbps = k;
    }
    if (c["boardId"].is<int>()) s.boardId = constrain(c["boardId"].as<int>(), 0, 3);
    if (!c["term"].isNull()) s.canTerm = c["term"].as<int>() ? 1 : 0;
    if (c["baseId"].is<uint32_t>()) s.canBaseId = c["baseId"].as<uint32_t>() & 0x1FFFFFFF;
    if (c["txPeriodMs"].is<int>()) s.txPeriodMs = constrain(c["txPeriodMs"].as<int>(), 5, 5000);
    if (c["statusPeriodMs"].is<int>()) s.statusPeriodMs = constrain(c["statusPeriodMs"].as<int>(), 20, 5000);
  }
  JsonObjectConst w = d["wifi"];
  if (!w.isNull()) {
    if (w["apSsid"].is<const char *>()) {
      if (!strlen(w["apSsid"].as<const char *>())) return "the access point needs a name";
      getS(w["apSsid"], s.apSsid, sizeof(s.apSsid));
    }
    if (w["apPass"].is<const char *>()) {
      const size_t n = strlen(w["apPass"].as<const char *>());
      if (n && n < 8) return "the WiFi password needs 8 characters or more (blank = open)";
      getS(w["apPass"], s.apPass, sizeof(s.apPass));
    }
    if (!w["espNow"].isNull()) s.espNow = w["espNow"].as<int>() ? 1 : 0;
    if (!w["canRelay"].isNull()) {
      const int r = w["canRelay"].as<int>();
      s.canRelay = r > 0 && r < CANN_R_MAX ? r : CANN_R_OFF;
    }
  }
  return "";
}

// ---------------------------------------------------------------- routes
static void statusOut(AsyncWebServerRequest *r) {
  JsonDocument d;
  d["version"] = FW_VERSION;
  d["uptime"] = millis() / 1000;
  d["heap"] = ESP.getFreeHeap();
  JsonArray ch = d["ch"].to<JsonArray>();
  for (uint8_t i = 0; i < NUM_CH; i++) {
    const ChCfg &c = cfg.ch[i];
    JsonObject o = ch.add<JsonObject>();
    const bool ok = live.valid[i];
    putF(o, "pin", live.pinV[i], 3);
    putF(o, "corr", live.corrV[i], 3);
    putF(o, "rail", live.railV[i], 3);
    putF(o, "v", ok ? live.value[i] : NAN, 4);
    o["ok"] = ok;
    o["warn"] = ok && (live.value[i] < c.lowWarn || live.value[i] > c.highWarn);
    o["en"] = live.railEn[i];
    o["fault"] = live.railFault[i];
    o["latched"] = live.latched[i];
  }
  JsonObject top = d.as<JsonObject>(); // the reply itself, so putF can add to it
  putF(top, "bus", live.busV, 3);
  putF(top, "vbat", live.vbat, 2);
  d["backfeed"] = live.backfeed;
  d["no12v"] = live.no12v;
  d["vbatLow"] = live.vbat < cfg.vbatLow;
  d["vbatHigh"] = live.vbat > cfg.vbatHigh;
  JsonArray adc = d["adc"].to<JsonArray>();
  adc.add(live.adcOk[0]);
  adc.add(live.adcOk[1]);
  d["exp"] = live.expOk;
  d["st"] = live.stFlags;
  d["led"] = live.led;
  d["cfgRev"] = live.cfgRev;
  d["logging"] = logRecording();
  JsonObject c = d["can"].to<JsonObject>();
  c["up"] = live.canUp;
  c["state"] = live.canState;
  c["tx"] = live.canTx;
  c["err"] = live.canErr;
  c["tec"] = live.canTec;
  c["rec"] = live.canRec;
  c["rx"] = live.canRx;
  // seconds since a gauge last talked to this board, -1 = never. A plain if / else on purpose: the inline-if version
  // mixed a whole number with -1, so "never" came out as 4294967295 seconds and the page took it as "heard".
  if (live.canRxMs) {
    c["rxAgo"] = (millis() - live.canRxMs) / 1000;
  } else {
    c["rxAgo"] = -1;
  }
  c["kbps"] = cfg.canKbps;
  c["id0"] = (cfg.canBaseId + (uint32_t)(cfg.boardId & 3) * 16) & 0x1FFFFFFF;
  JsonObject w = d["wifi"].to<JsonObject>();
  w["apClients"] = WiFi.softAPgetStationNum();
  w["ssid"] = cfg.apSsid;
  w["staConnected"] = wifiManagerStaConnected();
  w["staIP"] = wifiManagerStaConnected() ? wifiManagerStaIp() : String("");
  w["mdns"] = mdnsName;
  w["channel"] = WiFi.channel(); // a gauge must be on the same channel to hear the ESP-NOW broadcast
  w["espNow"] = live.nowUp;
  w["nowTx"] = live.nowTx;
  w["nowErr"] = live.nowErr;
  uint32_t fps, lostPs, frames, lost;
  nowRelayStats(fps, lostPs, frames, lost);
  JsonObject relay = d["canRelay"].to<JsonObject>(); // the CAN frame relay - as OpenHaldex's /api/status canRelay
  relay["rate"] = cfg.canRelay;
  relay["fps"] = fps;
  relay["lostPerSec"] = lostPs;
  relay["frames"] = frames;
  relay["lost"] = lost;
  sendJson(r, 200, d);
}

static void settingsOut(AsyncWebServerRequest *r) {
  JsonDocument d;
  settingsToJson(d);
  sendJson(r, 200, d);
}

static void settingsIn(AsyncWebServerRequest *r, const String &body) {
  JsonDocument d;
  if (deserializeJson(d, body) || !d.is<JsonObject>()) return sendStatus(r, 400, "bad JSON");
  xSemaphoreTake(pendMx, portMAX_DELAY);
  if (!pendDirty) pend = cfg;
  Settings s = pend;
  const char *why = settingsFromJson(s, d.as<JsonObjectConst>());
  if (!*why) { pend = s; pendDirty = true; }
  xSemaphoreGive(pendMx);
  if (*why) return sendStatus(r, 400, why);
  sendStatus(r, 200, "ok");
}

static void actionIn(AsyncWebServerRequest *r, const String &body) {
  JsonDocument d;
  if (deserializeJson(d, body)) return sendStatus(r, 400, "bad JSON");
  const String a = d["action"] | "";
  if (a == "reset") {
    const int ch = d["ch"] | 255;
    railReset(ch >= 1 && ch <= NUM_CH ? ch - 1 : 0xFF);
    return sendStatus(r, 200, ch >= 1 && ch <= NUM_CH ? "rail fault cleared" : "rail faults cleared");
  }
  if (a == "reboot") { pendAction = 1; return sendStatus(r, 200, "restarting"); }
  if (a == "factory") { pendAction = 2; return sendStatus(r, 200, "factory reset - restarting"); }
  sendStatus(r, 400, "unknown action");
}

static void framesOut(AsyncWebServerRequest *r) {
  JsonDocument d;
  JsonArray a = d["frames"].to<JsonArray>();
  for (uint8_t n = 0; n < 10; n++) {
    uint32_t id, count;
    uint8_t b[8];
    const bool any = canLastFrame(n, id, b, count);
    JsonObject o = a.add<JsonObject>();
    o["n"] = n;
    o["id"] = id;
    o["count"] = count;
    if (any) {
      char hex[24];
      snprintf(hex, sizeof(hex), "%02X %02X %02X %02X %02X %02X %02X %02X", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
      o["data"] = hex;
    }
  }
  sendJson(r, 200, d);
}

void webBegin() {
  pendMx = xSemaphoreCreateMutex();
  if (cfg.boardId) snprintf(mdnsName, sizeof(mdnsName), "adc2can%u", cfg.boardId + 1);
  else strlcpy(mdnsName, "adc2can", sizeof(mdnsName));

  static wifimgr_config_t w = wifiDefaultConfig();
  w.hostName = cfg.apSsid;   // "adc2can-XXXX" (per board), editable on the Settings tab
  w.mdnsName = mdnsName;     // http://adc2can.local (board 2: adc2can2.local ...)
  w.fwVersion = FW_VERSION;
  w.apPassword = cfg.apPass; // blank = open
  wifiManagerInit(&w);       // guarded FS mount, AP up, Home WiFi bridge if one is saved
  nowBegin();                // 1.3: the CAN frames over ESP-NOW too (adc2can_now.cpp), on the AP's channel

  otaSetup();                   // shared /api/ota/* first: its filter notes web activity for every request
  wifiManagerAttachSta(web);    // shared Home WiFi bridge: /api/wifi/sta, /api/wifi/sta/reset, /api/wifi/scan
  web.on("/api/status", HTTP_GET, statusOut);
  web.on("/api/settings", HTTP_GET, settingsOut);
  web.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *r) {}, NULL,
         [](AsyncWebServerRequest *r, uint8_t *d, size_t l, size_t i, size_t t) { parseJSON(r, d, l, i, t, settingsIn); });
  web.on("/api/action", HTTP_POST, [](AsyncWebServerRequest *r) {}, NULL,
         [](AsyncWebServerRequest *r, uint8_t *d, size_t l, size_t i, size_t t) { parseJSON(r, d, l, i, t, actionIn); });
  web.on("/api/can/frames", HTTP_GET, framesOut);
  logAttach(web);
  wifiManagerAttachStatic(web); // last: "/" = the web UI (or the recovery page) + static files, no-cache
  web.begin();
}

void webLoop() {
  // settings posted from the UI: take them over here, where cfg belongs
  if (pendDirty) {
    xSemaphoreTake(pendMx, portMAX_DELAY);
    const bool canSpeed = pend.canKbps != cfg.canKbps;
    const bool wifi = strcmp(pend.apSsid, cfg.apSsid) || strcmp(pend.apPass, cfg.apPass);
    const bool espNow = pend.espNow != cfg.espNow;
    cfg = pend;
    pendDirty = false;
    xSemaphoreGive(pendMx);
    settingsSave();
    cfgChanged(); // gauges on the bus pick the new settings up
    applyTermination();
    if (canSpeed) canRestart();
    if (wifi) { // new name / password: the access point restarts, the browser has to rejoin
      delay(300); // let the response leave first
      nowEnd();   // ESP-NOW rides on the WiFi interface: off before it goes down, back after
      wifiManagerStopAP();
      wifiManagerStartAP();
      nowBegin();
    } else if (espNow) {
      if (cfg.espNow) nowBegin();
      else nowEnd();
    }
  }
  if (pendAction && !actionAt) actionAt = millis() + 600; // the response leaves first
  if (actionAt && (int32_t)(millis() - actionAt) >= 0) {
    if (pendAction == 2) { settingsDefaults(); settingsSave(); }
    ESP.restart();
  }
  wifiManagerTick(); // bridge connection tracking + back-off
  otaTick();
}
