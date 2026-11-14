#pragma once

#include "Button.h"
#include "Led.h"
#include "DigitalLed.h"
#include "Sensor.h"
#include "ZigbeeManager.h"
#include "Mmwave.h"
#include "Radar.h"
#include "Audio.h"

#define BUTTON_PIN 23
#define DIGITAL_LED_PIN 18
#define OCCUPANCY_TIMEOUT 32000 // 32 seconds

// MAX98357A GAIN_SLOT: floating = 9dB for the boot sound, then held LOW = 12dB (the loudest
// setting reachable from a GPIO). The amp runs on 5V, so a 3.3V "high" would land between
// gain windows: never drive it high.
#define AMP_GAIN_PIN 1

class Control {
public:
  Control();
  void initialize();
  void loop();
  Sensor& getSensor();
private:
  Button* button;
  DigitalLed* digital_led;
  Led led;
  Sensor sensor;
  // Mmwave mmwave;
  Radar radar;
  Audio audio;

  bool connected;
  bool occupancy;
  unsigned long occupancy_timer;
  bool boot_sound_playing = false;  // amp stays at 9dB until the boot sound has finished

  // Deferred OTA reboot: a Zigbee OTA trigger must NOT reboot inside the Zigbee callback,
  // or the ZCL write-response ACK never reaches z2m ("Publish 'set' 'update' failed").
  // Instead we schedule the reboot ~1s later from loop(), after the ACK has gone out.
  unsigned long ota_reboot_at = 0;
  uint8_t ota_artifacts = 0;
  // OTA "update being installed" sound: after the ACK window we play the sound once and
  // only reboot into the updater when it has finished (or a safety cap elapses), so the
  // user always hears the full sound before the device drops off for the WiFi update.
  bool ota_sound_started = false;
  unsigned long ota_sound_deadline = 0;

  void buttonHoldHandler();
  void buttonPressedHandler();

  // Bench-only serial commands (compiled in only when DEBUG_SERIAL=1). Lets us
  // provision WiFi creds into NVS and trigger an OTA without the z2m cluster yet:
  //   wifi <ssid> <pass>   -> store WiFi credentials in NVS
  //   ota <url>            -> request a SPIFFS OTA from <url> (reboots to updater)
  //   otaapp <url>         -> request a firmware OTA from <url>
  //   play <index> [vol]   -> play MP3 file <index> (see Audio.h) at volume 0-100
  //   radar                -> print LD2410 radar status
  //   radar live on|off    -> start/stop the live gate-energy reports (as from Zigbee)
  //   radar set <moving|static|all> <v>|<v0..v8> -> set thresholds (as from Zigbee)
  //   radar restart|refresh -> restart the radar module / read it again (as from Zigbee)
  //   radar nvs            -> print the settings copy stored on the ESP
  void handleSerialCommands();
  void processSerialCommand(const String& line);

  void zigbeeConnectedHandler();
  void zigbeeDisconnectedHandler();

  void zigbeeLightChangeHandler(bool state, uint8_t r, uint8_t g, uint8_t b, uint8_t level);
  void zigbeePlayAudioHandler(uint8_t i);
  // OTA trigger over Zigbee: artifacts bitmask (1=firmware, 2=sounds, 3=both).
  void zigbeeOtaHandler(uint8_t artifacts);
  // Radar cluster write (Zigbee task): forwarded to Radar, applied from loop().
  void zigbeeRadarHandler(uint16_t attr, const uint8_t* data, uint8_t len);

  void sensorValueUpdateHandler();
  // Read the BME280 and push temperature/humidity to Zigbee, skipping invalid reads.
  void pushSensorValues();

  // void mmwaveValueChangedHandler(int16_t x, int16_t y);
  void mmwaveValueChangedHandler(bool o);
};
