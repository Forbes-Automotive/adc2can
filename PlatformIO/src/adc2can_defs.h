// adc2can V1 - shared types, settings and live state.
#pragma once
#include <Arduino.h>
#include "driver/twai.h"
#include <cannow_proto.h> // cannRate_t (the CAN frame relay)
#include "adc2can_pins.h"

#define FW_VERSION "1.4.1" // must match data/version.json "fs" (tools/make_release.py checks)
#define FW_MAJOR 1
#define FW_MINOR 4
#define CFG_PROTO 2   // CAN protocol: 2 = config shared + editable over CAN (n 8 / 9 / 12 / 13)
#define CFG_PARTS 32  // config parts per channel (adc2can_can.cpp); 1.4: 30 + Dial Min / Max
#define TABLE_PTS 8

// ---------------------------------------------------------------- channel conversion
enum ChMode : uint8_t { MODE_LINEAR = 0, MODE_TABLE = 1, MODE_NTC = 2 };
enum FaultPolicy : uint8_t { FAULT_RETRY = 0, FAULT_LATCH = 1 };

struct ChCfg {
  char name[16];      // sent to can2gauge as the gauge label
  char unit[8];       // sent to can2gauge as the unit
  uint8_t mode;       // ChMode
  uint8_t railOn;     // 1 = switch this sensor's 5 V rail on
  uint8_t ratiometric;// 1 = scale the reading to a 5.000 V supply using the measured rail (sensor powered from its pin 1)
  uint8_t smoothing;  // 1 = none .. 250 = heavy (exponential, as can2io / MCE18)
  uint8_t decimals;   // display hint for the web UI
  uint8_t spare[3];
  float scale;        // MODE_LINEAR: units per volt
  float offset;       // MODE_LINEAR: units at 0 V
  float trim;         // gain trim for the 10k/11k divider (1.000 = nominal)
  float tblV[TABLE_PTS], tblVal[TABLE_PTS]; // MODE_TABLE: volts -> value, ascending volts, unused points = NAN
  float ntcR25, ntcBeta, ntcPullup;          // MODE_NTC: sensor R at 25 C, beta, fitted R_PUn (to the channel's own rail)
  float lowWarn, highWarn;                   // web UI colouring + CAN "out of range" bit
};

struct Settings {
  uint32_t magic;
  ChCfg ch[NUM_CH];
  // rails
  uint8_t faultPolicy; // FaultPolicy: retry = leave enabled (TPS4H160A auto-retries); latch = switch off until /reset
  uint8_t spare1[3];
  uint16_t faultHoldMs;  // fault must persist this long before it counts (and before a latch)
  uint16_t backfeedRetryS; // sensor bus over-voltage: all rails off, retry after this many seconds
  float railLowV;        // rail on but below this = overload / short (rail fault)
  float busHighV;        // sensor bus above this = something has pushed 12 V onto a sensor 5 V pin
  float vbatLow, vbatHigh;
  float no12vBelow;      // VBAT below this = running from USB only
  // CAN
  uint16_t canKbps;      // 125 / 250 / 500 / 1000
  uint8_t boardId;       // 0-3: several adc2cans on one bus
  uint8_t canTerm;       // 1 = 120R termination on
  uint32_t canBaseId;    // 29-bit base; frames = base + boardId*16 + 0..7
  uint16_t txPeriodMs;   // value frames
  uint16_t statusPeriodMs;
  // WiFi
  char apSsid[24];
  char apPass[24];
  // ---- 1.3: appended (a 1.2 blob ends at espNow and migrates - settingsLoad) ----
  uint8_t espNow;      // 1 = also broadcast every CAN frame over ESP-NOW (adc2can_now.cpp): a gauge without CAN still sees it
  uint8_t canRelay;    // cannRate_t: relay the frames received from the car's CAN too (include/cannow_tx.h); 0 = off
  uint8_t spare2[2];
  // ---- 1.4: appended (a 1.3 blob ends here and keeps the defaults below - settingsLoad) ----
  float dialMin[NUM_CH]; // the dial's bottom for each sensor (web UI tiles, and sent to can2gauge); NAN = Auto
  float dialMax[NUM_CH]; // the dial's top; NAN = Auto (dialRange works it out from the sensor's scaling)
};

struct Live {
  float pinV[NUM_CH];  // volts at the sensor connector (after the divider maths)
  float corrV[NUM_CH]; // after ratiometric correction
  float value[NUM_CH]; // engineering value (smoothed)
  bool valid[NUM_CH];  // rail OK, ADC OK, value finite
  float railV[NUM_CH];
  bool railEn[NUM_CH];   // what the firmware is driving
  bool railFault[NUM_CH];// ST# flag or rail-low, after faultHoldMs
  bool latched[NUM_CH];
  uint32_t faultSince[NUM_CH];
  float busV, vbat;
  bool backfeed;       // sensor bus over-voltage -> all rails forced off
  bool no12v;          // VBAT absent (USB bench power): the buck - and so the sensor bus - is off, rail faults are not raised
  uint32_t backfeedMs;
  bool adcOk[2], expOk;
  uint8_t stFlags;     // raw TCA9554 input byte (bit n low = ST# of channel n+1 active)
  uint32_t canTx, canErr;
  uint32_t canRx, canRxMs; // frames from a gauge (config requests / edits) and when the last one came
  bool canUp;
  uint8_t canState;        // 0 stopped, 1 running, 2 bus-off, 3 recovering (twai_state_t)
  uint8_t canTec, canRec;  // TWAI transmit / receive error counters
  bool led;                // what the status LED is showing now
  uint32_t cfgRev;     // changes with every settings change (and every boot): gauges re-read the config when it moves
  bool nowUp;          // ESP-NOW broadcasting
  uint32_t nowTx, nowErr; // ESP-NOW packets sent / refused
};

extern Settings cfg;
extern Live live;

void settingsLoad();
void settingsSave();
void settingsDefaults();

void ioBegin();
void ioLoop();
void railReset(uint8_t ch); // clear a latched rail fault (0xFF = all)
void applyTermination();

void canBegin();
void canRestart(); // bit rate changed: reinstall the TWAI driver
void canLoop();
bool canLastFrame(uint8_t n, uint32_t &id, uint8_t *d, uint32_t &count); // last frame sent with offset n (0-9), for the web UI
void cfgChanged(); // settings changed (web UI or CAN): new revision, broadcast the whole config
// 1.4: sensor ch's dial range: its Dial Min / Max, or where Auto, worked out from its scaling. The one place the range
// comes from - the web UI and can2gauge both use what this says.
void dialRange(uint8_t ch, float &low, float &high);

// adc2can_now.cpp (1.3): the CAN frames over ESP-NOW as well (include/adcnow_proto.h)
void nowBegin();                         // after WiFi is up (and again after an AP restart); no-op when cfg.espNow is off
void nowEnd();                           // before WiFi goes down / when switched off
void nowQueue(uint8_t n, const uint8_t *d); // a frame just sent on CAN (or that would have been: no bus needed)
void nowFlush();                         // end of a canLoop() pass: broadcast what was queued
// the CAN frame relay (include/cannow_proto.h, sender include/cannow_tx.h - shared with OpenHaldex), cfg.canRelay
void nowTap(const twai_message_t &m);    // the CAN receive task: every frame received from the car's CAN
void nowRelayStats(uint32_t &framesPerSec, uint32_t &lostPerSec, uint32_t &frames, uint32_t &lost);

class AsyncWebServer;
void logBegin();    // adc2can_log.cpp: the flash data logger + the 10-minute trend
void logLoop();
void logAttach(AsyncWebServer &web); // /api/log, /api/log/data, /api/trend
bool logRecording();

void webBegin(); // WiFi (shared wifi_manager), OTA (shared ota_manager), JSON API, web UI from LittleFS
void webLoop();
void otaSetup(); // adc2can_ota.cpp: the shared ota_manager, configured for adc2can
void otaTick();
