#include "Debug.h"
#include "OtaManager.h"

#include <Preferences.h>
#include <esp_ota_ops.h>

// NVS namespaces/keys — SHARED with the updater image (must match updater.ino).
static const char* NS_OTA  = "ota";
static const char* K_ART   = "artifacts";  // uint8 bitmask
static const char* K_URL   = "url";         // String (server base, no trailing slash)
static const char* K_TOKEN = "token";       // String

static const char* NS_WIFI = "otawifi";
static const char* K_SSID  = "ssid";        // String
static const char* K_PASS  = "pass";        // String

// Server used when a trigger carries no URL (e.g. the Zigbee OTA trigger, which only
// carries the artifact selection): the ha_server LAN address. It is set in OtaConfig.h,
// which is git-ignored; copy OtaConfig.example.h to OtaConfig.h and fill it in. Without
// it the firmware still builds, and a trigger without a URL then has no server to use.
#if __has_include("OtaConfig.h")
#include "OtaConfig.h"
#endif
#ifndef OTA_DEFAULT_SERVER_URL
#define OTA_DEFAULT_SERVER_URL ""   // set in OtaConfig.h (see OtaConfig.example.h)
#endif

String OtaManager::deviceId() {
  // Full 8-byte EUI-64 (getEfuseMac returns it with the ff:fe fill), formatted to match
  // the Zigbee IEEE address zigbee2mqtt shows (e.g. "404ccafffe465e28"). ha_server keys
  // OTA files by this, so the front-end (which knows the ieee address) and the device agree.
  uint64_t mac = ESP.getEfuseMac();
  uint8_t b[8];
  for (int i = 0; i < 8; i++) {
    b[i] = (uint8_t)((mac >> (8 * i)) & 0xFF);
  }
  char buf[17];
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
  return String(buf);
}

void OtaManager::setWifiCredentials(const char* ssid, const char* pass) {
  Preferences p;
  p.begin(NS_WIFI, false);
  p.putString(K_SSID, ssid ? ssid : "");
  p.putString(K_PASS, pass ? pass : "");
  p.end();
}

void OtaManager::requestOta(uint8_t artifacts, const char* serverUrl, const char* token) {
  // A trigger without a URL (the Zigbee trigger) uses the default server.
  const char* url = (serverUrl != nullptr && strlen(serverUrl) > 0) ? serverUrl : OTA_DEFAULT_SERVER_URL;

  // Persist the intent for the updater to read after reboot.
  Preferences p;
  p.begin(NS_OTA, false);
  p.putUChar(K_ART, artifacts);
  p.putString(K_URL, url);
  p.putString(K_TOKEN, token ? token : "");
  p.end();

  // The main app always runs from ota_0, so the "next" update partition is ota_1 —
  // the updater. Boot into it; the updater brings up WiFi and does the actual work.
  const esp_partition_t* updater = esp_ota_get_next_update_partition(NULL);
  if (updater == nullptr) {
    DBG_PRINTLN("[OTA] no updater partition found, aborting request");
    return;
  }
  if (esp_ota_set_boot_partition(updater) != ESP_OK) {
    DBG_PRINTLN("[OTA] failed to set boot partition to updater");
    return;
  }

  DBG_PRINTF("[OTA] requested (artifacts=0x%02x), rebooting into updater\n", artifacts);
  delay(100);
  ESP.restart();
}

void OtaManager::markAppValidIfNeeded() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running == nullptr) {
    return;
  }
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
    return;
  }
  if (state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    DBG_PRINTLN("[OTA] running image marked valid (rollback cancelled)");
  }
}
