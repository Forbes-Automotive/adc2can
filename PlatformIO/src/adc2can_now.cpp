// adc2can - the CAN frames over ESP-NOW as well (1.3, include/adcnow_proto.h). Every frame adc2can_can.cpp sends is
// queued here and the queue is broadcast at the end of each canLoop() pass, so a can2gauge with power only - no CAN wire
// - shows the sensors, names, units and the shared config just the same. One-way: nothing received over the air.
// Optionally the frames received from the car's CAN go out too (cfg.canRelay, include/cannow_tx.h - the same relay as
// OpenHaldex's), pumped by their own task so a slow loop() pass (flash log, I2C) does not hold them up.
//
// Runs on the access point's interface and channel (wifi_manager, channel 1 by default). If the Home WiFi bridge joins a
// network on another channel, the radio - and these broadcasts - follow it, and a gauge on channel 1 stops hearing them.
#include "adc2can_defs.h"
#include <WiFi.h>
#include <esp_now.h>
#include <adcnow_proto.h>
#include <cannow_tx.h>

static uint8_t buf[sizeof(AdcnHdr) + ADCN_MAX_FRAMES * sizeof(AdcnFrame)];
static uint8_t count = 0, seq = 0;
static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static SemaphoreHandle_t nowMx = NULL; // ESP-NOW up / down against the relay task's sends

// the relay's packets: called with nowMx held and ESP-NOW up
static bool relaySend(const uint8_t *data, size_t len) { return esp_now_send(bcast, data, len) == ESP_OK; }

static void relayTask(void *) {
  for (;;) {
    cannSetRate(cfg.canRelay);
    if (live.nowUp && xSemaphoreTake(nowMx, pdMS_TO_TICKS(5)) == pdTRUE) {
      if (live.nowUp) cannPump(relaySend);
      xSemaphoreGive(nowMx);
    }
    vTaskDelay(pdMS_TO_TICKS(cannRate != CANN_R_OFF ? 4 : 20)); // relaying: well inside CANN_FLUSH_MS
  }
}

void nowBegin() {
  if (!nowMx) {
    nowMx = xSemaphoreCreateMutex();
    cannBegin(CANN_K_ADC2CAN);
    xTaskCreatePinnedToCore(relayTask, "canRelay", 4096, NULL, 3, NULL, 0);
  }
  if (live.nowUp || !cfg.espNow || WiFi.getMode() == WIFI_OFF) return;
  xSemaphoreTake(nowMx, portMAX_DELAY);
  if (esp_now_init() != ESP_OK) {
    xSemaphoreGive(nowMx);
    return;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, bcast, 6); // broadcast: no pairing, every gauge in range on the channel hears it
  peer.channel = 0;                 // the current WiFi channel (the AP's)
  peer.ifidx = WIFI_IF_AP;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    esp_now_deinit();
    xSemaphoreGive(nowMx);
    return;
  }
  count = 0;
  live.nowUp = true;
  cannLink(true);
  xSemaphoreGive(nowMx);
  Serial.println("ESP-NOW broadcast up");
}

void nowEnd() {
  if (!live.nowUp) return;
  xSemaphoreTake(nowMx, portMAX_DELAY);
  live.nowUp = false;
  cannLink(false);
  esp_now_deinit();
  count = 0;
  xSemaphoreGive(nowMx);
}

void nowFlush() {
  if (!live.nowUp || !count) return;
  AdcnHdr *h = (AdcnHdr *)buf;
  h->magic[0] = ADCN_MAGIC0;
  h->magic[1] = ADCN_MAGIC1;
  h->proto = ADCN_PROTO;
  h->board = cfg.boardId & 3;
  h->seq = seq++;
  h->count = count;
  h->fw[0] = FW_MAJOR;
  h->fw[1] = FW_MINOR;
  bool ok = false;
  if (xSemaphoreTake(nowMx, pdMS_TO_TICKS(5)) == pdTRUE) {
    ok = live.nowUp && esp_now_send(bcast, buf, sizeof(AdcnHdr) + count * sizeof(AdcnFrame)) == ESP_OK;
    xSemaphoreGive(nowMx);
  }
  if (ok) live.nowTx++;
  else live.nowErr++;
  count = 0;
}

void nowQueue(uint8_t n, const uint8_t *d) {
  if (!live.nowUp) return;
  if (count >= ADCN_MAX_FRAMES) nowFlush(); // a full packet (a config dump): send it and start the next
  AdcnFrame *f = (AdcnFrame *)(buf + sizeof(AdcnHdr) + count * sizeof(AdcnFrame));
  f->n = n;
  memcpy(f->d, d, 8);
  count++;
}

void nowTap(const twai_message_t &m) { cannTap(m); }

void nowRelayStats(uint32_t &framesPerSec, uint32_t &lostPerSec, uint32_t &frames, uint32_t &lost) {
  framesPerSec = cannStat.framesPerSec;
  lostPerSec = cannStat.lostPerSec;
  frames = cannStat.frames;
  lost = cannStat.lost;
}
