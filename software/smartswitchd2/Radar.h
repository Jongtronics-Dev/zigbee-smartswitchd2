#pragma once

#include <Arduino.h>
#include <functional>
#include "Ld2410.h"
#include "ZigbeeRadar.h"

#define RADAR_RX_PIN 17
#define RADAR_TX_PIN 16

// LD2410 presence radar: occupancy for Zigbee plus remote configuration.
//
// The radar stores its own settings, but a copy is kept in NVS so the ESP can restore
// it at boot if the radar was factory-reset or swapped. Bluetooth (HLKRadarTool) is
// kept off and can be switched on via Zigbee for BLUETOOTH_TIMEOUT; when it goes off
// again the settings made in the app are read back and become the new NVS copy.
class Radar {
  typedef std::function<void(bool occupied)> CallbackFunction;

public:
  void initialize(CallbackFunction presence_handler);
  void loop();

  // Push all configuration attributes to Zigbee (call on (re)connect).
  void publishAll();

  // Zigbee write on the radar cluster. Runs in the Zigbee task: only records the request.
  void handleZigbeeWrite(uint16_t attr, const uint8_t* data, uint8_t len);

  // Bench diagnostics (serial "radar" / "radar nvs" commands).
  void printStatus();
  void printStorage();

private:
  Ld2410 radar;
  Ld2410::Config config;  // as last read back from the radar
  uint8_t resolution = 0;
  bool bluetooth = false;
  bool live = false;
  char firmware[RADAR_FIRMWARE_MAX + 1] = "";
  bool ready = false;   // config successfully read from the radar
  bool synced = false;  // boot-time NVS restore done
  bool unavailable_reported = false;  // "not responding" already sent to Zigbee
  // Answers commands but sends no data frames (a loose wire, or stuck in config mode):
  // reported as "not responding" too, since there is nothing to measure.
  bool silent = false;
  unsigned long quiet_since = 0;       // start of the current wait for a data frame, 0 = none
  bool silent_restarted = false;       // the one quiet restart that often fixes it was tried
  unsigned long silent_retry_at = 0;   // while silent: next restart attempt

  // Whether an LD2410 is fitted at all: decided at boot, and again when one is unplugged
  // (nothing at all comes in any more) or plugged in (data frames start). The switch works
  // without one; it then has no occupancy.
  enum class Presence : uint8_t { DETECTING, ABSENT, PRESENT };
  Presence presence = Presence::DETECTING;
  uint8_t detect_attempts = 0;
  uint32_t frames_when_absent = 0;  // frames received when it was found absent
  bool reapply_bluetooth = false;  // after a factory reset

  unsigned long refresh_at = 0;  // 0 = no refresh scheduled
  bool adopt_on_refresh = false; // treat the radar's settings as truth on the next refresh
  unsigned long live_until = 0;
  unsigned long bluetooth_until = 0;
  unsigned long last_live_report = 0;

  CallbackFunction presence_cb;
  bool last_presence = false;
  unsigned long last_presence_check = 0;
  unsigned long last_presence_report = 0;

  // Requests from the Zigbee task, applied in loop().
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  struct Pending {
    bool config = false;
    Ld2410::Config wanted;
    int16_t resolution = -1;
    int8_t bluetooth = -1;
    int8_t live = -1;
    uint8_t command = 0;
    unsigned long changed_at = 0;
  } pending;

  void scheduleRefresh(unsigned long delay_ms, bool adopt = false);
  void refresh();
  void syncWithStorage();
  void applyPending();
  void restartRadar(bool adopt = false);
  void setLive(bool on);
  void setBluetooth(bool on);
  void handlePresence();
  void checkStreaming();
  void checkUnplugged();
  void checkPluggedIn();
  void markAbsent();
  void reportLive();

  void saveConfig();
  void publishConfig();
  static void printConfig(const char* label, const Ld2410::Config& c, uint8_t res);
};
