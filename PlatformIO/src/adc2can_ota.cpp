// adc2can - updates, the way every Forbes Automotive product does them (_SharedTheme/README_OTA.md): the OTA tab
// (data/ota.js) checks GitHub for a release (Releases/releases.json, written by tools/make_release.py) or takes the two
// files by hand, web UI first, then firmware, through the shared ota_manager routes. This file only configures it and
// adds what adc2can needs on top: the hooks and the rollback confirmation (a new image boots "pending verify" and is
// confirmed after 60 s of healthy running; a crash-loop rolls back to the previous slot).
#include "adc2can_defs.h"
#include <ota_manager.h>
#include "esp_ota_ops.h"

extern AsyncWebServer web; // adc2can_web.cpp

static uint32_t bootMs = 0;
static bool confirmed = false;

// Sensors keep converting and the values keep going out on CAN while an update is written; nothing here is unsafe to
// update with the engine running, so the default "always safe" hook is kept.

void otaOnStart(bool filesystem) {
  Serial.printf("OTA: %s upload starting\n", filesystem ? "web UI" : "firmware");
}

void otaOnEnd(bool filesystem, bool success) {
  Serial.printf("OTA: %s upload %s\n", filesystem ? "web UI" : "firmware", success ? "done" : "FAILED");
}

void otaSetup() {
  bootMs = millis();
  static ota_config_t c = otaDefaultConfig();
  c.fwVersion = FW_VERSION;
  c.product = "adc2can";
  c.githubRepo = "Forbes-Automotive/adc2can"; // Releases/ + releases.json live here
  otaManagerInit(&c);
  otaManagerAttach(web); // first, so its activity filter sees every request
}

void otaTick() {
  if (confirmed || millis() - bootMs < 60000) return;
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("OTA: new firmware confirmed");
  }
  confirmed = true;
}
