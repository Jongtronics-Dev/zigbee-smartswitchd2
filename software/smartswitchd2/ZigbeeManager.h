#pragma once

#include <Arduino.h>
#include <functional>

// Arduino ESP32 Zigbee library (arduino-esp32 v3.x, "Zigbee" tools mode).
// NOTE: this is the *library* header, not our old hand-rolled wrapper.
// The library exposes a global object called `Zigbee`, which is why this
// wrapper class is named ZigbeeManager and no longer `Zigbee`.
#include <Zigbee.h>
#include "ZigbeeRadar.h"

// One endpoint per function (device-type), instead of everything on ep 1.
// These numbers MUST stay in sync with the endpoint() map in smartswitch.mjs.
#define EP_LIGHT     1  // On/Off + Level + Color (the LED)
#define EP_SWITCH    2  // On/Off client (the physical button -> action)
#define EP_SENSOR    3  // Temperature + Humidity measurement
#define EP_OCCUPANCY 4  // Occupancy sensing (mmWave)
#define EP_AUDIO     5  // Analog output -> MP3 file index
#define EP_OTA       6  // Analog output -> OTA trigger (1=firmware, 2=sounds, 3=both)
#define EP_RADAR     7  // Custom cluster 0xFC10 -> LD2410 radar configuration + live data

// The light endpoint (EP1) also carries the device's Basic cluster. The Arduino
// Zigbee library can set manufacturer/model/power-source but has NO setter for
// the ZCL swBuildId (0x4000) string that z2m shows as "Firmware rev.". Since
// _cluster_list is protected, a thin subclass can add that attribute itself,
// so we don't have to patch the vendored core library.
class SmartSwitchLight : public ZigbeeColorDimmableLight {
public:
  explicit SmartSwitchLight(uint8_t endpoint) : ZigbeeColorDimmableLight(endpoint) {}

  // Add the swBuildId string to the Basic cluster. Must be called before
  // Zigbee.begin() (attributes can only be added while the cluster is built).
  bool setSoftwareBuildId(const char *swBuildId) {
    esp_zb_attribute_list_t *basic_cluster = esp_zb_cluster_list_get_cluster(
        _cluster_list, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (basic_cluster == nullptr) {
      return false;
    }
    // ZCL character string layout: first byte = length, then the characters.
    size_t len = strlen(swBuildId);
    if (len > 32) {
      len = 32;
    }
    char zcl_str[34];
    zcl_str[0] = static_cast<char>(len);
    memcpy(zcl_str + 1, swBuildId, len);
    zcl_str[len + 1] = '\0';
    return esp_zb_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
                                   ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID,
                                   ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
                                   ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                   (void *)zcl_str) == ESP_OK;
  }

  // Add the ZCL DateCode string (Basic cluster attr 0x0006) — the firmware build date,
  // which z2m shows as the "Firmware build date" (like Aqara/Philips "20161128"). Must be
  // called before Zigbee.begin(). Format is conventionally "YYYYMMDD".
  bool setDateCode(const char *dateCode) {
    esp_zb_attribute_list_t *basic_cluster = esp_zb_cluster_list_get_cluster(
        _cluster_list, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (basic_cluster == nullptr) {
      return false;
    }
    // ZCL DateCode is a character string (max 16 chars): first byte = length, then chars.
    size_t len = strlen(dateCode);
    if (len > 16) {
      len = 16;
    }
    char zcl_str[18];
    zcl_str[0] = static_cast<char>(len);
    memcpy(zcl_str + 1, dateCode, len);
    zcl_str[len + 1] = '\0';
    return esp_zb_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
                                   ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID,
                                   ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
                                   ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                   (void *)zcl_str) == ESP_OK;
  }
};

// Singleton facade around the Arduino Zigbee library.
// Keeps the same callback-based interface Control already uses, so the rest
// of the firmware barely changes.
class ZigbeeManager {
  // state, red, green, blue, level  (library converts x/y & hue/sat to RGB)
  typedef std::function<void(bool state, uint8_t r, uint8_t g, uint8_t b, uint8_t level)> CallbackFunctionLight;
  typedef std::function<void(uint8_t i)> CallbackFunctionAudio;
  typedef std::function<void(uint8_t artifacts)> CallbackFunctionOta;
  typedef std::function<void()> CallbackFunctionVoid;
  typedef std::function<void(uint16_t attr, const uint8_t* data, uint8_t len)> CallbackFunctionRadar;

public:
  static ZigbeeManager& instance() {
    static ZigbeeManager instance;
    return instance;
  }

  void initialize(CallbackFunctionLight light_handler,
                  CallbackFunctionAudio audio_handler,
                  CallbackFunctionOta ota_handler,
                  CallbackFunctionRadar radar_handler,
                  CallbackFunctionVoid connected_handler,
                  CallbackFunctionVoid disconnected_handler);

  // Poll from the main loop() to emit connected/disconnected transitions,
  // since the Arduino library reports link status by polling, not callbacks.
  void loop();

  void network_reset();

  // Button -> coordinator (bound). Interpreted as an "action" by zigbee2mqtt.
  void send_command_toggle();
  void send_command_off();

  // Sensor values pushed up to the coordinator.
  void send_attribute_occupancy(bool occupancy);
  void send_attribute_temperature(float temperature);
  void send_attribute_humidity(float humidity);

  // Radar endpoint: Radar publishes its configuration/live data through this.
  ZigbeeRadar& radar() { return _radar; }

private:
  ZigbeeManager()
    : _light(EP_LIGHT), _switch(EP_SWITCH), _sensor(EP_SENSOR),
      _occupancy(EP_OCCUPANCY), _audio(EP_AUDIO), _ota(EP_OTA), _radar(EP_RADAR) {}

  // Endpoint objects: each one is its own Zigbee endpoint.
  SmartSwitchLight         _light;
  ZigbeeSwitch             _switch;
  ZigbeeTempSensor         _sensor;   // temperature (+ humidity added on)
  ZigbeeOccupancySensor    _occupancy;
  ZigbeeAnalog             _audio;
  ZigbeeAnalog             _ota;       // OTA trigger (analog output written by z2m)
  ZigbeeRadar              _radar;     // LD2410 configuration (custom cluster)

  bool _was_connected = false;

  CallbackFunctionLight light_cb;
  CallbackFunctionAudio audio_cb;
  CallbackFunctionOta   ota_cb;
  CallbackFunctionRadar radar_cb;
  CallbackFunctionVoid  connected_cb;
  CallbackFunctionVoid  disconnected_cb;

  // The library uses plain C function pointers (no captures), so these
  // static thunks forward into the singleton's std::function callbacks.
  static void on_light(bool state, uint8_t r, uint8_t g, uint8_t b, uint8_t level);
  static void on_audio(float value);
  static void on_ota(float value);
  static void on_radar_write(uint16_t attr, const uint8_t* data, uint8_t len);
};
