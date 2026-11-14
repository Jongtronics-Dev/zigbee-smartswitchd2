#pragma once

#include <Arduino.h>
#include <Zigbee.h>

// Custom (manufacturer-range) cluster that exposes the LD2410 radar configuration and
// live data to zigbee2mqtt. MUST stay in sync with the ssd2Radar cluster in smartswitch.mjs.
#define RADAR_CLUSTER_ID 0xFC10

namespace RadarAttr {
  constexpr uint16_t MOVING_SENSITIVITY = 0x0000;  // octet string[9], per-gate threshold 0..100
  constexpr uint16_t STATIC_SENSITIVITY = 0x0001;  // octet string[9]
  constexpr uint16_t MAX_MOVING_GATE    = 0x0002;  // uint8 2..8
  constexpr uint16_t MAX_STATIC_GATE    = 0x0003;  // uint8 2..8
  constexpr uint16_t NO_ONE_DURATION    = 0x0004;  // uint16 seconds
  constexpr uint16_t RESOLUTION         = 0x0005;  // uint8: 0 = 0.75 m/gate, 1 = 0.20 m/gate
  constexpr uint16_t BLUETOOTH          = 0x0006;  // bool (auto-off after a timeout)
  constexpr uint16_t LIVE               = 0x0007;  // bool: stream LIVE_DATA (auto-off after a timeout)
  constexpr uint16_t LIVE_DATA          = 0x0008;  // octet string[LIVE_DATA_SIZE], see Radar.cpp
  // char string: radar firmware version, "" = fitted but not answering (or answering but sending
  // no measurements), "none" = no radar fitted.
  // (A separate "present" attribute made the stack assert on report; this field already reports.)
  constexpr uint16_t FIRMWARE           = 0x0009;
  constexpr uint16_t COMMAND            = 0x000A;  // uint8 write: 1 restart, 2 factory reset, 3 refresh
}

constexpr uint8_t RADAR_GATES = 9;
constexpr uint8_t RADAR_LIVE_DATA_SIZE = 29;
constexpr uint8_t RADAR_FIRMWARE_MAX = 20;

class ZigbeeRadar : public ZigbeeEP {
public:
  // Called from the Zigbee task: copy the data and return, never block here.
  typedef void (*WriteCallback)(uint16_t attr, const uint8_t* data, uint8_t len);

  explicit ZigbeeRadar(uint8_t endpoint);

  void onWrite(WriteCallback cb) { _on_write = cb; }

  // Update the local attribute and report it to the coordinator.
  bool setOctets(uint16_t attr, const uint8_t* data, uint8_t len);
  bool setString(uint16_t attr, const char* value);
  bool setU8(uint16_t attr, uint8_t value);
  bool setU16(uint16_t attr, uint16_t value);
  bool setBool(uint16_t attr, bool value);

private:
  WriteCallback _on_write = nullptr;

  bool report(uint16_t attr);
  void zbAttributeSet(const esp_zb_zcl_set_attr_value_message_t* message) override;
};
