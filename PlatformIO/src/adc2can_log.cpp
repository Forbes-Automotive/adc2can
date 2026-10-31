// adc2can - data logger (flash) and the 10-minute trend (RAM).
//
// LOGGER: the raw "logs" partition (partitions_16mb.csv, 3072 x 4 kB sectors = 12 MB) is a ring of self-describing sectors -
// no filesystem, so the whole partition is log space, wear is spread evenly and a web UI update never touches it.
//   sector = 256-byte header (SecHdr) + 96 records of 40 bytes (Rec), 4096 bytes in all.
//   A recording ("log") is every sector with the same session id, in seq order. A record slot whose t is 0xFFFFFFFF
//   is unwritten: the end of the data in that sector. Each header carries the names / units / decimals and the start
//   time, so any surviving sector decodes on its own (the oldest ones are reused when "overwrite oldest" is set).
//   Deleting a log only clears the magic word of its sectors (flash can always program 1 -> 0, no erase); a sector is
//   erased when it is next used.
//   The browser downloads a log as its raw sectors (GET /api/log/data) and turns them into CSV itself (data/app.js).
//
// TREND: the last 10 minutes of every sensor as 1 s averages, for the Logging tab's graphs (GET /api/trend).
//
// Flash erase / write runs in loop() (it owns the partition); the web handlers read through the mutex.
#include "adc2can_defs.h"
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <algorithm>
#include <map>
#include <memory>
#include <vector>

static const uint32_t SEC_MAGIC = 0x4C433241; // "A2CL"
static const uint16_t SEC_SIZE = 4096, HDR_SIZE = 256, REC_SIZE = 40, RECS = (SEC_SIZE - HDR_SIZE) / REC_SIZE; // 96
static const uint32_t ERASED = 0xFFFFFFFF;
static const uint8_t RATES[] = {1, 2, 5, 10, 20, 50};

struct __attribute__((packed)) SecHdr {
  uint32_t magic, seq, session, periodMs;
  uint64_t epochMs; // wall clock at the log's start (from the browser that started it), 0 = unknown
  uint32_t tFirst;  // ms since the log's start of this sector's first record
  uint8_t decimals[NUM_CH];
  char name[NUM_CH][16];
  char unit[NUM_CH][8];
  uint8_t pad[HDR_SIZE - 228];
};
struct __attribute__((packed)) Rec {
  uint32_t t;         // ms since the log's start
  float v[NUM_CH];    // NaN = not valid
  uint8_t valid, fault;
  uint16_t vbatMv;
};
static_assert(sizeof(SecHdr) == HDR_SIZE, "sector header");
static_assert(sizeof(Rec) == REC_SIZE, "record");

static const esp_partition_t *part = nullptr;
static uint16_t nSec = 0;
static uint32_t *secSeq = nullptr;  // 0 = free (erased or deleted)
static uint32_t *secSess = nullptr;
static uint32_t maxSeq = 0, maxSession = 0;
static SemaphoreHandle_t mx;

// recorder
enum { ST_IDLE, ST_REC, ST_FULL };
static uint8_t state = ST_IDLE;
static uint8_t rateHz = 10, autoStart = 0, whenFull = 0; // whenFull: 0 stop, 1 overwrite the oldest
static uint32_t session = 0, tStart = 0, nextAt = 0, recCount = 0;
static uint64_t epochMs = 0;
static int32_t curSec = -1;         // sector being filled
static uint16_t curSlot = 0;        // next free slot in it (written + buffered)
static uint16_t flushedSlot = 0;    // slots already in flash
static Rec buf[RECS];               // records not yet in flash (slots flushedSlot .. curSlot-1)
static uint32_t lastFlush = 0;
static SecHdr hdr;                  // the running log's header (names etc. frozen at its start)

// commands from the web task
static volatile uint8_t cmd = 0;    // 1 start, 2 stop, 3 delete, 4 delete all
static uint32_t cmdSession = 0;
static uint64_t cmdEpoch = 0;

// trend
static const uint16_t TREND_N = 600;
static float trend[TREND_N][NUM_CH];
static uint32_t trendSeq = 0; // points pushed so far
static float acc[NUM_CH];
static uint8_t accN[NUM_CH];
static uint32_t tTrend = 0, tAcc = 0;

static uint32_t secAddr(uint16_t s) { return (uint32_t)s * SEC_SIZE; }

// ---------------------------------------------------------------- flash
static void flushRecs() {
  if (curSec < 0 || curSlot == flushedSlot) return;
  esp_partition_write(part, secAddr(curSec) + HDR_SIZE + flushedSlot * REC_SIZE, &buf[flushedSlot], (curSlot - flushedSlot) * REC_SIZE);
  flushedSlot = curSlot;
  lastFlush = millis();
}

// the next sector to fill: a free one ahead of the current, else (overwrite) the oldest; -1 = full
static int32_t pickSector() {
  const uint16_t from = curSec < 0 ? 0 : (curSec + 1) % nSec;
  for (uint16_t k = 0; k < nSec; k++) {
    const uint16_t s = (from + k) % nSec;
    if (!secSeq[s]) return s;
  }
  if (!whenFull) return -1;
  int32_t best = -1;
  for (uint16_t s = 0; s < nSec; s++)
    if ((int32_t)s != curSec && (best < 0 || secSeq[s] < secSeq[best])) best = s;
  return best;
}

static bool openSector(uint32_t tFirst) {
  const int32_t s = pickSector();
  if (s < 0) return false;
  esp_partition_erase_range(part, secAddr(s), SEC_SIZE);
  hdr.seq = ++maxSeq;
  hdr.tFirst = tFirst;
  esp_partition_write(part, secAddr(s), &hdr, HDR_SIZE);
  xSemaphoreTake(mx, portMAX_DELAY);
  secSeq[s] = hdr.seq;
  secSess[s] = session;
  curSec = s;
  curSlot = flushedSlot = 0;
  xSemaphoreGive(mx);
  return true;
}

static void startLog(uint64_t epoch) {
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = SEC_MAGIC;
  hdr.session = session = ++maxSession;
  hdr.periodMs = 1000 / rateHz;
  hdr.epochMs = epochMs = epoch;
  for (uint8_t i = 0; i < NUM_CH; i++) {
    hdr.decimals[i] = cfg.ch[i].decimals;
    strlcpy(hdr.name[i], cfg.ch[i].name, sizeof(hdr.name[i]));
    strlcpy(hdr.unit[i], cfg.ch[i].unit, sizeof(hdr.unit[i]));
  }
  tStart = nextAt = millis();
  recCount = 0;
  curSec = -1; // a log always starts in a sector of its own
  state = ST_REC;
  Serial.printf("log %u started at %u Hz\n", session, rateHz);
}

static void stopLog(uint8_t why) {
  flushRecs();
  state = why;
  curSec = -1;
  Serial.printf("log %u %s, %u records\n", session, why == ST_FULL ? "stopped: memory full" : "stopped", recCount);
}

static void deleteSectors(bool all, uint32_t sess) {
  const uint32_t zero = 0;
  for (uint16_t s = 0; s < nSec; s++) {
    if (!secSeq[s] || (!all && secSess[s] != sess)) continue;
    esp_partition_write(part, secAddr(s), &zero, 4); // magic -> 0: free, erased when next used
    xSemaphoreTake(mx, portMAX_DELAY);
    secSeq[s] = 0;
    xSemaphoreGive(mx);
  }
}

static void record(uint32_t now) {
  Rec r;
  r.t = now - tStart;
  r.valid = r.fault = 0;
  for (uint8_t i = 0; i < NUM_CH; i++) {
    r.v[i] = live.valid[i] ? live.value[i] : NAN;
    if (live.valid[i]) r.valid |= 1 << i;
    if (live.railFault[i]) r.fault |= 1 << i;
  }
  r.vbatMv = live.vbat > 0 ? (uint16_t)min(65535.0f, live.vbat * 1000.0f) : 0;
  if (curSec < 0 || curSlot >= RECS) {
    flushRecs();
    if (!openSector(r.t)) { stopLog(ST_FULL); return; }
  }
  buf[curSlot++] = r;
  recCount++;
  if (curSlot >= RECS) flushRecs();
}

// ---------------------------------------------------------------- settings (NVS "log", not in Settings: no reset of the rest)
static void prefsLoad() {
  Preferences p;
  p.begin("log", true);
  rateHz = p.getUChar("rate", 10);
  autoStart = p.getUChar("auto", 0);
  whenFull = p.getUChar("full", 0);
  p.end();
  bool ok = false;
  for (uint8_t r : RATES) ok |= r == rateHz;
  if (!ok) rateHz = 10;
}
static void prefsSave() {
  Preferences p;
  p.begin("log", false);
  p.putUChar("rate", rateHz);
  p.putUChar("auto", autoStart);
  p.putUChar("full", whenFull);
  p.end();
}

// ---------------------------------------------------------------- public
void logBegin() {
  mx = xSemaphoreCreateMutex();
  prefsLoad();
  for (uint16_t k = 0; k < TREND_N; k++)
    for (uint8_t i = 0; i < NUM_CH; i++) trend[k][i] = NAN;
  part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x99, "logs");
  if (!part) { Serial.println("log: no \"logs\" partition - flash the partition table over USB"); return; }
  nSec = part->size / SEC_SIZE;
  secSeq = (uint32_t *)calloc(nSec, 4);
  secSess = (uint32_t *)calloc(nSec, 4);
  for (uint16_t s = 0; s < nSec; s++) {
    uint32_t h[3];
    esp_partition_read(part, secAddr(s), h, sizeof(h));
    if (h[0] != SEC_MAGIC) continue;
    secSeq[s] = h[1];
    secSess[s] = h[2];
    maxSeq = max(maxSeq, h[1]);
    maxSession = max(maxSession, h[2]);
  }
  if (autoStart) startLog(0);
}

void logLoop() {
  const uint32_t now = millis();
  // trend: 100 ms samples averaged into 1 s points
  if (now - tAcc >= 100) {
    tAcc = now;
    for (uint8_t i = 0; i < NUM_CH; i++)
      if (live.valid[i] && isfinite(live.value[i])) { acc[i] += live.value[i]; accN[i]++; }
  }
  if (now - tTrend >= 1000) {
    tTrend = now;
    xSemaphoreTake(mx, portMAX_DELAY);
    float *p = trend[trendSeq % TREND_N];
    for (uint8_t i = 0; i < NUM_CH; i++) { p[i] = accN[i] ? acc[i] / accN[i] : NAN; acc[i] = 0; accN[i] = 0; }
    trendSeq++;
    xSemaphoreGive(mx);
  }
  if (!part) return;
  // commands from the web UI
  if (cmd) {
    const uint8_t c = cmd;
    cmd = 0;
    if (c == 1 && state != ST_REC) startLog(cmdEpoch);
    else if (c == 2 && state == ST_REC) stopLog(ST_IDLE);
    else if (c == 3) {
      if (state == ST_REC && cmdSession == session) stopLog(ST_IDLE);
      deleteSectors(false, cmdSession);
      if (state == ST_FULL) state = ST_IDLE;
    } else if (c == 4) {
      if (state == ST_REC) stopLog(ST_IDLE);
      deleteSectors(true, 0);
      state = ST_IDLE;
    }
  }
  if (state != ST_REC) return;
  if ((int32_t)(now - nextAt) >= 0) {
    const uint32_t period = 1000 / rateHz;
    nextAt += period;
    if ((int32_t)(now - nextAt) > (int32_t)period) nextAt = now + period; // fell behind (flash erase): no catch-up burst
    record(now);
  }
  if (now - lastFlush >= 1000) flushRecs(); // at most a second lost on a power cut
}

// ---------------------------------------------------------------- web
struct LogInfo { uint32_t sectors = 0, firstSeq = 0xFFFFFFFF, lastSeq = 0; uint16_t firstSec = 0, lastSec = 0; };

// records in a sector (slots up to the first unwritten one)
static uint16_t secRecords(uint16_t s, uint32_t *lastT) {
  uint16_t lo = 0, hi = RECS; // records are written in order: binary search for the first unwritten slot
  while (lo < hi) {
    const uint16_t m = (lo + hi) / 2;
    uint32_t t;
    esp_partition_read(part, secAddr(s) + HDR_SIZE + m * REC_SIZE, &t, 4);
    if (t == ERASED) hi = m; else lo = m + 1;
  }
  if (lo && lastT) esp_partition_read(part, secAddr(s) + HDR_SIZE + (lo - 1) * REC_SIZE, lastT, 4);
  return lo;
}

static void logOut(AsyncWebServerRequest *r) {
  JsonDocument d;
  d["state"] = state == ST_REC ? "recording" : state == ST_FULL ? "full" : "idle";
  d["available"] = part != nullptr;
  d["rateHz"] = rateHz;
  d["autoStart"] = autoStart;
  d["whenFull"] = whenFull;
  d["session"] = state == ST_REC ? session : 0;
  d["elapsedMs"] = state == ST_REC ? millis() - tStart : 0;
  d["records"] = state == ST_REC ? recCount : 0;
  d["recSize"] = REC_SIZE;
  d["recsPerSector"] = RECS;
  d["sectors"] = nSec;
  JsonArray rates = d["rates"].to<JsonArray>();
  for (uint8_t x : RATES) rates.add(x);
  if (part) {
    std::map<uint32_t, LogInfo> logs;
    uint32_t used = 0;
    xSemaphoreTake(mx, portMAX_DELAY);
    for (uint16_t s = 0; s < nSec; s++) {
      if (!secSeq[s]) continue;
      used++;
      LogInfo &L = logs[secSess[s]];
      L.sectors++;
      if (secSeq[s] < L.firstSeq) { L.firstSeq = secSeq[s]; L.firstSec = s; }
      if (secSeq[s] > L.lastSeq) { L.lastSeq = secSeq[s]; L.lastSec = s; }
    }
    xSemaphoreGive(mx);
    d["usedSectors"] = used;
    // time left at this rate: free sectors (or, overwriting, the whole memory)
    const uint32_t freeRecs = (nSec - used) * RECS;
    d["secondsLeft"] = freeRecs / rateHz;
    JsonArray a = d["logs"].to<JsonArray>();
    for (auto it = logs.rbegin(); it != logs.rend(); ++it) { // newest first
      const LogInfo &L = it->second;
      SecHdr h;
      esp_partition_read(part, secAddr(L.firstSec), &h, 32);
      uint32_t lastT = h.tFirst;
      const bool active = state == ST_REC && it->first == session;
      const uint16_t inLast = active ? curSlot : secRecords(L.lastSec, &lastT);
      if (active && curSlot) lastT = buf[curSlot - 1].t;
      JsonObject o = a.add<JsonObject>();
      o["id"] = it->first;
      o["sectors"] = L.sectors;
      o["bytes"] = L.sectors * SEC_SIZE;
      o["records"] = (L.sectors - 1) * RECS + inLast;
      o["periodMs"] = h.periodMs;
      o["epochMs"] = (double)h.epochMs;
      o["startMs"] = h.tFirst; // > 0: the start was overwritten
      o["durationMs"] = lastT - h.tFirst;
      o["active"] = active;
    }
  }
  String out;
  serializeJson(d, out);
  r->send(200, "application/json", out);
}

static void logIn(AsyncWebServerRequest *r, JsonObjectConst d) {
  const String a = d["action"] | "";
  if (d["rateHz"].is<int>()) {
    const int x = d["rateHz"];
    bool ok = false;
    for (uint8_t v : RATES) ok |= v == x;
    if (!ok) { r->send(400, "application/json", "{\"error\":\"rate must be 1, 2, 5, 10, 20 or 50 Hz\"}"); return; }
    if (state == ST_REC && x != rateHz) { r->send(400, "application/json", "{\"error\":\"stop the log to change the rate\"}"); return; }
    rateHz = x;
  }
  if (!d["autoStart"].isNull()) autoStart = d["autoStart"].as<int>() ? 1 : 0;
  if (!d["whenFull"].isNull()) whenFull = d["whenFull"].as<int>() ? 1 : 0;
  prefsSave();
  if (!part && a.length()) { r->send(400, "application/json", "{\"error\":\"no log partition - flash over USB once\"}"); return; }
  if (a == "start") { cmdEpoch = (uint64_t)(d["epochMs"] | 0.0); cmd = 1; } // the browser's clock (JSON number, ms)
  else if (a == "stop") cmd = 2;
  else if (a == "delete") { cmdSession = d["id"] | 0; cmd = 3; }
  else if (a == "deleteAll") cmd = 4;
  r->send(200, "application/json", a == "start" ? "{\"status\":\"recording\"}" : a == "stop" ? "{\"status\":\"stopped\"}" :
                                     a.startsWith("delete") ? "{\"status\":\"deleted\"}" : "{\"status\":\"saved\"}");
}

// GET /api/log/data?id=N: the log's sectors, oldest first, as stored
static void logData(AsyncWebServerRequest *r) {
  if (!part || !r->hasParam("id")) { r->send(400, "text/plain", "id?"); return; }
  const uint32_t id = r->getParam("id")->value().toInt();
  auto list = std::make_shared<std::vector<uint16_t>>();
  xSemaphoreTake(mx, portMAX_DELAY);
  for (uint16_t s = 0; s < nSec; s++)
    if (secSeq[s] && secSess[s] == id) list->push_back(s);
  std::sort(list->begin(), list->end(), [](uint16_t a, uint16_t b) { return secSeq[a] < secSeq[b]; });
  xSemaphoreGive(mx);
  if (list->empty()) { r->send(404, "text/plain", "no such log"); return; }
  const size_t len = list->size() * SEC_SIZE;
  AsyncWebServerResponse *res = r->beginResponse("application/octet-stream", len, [list](uint8_t *out, size_t max, size_t index) -> size_t {
    const size_t s = index / SEC_SIZE, off = index % SEC_SIZE;
    if (s >= list->size()) return 0;
    const size_t n = min(max, (size_t)(SEC_SIZE - off));
    esp_partition_read(part, secAddr((*list)[s]) + off, out, n);
    return n;
  });
  res->addHeader("Content-Disposition", "attachment; filename=\"adc2can-log-" + String(id) + ".a2c\"");
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

// GET /api/trend?since=N: the 1 s points after N (all of the last 10 minutes when N is 0 or too old)
static void trendOut(AsyncWebServerRequest *r) {
  const uint32_t since = r->hasParam("since") ? r->getParam("since")->value().toInt() : 0;
  AsyncResponseStream *s = r->beginResponseStream("application/json");
  xSemaphoreTake(mx, portMAX_DELAY);
  const uint32_t seq = trendSeq;
  const uint32_t from = max(since, seq > TREND_N ? seq - TREND_N : 0);
  s->printf("{\"seq\":%u,\"periodMs\":1000,\"from\":%u,\"ch\":[", seq, from);
  for (uint8_t i = 0; i < NUM_CH; i++) {
    s->print(i ? "],[" : "[");
    const uint8_t dp = min(4, cfg.ch[i].decimals + 1);
    for (uint32_t k = from; k < seq; k++) {
      const float v = trend[k % TREND_N][i];
      if (k > from) s->print(',');
      if (isfinite(v)) s->printf("%.*f", dp, v); else s->print("null");
    }
  }
  xSemaphoreGive(mx);
  s->print("]]}");
  r->send(s);
}

void logAttach(AsyncWebServer &web) {
  web.on("/api/log/data", HTTP_GET, logData); // before /api/log: the server matches "<uri>/..." prefixes
  web.on("/api/log", HTTP_GET, logOut);
  web.on("/api/log", HTTP_POST, [](AsyncWebServerRequest *r) {}, NULL,
         [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
           String *body = (String *)r->_tempObject;
           if (index == 0) { body = new String(); body->reserve(total); r->_tempObject = body; }
           body->concat((const char *)data, len);
           if (index + len < total) return;
           JsonDocument d;
           if (deserializeJson(d, *body)) r->send(400, "application/json", "{\"error\":\"bad JSON\"}");
           else logIn(r, d.as<JsonObjectConst>());
           delete body;
           r->_tempObject = NULL;
         });
  web.on("/api/trend", HTTP_GET, trendOut);
}

bool logRecording() { return state == ST_REC; }
