#include "Debug.h"
#include "Control.h"
#include "OtaManager.h"

Control::Control()
  : connected(false), occupancy(false) {
  button = new Button(BUTTON_PIN);
  digital_led = new DigitalLed(DIGITAL_LED_PIN);
  sensor = Sensor();
  audio = Audio();
}

void Control::initialize() {
    button->initialize(
        std::bind(&Control::buttonPressedHandler, this),
        std::bind(&Control::buttonHoldHandler, this)
    );

  digital_led->initialize();

  // Start at 9dB (GAIN floating) for the boot sound: on USB power the boot sound at 12dB,
  // on top of everything starting up, browned the switch out. loop() switches to 12dB once
  // the boot sound has finished. (The 230V supply has never had this problem.)
  pinMode(AMP_GAIN_PIN, INPUT);

  sensor.initialize(
    std::bind(&Control::sensorValueUpdateHandler, this));

  // mmwave.initialize(
  //   std::bind(&Control::mmwaveValueChangedHandler, this, std::placeholders::_1, std::placeholders::_2)
  // );

  radar.initialize(
    std::bind(&Control::mmwaveValueChangedHandler, this, std::placeholders::_1));

  audio.initialize();

  ZigbeeManager::instance().initialize(
    std::bind(&Control::zigbeeLightChangeHandler, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5),
    std::bind(&Control::zigbeePlayAudioHandler, this, std::placeholders::_1),
    std::bind(&Control::zigbeeOtaHandler, this, std::placeholders::_1),
    std::bind(&Control::zigbeeRadarHandler, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3),
    std::bind(&Control::zigbeeConnectedHandler, this),
    std::bind(&Control::zigbeeDisconnectedHandler, this));

  zigbeeDisconnectedHandler();

  // Seed the temperature/humidity attributes with a real reading right after the stack
  // starts, so a read during interview isn't the 0 default. (Skipped if the BME isn't
  // ready yet; the push on connect below is the reliable path.)
  pushSensorValues();

  audio.play(19, 100);
  boot_sound_playing = true;
}

void Control::loop() {
  button->loop();
  digital_led->loop();
  sensor.loop();
  radar.loop();
  ZigbeeManager::instance().loop();

  if (boot_sound_playing && !audio.isBusy()) {
    boot_sound_playing = false;
    pinMode(AMP_GAIN_PIN, OUTPUT);
    digitalWrite(AMP_GAIN_PIN, LOW);  // 12dB from now on
  }

#if DEBUG_SERIAL
  handleSerialCommands();
#endif

  // OTA sequence (see zigbeeOtaHandler). Once the ACK window has passed we play the
  // "update being installed" sound and only reboot into the updater after it has finished
  // playing (or a safety cap elapses), so the sound is always heard in full first.
  if (ota_artifacts != 0) {
    if (!ota_sound_started) {
      if (ota_reboot_at != 0 && millis() >= ota_reboot_at) {
        ota_reboot_at = 0;
        audio.play(AUDIO_UPDATE_INSTALLING, 100);
        ota_sound_started = true;
        ota_sound_deadline = millis() + 8000; // safety cap: never wait forever on audio
      }
    } else if (!audio.isBusy() || millis() >= ota_sound_deadline) {
      uint8_t artifacts = ota_artifacts;
      ota_artifacts = 0;
      OtaManager::instance().requestOta(artifacts, "", ""); // reboots into the updater
    }
  }

  if (occupancy && occupancy_timer + OCCUPANCY_TIMEOUT < millis()) {
    mmwaveValueChangedHandler(false);
  }
}

// --- Bench-only serial commands (see Control.h). Compiled in via loop() only when
//     DEBUG_SERIAL=1; harmless to keep, remove the loop() call for production. ---
void Control::handleSerialCommands() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length()) {
        processSerialCommand(line);
        line = "";
      }
    } else {
      line += c;
    }
  }
}

void Control::processSerialCommand(const String& line) {
  if (line.startsWith("wifi ")) {
    String rest = line.substring(5);
    rest.trim();
    int sp = rest.indexOf(' ');
    if (sp < 0) {
      DBG_PRINTLN("[CMD] usage: wifi <ssid> <pass>");
      return;
    }
    String ssid = rest.substring(0, sp);
    String pass = rest.substring(sp + 1);
    OtaManager::instance().setWifiCredentials(ssid.c_str(), pass.c_str());
    DBG_PRINTF("[CMD] WiFi creds saved to NVS (ssid='%s', pass length=%d)\n",
               ssid.c_str(), (int)pass.length());
  } else if (line.startsWith("otaapp ")) {
    String url = line.substring(7);
    url.trim();
    DBG_PRINTF("[CMD] requesting firmware OTA from %s\n", url.c_str());
    OtaManager::instance().requestOta(OtaManager::ARTIFACT_APP, url.c_str(), "");
  } else if (line.startsWith("ota ")) {
    String url = line.substring(4);
    url.trim();
    DBG_PRINTF("[CMD] requesting SPIFFS OTA from %s\n", url.c_str());
    OtaManager::instance().requestOta(OtaManager::ARTIFACT_SPIFFS, url.c_str(), "");
  } else if (line == "radar") {
    radar.printStatus();
  } else if (line == "radar live on" || line == "radar live off") {
    // Same path as a Zigbee write, so the live reporting can be tested on the bench.
    const uint8_t on = line.endsWith("on") ? 1 : 0;
    radar.handleZigbeeWrite(RadarAttr::LIVE, &on, 1);
  } else if (line.startsWith("radar set ")) {
    // radar set <moving|static|all> <v0> [v1 .. v8] - one value sets every gate.
    // Same path as a Zigbee write from the web UI.
    String rest = line.substring(10);
    rest.trim();
    int sp = rest.indexOf(' ');
    String which = sp < 0 ? rest : rest.substring(0, sp);
    const char* p = sp < 0 ? "" : rest.c_str() + sp + 1;
    uint8_t v[RADAR_GATES];
    int n = 0;
    char* end = nullptr;
    while (n < RADAR_GATES) {
      long x = strtol(p, &end, 10);
      if (end == p) break;
      v[n++] = (uint8_t)constrain(x, 0, 100);
      p = end;
    }
    if (n == 1) memset(v, v[0], RADAR_GATES);
    if ((n != 1 && n != RADAR_GATES) || (which != "moving" && which != "static" && which != "all")) {
      DBG_PRINTLN("[CMD] usage: radar set <moving|static|all> <v> | <v0..v8>");
      return;
    }
    if (which != "static") radar.handleZigbeeWrite(RadarAttr::MOVING_SENSITIVITY, v, RADAR_GATES);
    if (which != "moving") radar.handleZigbeeWrite(RadarAttr::STATIC_SENSITIVITY, v, RADAR_GATES);
    DBG_PRINTF("[CMD] radar %s thresholds queued\n", which.c_str());
  } else if (line == "radar restart" || line == "radar refresh") {
    const uint8_t cmd = line.endsWith("restart") ? 1 : 3;
    radar.handleZigbeeWrite(RadarAttr::COMMAND, &cmd, 1);
  } else if (line == "radar nvs") {
    radar.printStorage();
  } else if (line.startsWith("play ")) {
    int index = 0, volume = 100;
    sscanf(line.c_str() + 5, "%d %d", &index, &volume);
    DBG_PRINTF("[CMD] play %d at volume %d\n", index, volume);
    audio.play(index, constrain(volume, 0, 100));
  } else {
    DBG_PRINTF("[CMD] unknown command: %s\n", line.c_str());
  }
}

Sensor& Control::getSensor() {
  return sensor;
}

void Control::buttonHoldHandler() {
  ZigbeeManager::instance().network_reset();
}

void Control::buttonPressedHandler() {
    // Queue the local click sound FIRST. audio.play() is non-blocking (it just hands the
    // file index to the audio task), whereas the Zigbee toggle below acquires the stack
    // lock (esp_zb_lock_acquire(portMAX_DELAY), blocking) and can stall this handler.
    // Doing the send first meant the lamp (a full network round-trip away) could react
    // before the local sound had even started. Queue the sound first so it always leads.
    if (random(0, 10) == 0) {
        // 10% chance (1 in 10) -> Play the special sound
        audio.play(7, 100);
    }
    else {
        // 90% chance -> Play the standard sound
        audio.play(6, 100);
    }

    if (connected) {
        ZigbeeManager::instance().send_command_toggle();
    }
}

void Control::sensorValueUpdateHandler() {
  if (!connected) return;
  pushSensorValues();
}

// Read the BME280 and push temperature/humidity. Skips invalid reads (NaN or out of
// the sensor's range) so we never publish the boot-time 0 default or a failed read.
// Not gated on `connected`: setting the attribute before joining seeds it so the very
// first value z2m reads/reports is real instead of 0.
void Control::pushSensorValues() {
  float t = sensor.getTemperature();
  float h = sensor.getHumidity();
  if (isnan(t) || isnan(h)) return;
  if (t < -40.0f || t > 85.0f || h < 0.0f || h > 100.0f) return;
  ZigbeeManager::instance().send_attribute_temperature(t);
  ZigbeeManager::instance().send_attribute_humidity(h);
}

// void Control::mmwaveValueChangedHandler(int16_t x, int16_t y)
// {
//   DBG_PRINTF("Mmwave values (%d, %d)\n", x, y);
// }

void Control::mmwaveValueChangedHandler(bool o) {
  if (!connected) return;

  //if (!occupancy && o) audio.play(5, 100);

  occupancy = o;
  occupancy_timer = millis();

  ZigbeeManager::instance().send_attribute_occupancy(occupancy);
}

void Control::zigbeeLightChangeHandler(bool state, uint8_t r, uint8_t g, uint8_t b, uint8_t level) {
  digital_led->setBlinking(false);
  // One call: setRGB() followed by setLevel() briefly showed the colour at full brightness.
  digital_led->setColor(r, g, b, state ? level : 0);
  DBG_PRINTF("set light state:%d rgb(%d,%d,%d) level:%d\n", state, r, g, b, level);
}

void Control::zigbeePlayAudioHandler(uint8_t i) {
  if (i == 0) {
    audio.stop();
    return;
  }

  audio.play(i, 75);
}

void Control::zigbeeOtaHandler(uint8_t artifacts) {
  // 0 = no-op (e.g. a reset/echo of the attribute). 1=firmware, 2=sounds, 3=both.
  if (artifacts == 0) {
    return;
  }
  DBG_PRINTF("Zigbee OTA trigger: artifacts=%d (reboot deferred ~1s for the ACK)\n", artifacts);
  // Do NOT reboot here (inside the Zigbee callback) — schedule it so the write-response
  // ACK reaches z2m first. loop() performs the actual requestOta() when the timer expires.
  ota_artifacts = artifacts;
  ota_reboot_at = millis() + 1000;
}

void Control::zigbeeRadarHandler(uint16_t attr, const uint8_t* data, uint8_t len) {
  radar.handleZigbeeWrite(attr, data, len);
}

void Control::zigbeeConnectedHandler() {
  connected = true;
  DBG_PRINTLN("Zigbee connected");
  digital_led->setBlinking(false);
  digital_led->setRGB(0, 70, 0);
  digital_led->setOffTimeout(4000);

  // A successful Zigbee join is our "healthy" signal: if this boot is a freshly
  // OTA-flashed image still pending verification, confirm it now so the bootloader
  // keeps it instead of rolling back on the next reset.
  OtaManager::instance().markAppValidIfNeeded();

  // Push a real reading immediately so the first temperature/humidity report after
  // (re)connect is the actual value, not the 0 the attribute defaults to at boot.
  pushSensorValues();
  radar.publishAll();
}

void Control::zigbeeDisconnectedHandler() {
  connected = false;
  DBG_PRINTLN("Zigbee disconnected");
  digital_led->setBlinking(true);
  digital_led->setRGB(70, 0, 0);
}