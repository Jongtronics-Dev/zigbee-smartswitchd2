#pragma once

#include <Arduino.h>

// OTA trigger for the MAIN (Zigbee) firmware.
//
// On 4 MB flash a single WiFi-enabled image can't be A/B-slotted (it is ~1.56 MB),
// so WiFi lives in a SEPARATE small "updater" image in the other OTA slot (ota_1).
// The main app therefore does NOT contain any WiFi/HTTP/Update code — it only:
//   * stores the update intent (artifacts + server URL + token) in NVS, and
//   * switches the boot partition to the updater and reboots.
// The updater then brings up WiFi, pulls the firmware/sound image, flashes it into
// the main slot (ota_0) and/or spiffs, and reboots back into the main app.
//
// See README.md, section 4 for the full design.
class OtaManager {
public:
  // Which artifacts the update should flash (bitmask), stored for the updater.
  static constexpr uint8_t ARTIFACT_APP    = 0x01; // main firmware -> ota_0
  static constexpr uint8_t ARTIFACT_SPIFFS = 0x02; // sound image   -> spiffs

  static OtaManager& instance() {
    static OtaManager instance;
    return instance;
  }

  // Persist the WiFi credentials the updater uses (from provisioning). Stored in NVS
  // so the (WiFi-less) main app and the updater share them.
  void setWifiCredentials(const char* ssid, const char* pass);

  // Request an update: persist intent, switch boot to the updater slot, reboot.
  // Does not return. Called from the Zigbee command handler.
  void requestOta(uint8_t artifacts, const char* serverUrl, const char* token);

  // Call once from the normal firmware after a healthy start (Zigbee joined) to
  // confirm a freshly-flashed image so the bootloader won't roll it back. Safe no-op
  // when no rollback is pending.
  void markAppValidIfNeeded();

  // Stable per-device id (from the eFuse MAC), e.g. "404cca465e28".
  static String deviceId();

private:
  OtaManager() {}
};
