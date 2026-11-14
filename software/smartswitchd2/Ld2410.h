#pragma once

#include <Arduino.h>

// Minimal driver for the Hi-Link LD2410(B/C) 24 GHz radar over UART.
// Replaces the ld2410 library (v0.1.3), whose 40-byte frame buffer cannot hold the
// 45-byte engineering frames and which has no resolution/bluetooth/firmware commands.
//
// Protocol (verified on the bench against firmware V2.04.23022511):
//   command / ACK frame: FD FC FB FA | len(2) | cmd(2) | value | 04 03 02 01
//   data frame:          F4 F3 F2 F1 | len(2) | type | AA | ... | 55 00 | F8 F7 F6 F5
// ACKs carry cmd | 0x0100 followed by a 2-byte status (0 = success).
// Commands are blocking (the radar answers within a few ms); call them from loop(),
// never from a Zigbee callback.
class Ld2410 {
public:
  static constexpr uint8_t GATES = 9;  // gates 0..8

  struct Config {
    uint8_t maxMovingGate = 8;   // 2..8
    uint8_t maxStaticGate = 8;   // 2..8
    uint16_t noOneDuration = 5;  // seconds the radar keeps reporting presence after the last target
    uint8_t moving[GATES] = {};  // per-gate moving threshold 0..100
    uint8_t stat[GATES] = {};    // per-gate static threshold 0..100
  };

  struct Target {
    uint8_t state = 0;            // 0 none, 1 moving, 2 static, 3 both
    uint16_t movingDistance = 0;  // cm
    uint8_t movingEnergy = 0;
    uint16_t staticDistance = 0;  // cm
    uint8_t staticEnergy = 0;
    uint16_t detectionDistance = 0;  // cm
    // Engineering mode only:
    bool engineering = false;
    uint8_t movingGates[GATES] = {};
    uint8_t staticGates[GATES] = {};
    uint8_t light = 0;
    uint8_t out = 0;
  };

  enum Resolution : uint8_t { RESOLUTION_75CM = 0, RESOLUTION_20CM = 1 };

  void begin(HardwareSerial& uart, int rx_pin, int tx_pin);

  // Parse incoming bytes. Returns true when a new data frame was decoded.
  bool loop();

  // A data frame arrived within the last second.
  bool connected() const;
  bool presence() const { return _target.state != 0; }
  const Target& target() const { return _target; }
  // Diagnostics: raw bytes received and frames decoded since boot.
  uint32_t bytesReceived() const { return _bytes; }
  uint32_t framesReceived() const { return _frames; }
  // When the last byte of any kind (data frame or ACK) came in; 0 = never.
  unsigned long lastByteAt() const { return _last_byte; }

  bool readConfig(Config& config);
  // Writes only what differs from `current`.
  bool writeConfig(const Config& wanted, const Config& current);
  bool readFirmware(char* out, size_t len);
  bool readResolution(uint8_t& resolution);
  bool setResolution(uint8_t resolution);  // takes effect after restart()
  bool setBluetooth(bool on);              // takes effect after restart()
  bool setEngineering(bool on);
  bool restart();                          // radar is unresponsive for ~1.1 s afterwards
  bool factoryReset();                     // takes effect after restart()

private:
  static constexpr size_t MAX_FRAME = 64;

  HardwareSerial* _uart = nullptr;
  uint8_t _frame[MAX_FRAME];
  size_t _pos = 0;
  size_t _expected = 0;
  unsigned long _last_data = 0;
  unsigned long _last_byte = 0;
  uint32_t _bytes = 0;
  uint32_t _frames = 0;
  Target _target;

  uint16_t _ack_cmd = 0;
  uint8_t _ack[MAX_FRAME];
  size_t _ack_len = 0;
  bool _ack_ready = false;

  bool feed(uint8_t b);
  bool parseFrame();
  bool parseData(const uint8_t* p, size_t len);

  // Sends one command and waits for its ACK. `ack` receives the bytes after the status.
  bool command(uint16_t cmd, const uint8_t* value = nullptr, size_t value_len = 0,
               uint8_t* ack = nullptr, size_t* ack_len = nullptr, uint32_t timeout_ms = 300);
  bool enterConfig();
  bool leaveConfig();
  // Runs `cmd` inside an enter/leave config-mode pair.
  bool configCommand(uint16_t cmd, const uint8_t* value = nullptr, size_t value_len = 0,
                     uint8_t* ack = nullptr, size_t* ack_len = nullptr);
};
