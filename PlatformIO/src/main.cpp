// adc2can V1.0 - 8 channel 0-5 V sensor -> CAN adapter for can2gauge (Forbes Automotive)
// ESP32-S3-WROOM-1-N16R8 | 2x ADS1115 | 2x TPS4H160A switched sensor rails | TCAN1044V | WiFi AP web UI
#include "adc2can_defs.h"

Settings cfg;
Live live;

void setup() {
  Serial.begin(115200);
  settingsLoad();
  ioBegin();   // rails come up as configured, inputs start converting
  canBegin();
  logBegin(); // auto-starts a log when set to
  webBegin();
  Serial.printf("adc2can %s  AP %s  CAN %u kbit/s base 0x%08lX board %u\n", FW_VERSION, cfg.apSsid, cfg.canKbps,
                (unsigned long)cfg.canBaseId, cfg.boardId);
}

void loop() {
  ioLoop();
  canLoop();
  logLoop();
  webLoop();
}
