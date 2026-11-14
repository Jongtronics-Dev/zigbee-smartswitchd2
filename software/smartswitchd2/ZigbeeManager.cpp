#include "Debug.h"
#include "ZigbeeManager.h"
#include "Version.h"
#include <Preferences.h>
#include "esp_partition.h"
#include "esp_system.h"

// Zigbee network data (zb_storage) made with an older endpoint layout makes the stack
// assert: a switch that went from the layout without the radar endpoint to 0.3.6 aborted
// on its first radar report (esp_zigbee_zcl_command.c:263), every boot. Erasing zb_storage
// cures it: the switch starts as a new device and joins again when pairing is allowed in
// z2m (same IEEE address, so z2m and ha_server know it as before).
//
// Two ways to get there:
//  - ZB_LAYOUT: bump it when endpoints or clusters are added or removed, and every switch
//    erases once on its first start with the new layout. A switch without a recorded layout
//    (before 0.3.7) is taken to be on the current one: nothing on the switch tells reliably
//    which layout its network data was made with (0.3.7 guessed from the radar settings in
//    NVS, and a switch whose radar had been fitted once but whose network data was older
//    still crashed).
//  - The crash-loop guard: CRASH_LIMIT panics in a row, each within CRASH_WINDOW of the
//    start, and zb_storage is erased whatever the cause. That catches the old network data
//    the layout cannot see.
//   1 = up to 0.2.x (endpoints 1..6)
//   2 = 0.3.0 and later (endpoint 7, radar cluster 0xFC10)
static constexpr uint8_t ZB_LAYOUT = 2;
static constexpr uint8_t CRASH_LIMIT = 3;
static constexpr unsigned long CRASH_WINDOW = 60000;  // ms; a crash later than this is not a boot loop

static bool crash_count_cleared = false;

static void eraseZigbeeStorage(const char* why) {
  const esp_partition_t* zb = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                       ESP_PARTITION_SUBTYPE_DATA_FAT, "zb_storage");
  if (zb && esp_partition_erase_range(zb, 0, zb->size) == ESP_OK) {
    DBG_PRINTF("[Zigbee] network data erased (%s), joining as a new device\n", why);
  } else {
    DBG_PRINTLN("[Zigbee] could not erase zb_storage");
  }
}

// Before the stack starts.
static void eraseStaleZigbeeStorage() {
  Preferences p;
  p.begin("zigbee", false);

  uint8_t layout = p.getUChar("layout", ZB_LAYOUT);
  bool erase = layout != ZB_LAYOUT;

  // Count the panics in a row; the counter is cleared once the switch has run CRASH_WINDOW.
  uint8_t crashes = esp_reset_reason() == ESP_RST_PANIC ? p.getUChar("crashes", 0) + 1 : 0;
  if (crashes >= CRASH_LIMIT) {
    erase = true;
    crashes = 0;
  }

  if (erase) {
    eraseZigbeeStorage(layout != ZB_LAYOUT ? "older endpoint layout" : "boot loop");
  }
  p.putUChar("layout", ZB_LAYOUT);
  p.putUChar("crashes", crashes);
  p.end();
}

// Once the switch has run CRASH_WINDOW, a crash is no longer part of a boot loop.
static void clearCrashCount() {
  if (crash_count_cleared || millis() < CRASH_WINDOW) return;
  crash_count_cleared = true;
  Preferences p;
  p.begin("zigbee", false);
  if (p.getUChar("crashes", 0) != 0) p.putUChar("crashes", 0);
  p.end();
}

// Convert the compiler __DATE__ ("Mmm dd yyyy") to a ZCL DateCode "YYYYMMDD", so the
// firmware build date is reported to z2m automatically — it comes from the switch itself.
static const char* buildDateCode() {
  static char code[9] = {0};
  const char* d = __DATE__; // e.g. "Aug 11 2025"
  static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = { d[0], d[1], d[2], 0 };
  const char* p = strstr(months, mon);
  int m = p ? (int)((p - months) / 3) + 1 : 0;
  int day = atoi(d + 4);
  int year = atoi(d + 7);
  snprintf(code, sizeof(code), "%04d%02d%02d", year, m, day);
  return code;
}

void ZigbeeManager::initialize(CallbackFunctionLight light_handler,
                               CallbackFunctionAudio audio_handler,
                               CallbackFunctionOta ota_handler,
                               CallbackFunctionRadar radar_handler,
                               CallbackFunctionVoid connected_handler,
                               CallbackFunctionVoid disconnected_handler) {
  light_cb = light_handler;
  audio_cb = audio_handler;
  ota_cb = ota_handler;
  radar_cb = radar_handler;
  connected_cb = connected_handler;
  disconnected_cb = disconnected_handler;

  // --- Endpoint 1: colour dimmable light (on/off + level + colour) ---
  // EP1 also hosts the Basic cluster, so set the device metadata z2m reads at
  // interview here: manufacturer/model, mains power source, and the firmware
  // version (swBuildId -> z2m "Firmware rev."). z2m only re-reads these when the
  // device is (re-)interviewed, so changing them needs a re-pair to show up.
  _light.setManufacturerAndModel("Jongtronics", "SmartSwitchD2");
  _light.setPowerSource(ZB_POWER_SOURCE_MAINS);
  _light.setSoftwareBuildId(SOFTWARE_VERSION);
  _light.setDateCode(buildDateCode()); // z2m "Firmware build date" (YYYYMMDD)
  _light.onLightChangeRgb(&ZigbeeManager::on_light);

  // --- Endpoint 2: on/off switch (client) driven by the physical button ---
  // No configuration needed here; commands are sent from send_command_*().

  // --- Endpoint 3: temperature + humidity ---
  _sensor.setMinMaxValue(-40.0, 80.0);
  _sensor.setTolerance(0.1);
  _sensor.addHumiditySensor(0.0, 100.0, 0.1);

  // --- Endpoint 4: occupancy sensing (mmWave) ---
  _occupancy.setSensorType(ESP_ZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_ULTRASONIC);

  // --- Endpoint 5: analog output used as an MP3 file index ---
  _audio.addAnalogOutput();
  _audio.onAnalogOutputChange(&ZigbeeManager::on_audio);

  // --- Endpoint 6: analog output used as an OTA trigger ---
  // z2m writes presentValue = 1 (firmware), 2 (sounds) or 3 (both); the callback asks
  // OtaManager to reboot into the WiFi updater. Same mechanism as the audio endpoint.
  _ota.addAnalogOutput();
  _ota.onAnalogOutputChange(&ZigbeeManager::on_ota);

  // --- Endpoint 7: LD2410 radar configuration (custom cluster 0xFC10) ---
  // Writes are handed to Radar, which applies them from loop(); it reports values
  // straight to the coordinator, so no binding is needed.
  _radar.onWrite(&ZigbeeManager::on_radar_write);

  // Register all endpoints, then start the stack as a router.
  Zigbee.addEndpoint(&_light);
  Zigbee.addEndpoint(&_switch);
  Zigbee.addEndpoint(&_sensor);
  Zigbee.addEndpoint(&_occupancy);
  Zigbee.addEndpoint(&_audio);
  Zigbee.addEndpoint(&_ota);
  Zigbee.addEndpoint(&_radar);

  eraseStaleZigbeeStorage();
  if (!Zigbee.begin(ZIGBEE_ROUTER)) {
    DBG_PRINTLN("Zigbee failed to start, restarting...");
    ESP.restart();
  }

  // NOTE: reporting is configured by the coordinator (zigbee2mqtt) via bind +
  // configureReporting in the .mjs configure() step. We deliberately do NOT
  // call setReporting() here: device-side reporting records created before the
  // device is bound make the periodic report timer assert in the Zigbee stack
  // (zb_zcl_report_attr -> zcl_general_commands.c:612).

  // Re-apply any persisted light state to the LED after a reboot.
  _light.restoreLight();
}

void ZigbeeManager::loop() {
  clearCrashCount();

  // The library exposes link status by polling; translate it into the
  // connected/disconnected callbacks the rest of the firmware expects.
  bool now_connected = Zigbee.connected();
  if (now_connected != _was_connected) {
    _was_connected = now_connected;
    if (now_connected) {
      if (connected_cb) connected_cb();
    } else {
      if (disconnected_cb) disconnected_cb();
    }
  }
}

void ZigbeeManager::network_reset() {
  Zigbee.factoryReset();
}

void ZigbeeManager::send_command_toggle() {
  _switch.lightToggle();  // sent to the bound target (the coordinator)
}

void ZigbeeManager::send_command_off() {
  _switch.lightOff();
}

void ZigbeeManager::send_attribute_occupancy(bool occupancy) {
  _occupancy.setOccupancy(occupancy);
  DBG_PRINTF("Send OCCUPANCY (%d)\n", occupancy);
}

void ZigbeeManager::send_attribute_temperature(float temperature) {
  _sensor.setTemperature(temperature);
}

void ZigbeeManager::send_attribute_humidity(float humidity) {
  _sensor.setHumidity(humidity);
}

// --- static thunks -> singleton std::function callbacks ---

void ZigbeeManager::on_light(bool state, uint8_t r, uint8_t g, uint8_t b, uint8_t level) {
  if (ZigbeeManager::instance().light_cb) {
    ZigbeeManager::instance().light_cb(state, r, g, b, level);
  }
}

void ZigbeeManager::on_audio(float value) {
  if (ZigbeeManager::instance().audio_cb) {
    ZigbeeManager::instance().audio_cb((uint8_t)value);
  }
}

void ZigbeeManager::on_ota(float value) {
  if (ZigbeeManager::instance().ota_cb) {
    ZigbeeManager::instance().ota_cb((uint8_t)value);
  }
}

void ZigbeeManager::on_radar_write(uint16_t attr, const uint8_t* data, uint8_t len) {
  if (ZigbeeManager::instance().radar_cb) {
    ZigbeeManager::instance().radar_cb(attr, data, len);
  }
}
