#include "Debug.h"
#include "Radar.h"
#include "ZigbeeManager.h"
#include <Preferences.h>

static constexpr unsigned long PRESENCE_CHECK_INTERVAL = 10;          // ms
static constexpr unsigned long PRESENCE_FORCED_INTERVAL = 30000;      // re-send occupancy every 30 s
static constexpr unsigned long RETRY_INTERVAL = 5000;                 // radar not answering
static constexpr uint8_t DETECT_ATTEMPTS = 4;                         // at boot: is a radar fitted?
static constexpr unsigned long DETECT_RETRY = 1000;
static constexpr unsigned long APPLY_DEBOUNCE = 300;                  // z2m writes one attribute per key
static constexpr unsigned long RESTART_SETTLE = 1500;                 // radar needs ~1.1 s after a restart
static constexpr unsigned long LIVE_TIMEOUT = 5UL * 60 * 1000;        // front-end renews while it is open
static constexpr unsigned long LIVE_INTERVAL = 500;                   // live data report rate
static constexpr unsigned long BLUETOOTH_TIMEOUT = 15UL * 60 * 1000;  // HLKRadarTool session length
static constexpr unsigned long NO_DATA_TIMEOUT = 5000;                // radar streams ~10 frames/s
static constexpr unsigned long SILENT_RETRY = 60000;                  // restart a silent radar this often
// Nothing at all from the radar this long - no data frame and no ACK - and it is gone. Longer
// than SILENT_RETRY, so a radar that only answers the restart it gets every minute is still
// "not responding", not "not fitted".
static constexpr unsigned long UNPLUGGED_TIMEOUT = 75000;

static const char* NVS_NAMESPACE = "radar";

// millis() deadline check that survives the 49-day wrap.
static bool due(unsigned long deadline) {
  return (long)(millis() - deadline) >= 0;
}

static bool sameConfig(const Ld2410::Config& a, const Ld2410::Config& b) {
  return a.maxMovingGate == b.maxMovingGate && a.maxStaticGate == b.maxStaticGate &&
         a.noOneDuration == b.noOneDuration && memcmp(a.moving, b.moving, Ld2410::GATES) == 0 &&
         memcmp(a.stat, b.stat, Ld2410::GATES) == 0;
}

static void clampConfig(Ld2410::Config& c) {
  c.maxMovingGate = constrain(c.maxMovingGate, 2, 8);
  c.maxStaticGate = constrain(c.maxStaticGate, 2, 8);
  for (uint8_t i = 0; i < Ld2410::GATES; i++) {
    c.moving[i] = min<uint8_t>(c.moving[i], 100);
    c.stat[i] = min<uint8_t>(c.stat[i], 100);
  }
}

void Radar::initialize(CallbackFunction presence_handler) {
  presence_cb = presence_handler;
  radar.begin(Serial1, RADAR_RX_PIN, RADAR_TX_PIN);
  // First contact from loop(), once the radar has booted and Zigbee is up.
  scheduleRefresh(500);
}

void Radar::loop() {
  radar.loop();
  handlePresence();
  checkStreaming();
  checkUnplugged();
  checkPluggedIn();

  if (refresh_at && due(refresh_at)) {
    refresh_at = 0;
    refresh();
  }

  unsigned long changed_at;
  portENTER_CRITICAL(&mux);
  changed_at = pending.changed_at;
  portEXIT_CRITICAL(&mux);
  if (changed_at && millis() - changed_at >= APPLY_DEBOUNCE) applyPending();

  if (live && due(live_until)) {
    DBG_PRINTLN("[Radar] live mode timed out");
    setLive(false);
  }
  if (bluetooth && due(bluetooth_until)) {
    DBG_PRINTLN("[Radar] bluetooth timed out");
    setBluetooth(false);
  }
  if (live) reportLive();
}

void Radar::handlePresence() {
  if (millis() - last_presence_check < PRESENCE_CHECK_INTERVAL || !radar.connected()) return;
  last_presence_check = millis();

  bool value = radar.presence();
  if (value != last_presence || millis() - last_presence_report > PRESENCE_FORCED_INTERVAL) {
    last_presence = value;
    last_presence_report = millis();
    presence_cb(value);
  }
}

// A radar that answers commands can still send no measurements: a loose RX wire that only
// passes the short ACKs, or a radar left in config mode by a lost "end config". Presence then
// freezes without anything failing, so watch the data frames themselves. The first time it
// happens, restart the radar quietly (that gets it out of config mode); if that does not bring
// the frames back, report "not responding" and keep restarting it every SILENT_RETRY.
void Radar::checkStreaming() {
  if (!ready) return;  // not answering at all: refresh() reports and retries that
  if (radar.connected()) {
    quiet_since = 0;
    silent_restarted = false;
    if (silent) {
      DBG_PRINTLN("[Radar] measurements are back");
      silent = false;
      publishAll();
    }
    return;
  }
  // A restart or refresh is under way: the radar is quiet on purpose.
  if (refresh_at) {
    quiet_since = 0;
    return;
  }
  if (!quiet_since) {
    quiet_since = millis() | 1;
    return;
  }
  if (millis() - quiet_since < NO_DATA_TIMEOUT) return;
  quiet_since = 0;

  if (!silent_restarted) {
    DBG_PRINTLN("[Radar] no measurements, restarting radar");
    silent_restarted = true;
    restartRadar();
    return;
  }
  if (!silent) {
    DBG_PRINTLN("[Radar] radar answers but sends no measurements");
    silent = true;
    silent_retry_at = millis() + SILENT_RETRY;
    publishAll();
    return;
  }
  if (due(silent_retry_at)) {
    silent_retry_at = millis() + SILENT_RETRY;
    restartRadar();
  }
}

// A radar that was there and now sends nothing at all - not a data frame, not an ACK to the
// restarts and re-reads that keep coming - has been unplugged (or is dead): report "not fitted",
// like a switch that never had one, and stop occupancy.
void Radar::checkUnplugged() {
  if (presence != Presence::PRESENT) return;
  unsigned long last = radar.lastByteAt();
  if (last && millis() - last < UNPLUGGED_TIMEOUT) return;
  DBG_PRINTLN("[Radar] nothing from the radar any more, taking it as unplugged");
  markAbsent();
}

// Not fitted, and data frames start coming in: a radar was plugged in. Look at it properly.
void Radar::checkPluggedIn() {
  if (presence != Presence::ABSENT || radar.framesReceived() == frames_when_absent) return;
  DBG_PRINTLN("[Radar] data frames again, looking for the radar");
  presence = Presence::DETECTING;
  detect_attempts = 0;
  scheduleRefresh(DETECT_RETRY);
}

void Radar::markAbsent() {
  presence = Presence::ABSENT;
  frames_when_absent = radar.framesReceived();
  ready = false;
  live = false;
  silent = false;
  silent_restarted = false;
  quiet_since = 0;
  refresh_at = 0;
  unavailable_reported = false;
  firmware[0] = '\0';
  // No radar, no occupancy: clear one that was left standing.
  if (last_presence) {
    last_presence = false;
    presence_cb(false);
  }
  publishAll();
}

void Radar::scheduleRefresh(unsigned long delay_ms, bool adopt) {
  refresh_at = millis() + delay_ms;
  if (refresh_at == 0) refresh_at = 1;
  adopt_on_refresh |= adopt;
}

// Read everything back from the radar and publish it.
void Radar::refresh() {
  char fw[RADAR_FIRMWARE_MAX + 1];
  Ld2410::Config c;
  uint8_t res = 0;
  if (!radar.readFirmware(fw, sizeof(fw)) || !radar.readConfig(c) || !radar.readResolution(res)) {
    if (presence != Presence::PRESENT) {
      // Detection: a radar that streams data frames is fitted, even if it is not answering yet.
      // New frames, that is: a radar that was unplugged left its count behind.
      if (radar.framesReceived() == frames_when_absent) {
        if (++detect_attempts < DETECT_ATTEMPTS) {
          scheduleRefresh(DETECT_RETRY);
          return;
        }
        // Not fitted: stop trying. Frames coming in, or a "refresh" from Zigbee, detect again.
        DBG_PRINTLN("[Radar] no LD2410 found, radar disabled");
        markAbsent();
        return;
      }
      presence = Presence::PRESENT;
    }
    // Fitted but not answering: report it once, not on every retry; a rejoin re-reports it
    // through publishAll().
    if (ready || !unavailable_reported) {
      DBG_PRINTLN("[Radar] radar not responding");
      ready = false;
      live = false;
      firmware[0] = '\0';
      publishAll();
      unavailable_reported = true;
    }
    scheduleRefresh(RETRY_INTERVAL);
    return;
  }

  strlcpy(firmware, fw, sizeof(firmware));
  portENTER_CRITICAL(&mux);
  config = c;
  portEXIT_CRITICAL(&mux);
  resolution = res;
  presence = Presence::PRESENT;
  ready = true;
  unavailable_reported = false;
  DBG_PRINTF("[Radar] firmware %s, gates %u/%u, no-one %us, resolution %s\n", firmware,
             config.maxMovingGate, config.maxStaticGate, config.noOneDuration, resolution ? "0.20m" : "0.75m");

  if (!synced) {
    synced = true;
    syncWithStorage();
    return;
  }
  if (adopt_on_refresh) {
    adopt_on_refresh = false;
    saveConfig();
  }
  if (reapply_bluetooth) {
    // A factory reset may have switched Bluetooth back to the radar's default (on).
    reapply_bluetooth = false;
    if (radar.setBluetooth(bluetooth)) {
      restartRadar();
      return;
    }
  }
  if (live) radar.setEngineering(true);  // a radar restart drops engineering mode
  publishAll();
}

// Boot-time reconciliation between the radar and the copy in NVS.
void Radar::syncWithStorage() {
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  Ld2410::Config stored;
  bool has_stored = p.getBytes("cfg", &stored, sizeof(stored)) == sizeof(stored);
  uint8_t stored_res = p.getUChar("res", 0xFF);
  bool bt_wanted = p.getBool("bt", false);
  uint8_t bt_applied = p.getUChar("bt_applied", 0xFF);

  if (bt_wanted) {
    // Rebooted during a Bluetooth session: settings made in the app are the truth.
    DBG_PRINTLN("[Radar] bluetooth session interrupted, adopting radar settings");
    has_stored = false;
    stored_res = 0xFF;
    bt_wanted = false;
    p.putBool("bt", false);
  }

  bool restart = false;
  if (has_stored && !sameConfig(stored, config)) {
    DBG_PRINTLN("[Radar] radar settings differ from NVS, restoring");
    radar.writeConfig(stored, config);
    Ld2410::Config c;
    if (radar.readConfig(c)) {
      portENTER_CRITICAL(&mux);
      config = c;
      portEXIT_CRITICAL(&mux);
    }
  }
  if (stored_res <= 1 && stored_res != resolution && radar.setResolution(stored_res)) {
    resolution = stored_res;
    restart = true;
  }
  if (bt_applied != (uint8_t)bt_wanted && radar.setBluetooth(bt_wanted)) {
    DBG_PRINTF("[Radar] bluetooth %s\n", bt_wanted ? "on" : "off");
    p.putUChar("bt_applied", bt_wanted);
    restart = true;
  }
  p.end();
  bluetooth = bt_wanted;
  saveConfig();

  if (restart) {
    restartRadar();
  } else {
    publishAll();
  }
}

void Radar::applyPending() {
  Pending p;
  portENTER_CRITICAL(&mux);
  p = pending;
  pending = Pending();
  portEXIT_CRITICAL(&mux);

  if (!ready) {
    if (p.command == 3) {
      // "Read again" also looks for a radar that was not found (or not answering) before.
      DBG_PRINTLN("[Radar] looking for the radar again");
      if (presence == Presence::ABSENT) presence = Presence::DETECTING;
      detect_attempts = 0;
      scheduleRefresh(0);
    } else {
      DBG_PRINTLN("[Radar] radar not available, ignoring request");
      publishAll();
    }
    if (p.command) ZigbeeManager::instance().radar().setU8(RadarAttr::COMMAND, 0);
    return;
  }

  bool restart = false, adopt = false;

  if (p.config) {
    clampConfig(p.wanted);
    if (!radar.writeConfig(p.wanted, config)) DBG_PRINTLN("[Radar] not all settings were accepted");
    Ld2410::Config c;
    if (radar.readConfig(c)) {
      portENTER_CRITICAL(&mux);
      config = c;
      portEXIT_CRITICAL(&mux);
    }
    saveConfig();
    publishConfig();
  }
  if (p.resolution >= 0 && p.resolution <= 1 && p.resolution != resolution && radar.setResolution(p.resolution)) {
    resolution = p.resolution;
    saveConfig();
    restart = true;
  }
  if (p.bluetooth >= 0) setBluetooth(p.bluetooth);
  if (p.live >= 0) setLive(p.live);

  switch (p.command) {
    case 1:  // restart
      restart = true;
      break;
    case 2:  // factory reset: the defaults become the new NVS copy
      if (radar.factoryReset()) {
        restart = true;
        adopt = true;
        reapply_bluetooth = true;
      }
      break;
    case 3:  // refresh; a silent radar answers a re-read fine, it needs the restart
      if (silent) {
        restart = true;
      } else {
        scheduleRefresh(0);
      }
      break;
  }
  if (p.command) ZigbeeManager::instance().radar().setU8(RadarAttr::COMMAND, 0);

  if (restart) restartRadar(adopt);
}

void Radar::restartRadar(bool adopt) {
  DBG_PRINTLN("[Radar] restarting radar");
  radar.restart();
  scheduleRefresh(RESTART_SETTLE, adopt);
}

void Radar::setLive(bool on) {
  if (on) live_until = millis() + LIVE_TIMEOUT;
  if (on != live && radar.setEngineering(on)) {
    live = on;
    DBG_PRINTF("[Radar] live mode %s\n", live ? "on" : "off");
  }
  ZigbeeManager::instance().radar().setBool(RadarAttr::LIVE, live);
}

void Radar::setBluetooth(bool on) {
  if (on) bluetooth_until = millis() + BLUETOOTH_TIMEOUT;
  if (on == bluetooth) {
    ZigbeeManager::instance().radar().setBool(RadarAttr::BLUETOOTH, bluetooth);
    return;
  }
  if (!radar.setBluetooth(on)) {
    ZigbeeManager::instance().radar().setBool(RadarAttr::BLUETOOTH, bluetooth);
    return;
  }
  bluetooth = on;
  DBG_PRINTF("[Radar] bluetooth %s\n", bluetooth ? "on" : "off");

  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putBool("bt", on);
  p.putUChar("bt_applied", on);
  p.end();

  // Bluetooth only changes after a restart. When it goes off, keep what was set in the app.
  restartRadar(!on);
}

// Live data layout (RadarAttr::LIVE_DATA), little endian, MUST match smartswitch.mjs:
//   [0] target state  [1..2] moving cm  [3] moving energy  [4..5] static cm  [6] static energy
//   [7..8] detection cm  [9..17] moving gate energies  [18..26] static gate energies
//   [27] light  [28] OUT pin
void Radar::reportLive() {
  const Ld2410::Target& t = radar.target();
  if (!t.engineering || millis() - last_live_report < LIVE_INTERVAL) return;
  last_live_report = millis();

  uint8_t d[RADAR_LIVE_DATA_SIZE];
  d[0] = t.state;
  d[1] = t.movingDistance & 0xFF;
  d[2] = t.movingDistance >> 8;
  d[3] = t.movingEnergy;
  d[4] = t.staticDistance & 0xFF;
  d[5] = t.staticDistance >> 8;
  d[6] = t.staticEnergy;
  d[7] = t.detectionDistance & 0xFF;
  d[8] = t.detectionDistance >> 8;
  memcpy(&d[9], t.movingGates, Ld2410::GATES);
  memcpy(&d[18], t.staticGates, Ld2410::GATES);
  d[27] = t.light;
  d[28] = t.out;
  ZigbeeManager::instance().radar().setOctets(RadarAttr::LIVE_DATA, d, sizeof(d));
}

void Radar::handleZigbeeWrite(uint16_t attr, const uint8_t* data, uint8_t len) {
  if (len == 0) return;
  portENTER_CRITICAL(&mux);
  // Config fields start from the current radar config, so a partial write keeps the rest.
  bool config_field = attr <= RadarAttr::NO_ONE_DURATION;
  if (config_field && !pending.config) {
    pending.wanted = config;
    pending.config = true;
  }
  switch (attr) {
    case RadarAttr::MOVING_SENSITIVITY:
      memcpy(pending.wanted.moving, data, min<uint8_t>(len, Ld2410::GATES));
      break;
    case RadarAttr::STATIC_SENSITIVITY:
      memcpy(pending.wanted.stat, data, min<uint8_t>(len, Ld2410::GATES));
      break;
    case RadarAttr::MAX_MOVING_GATE: pending.wanted.maxMovingGate = data[0]; break;
    case RadarAttr::MAX_STATIC_GATE: pending.wanted.maxStaticGate = data[0]; break;
    case RadarAttr::NO_ONE_DURATION:
      if (len >= 2) pending.wanted.noOneDuration = data[0] | (data[1] << 8);
      break;
    case RadarAttr::RESOLUTION: pending.resolution = data[0]; break;
    case RadarAttr::BLUETOOTH: pending.bluetooth = data[0] != 0; break;
    case RadarAttr::LIVE: pending.live = data[0] != 0; break;
    case RadarAttr::COMMAND: pending.command = data[0]; break;
  }
  pending.changed_at = millis() | 1;
  portEXIT_CRITICAL(&mux);
}

void Radar::saveConfig() {
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putBytes("cfg", &config, sizeof(config));
  p.putUChar("res", resolution);
  p.end();
}

void Radar::publishConfig() {
  ZigbeeRadar& zb = ZigbeeManager::instance().radar();
  zb.setOctets(RadarAttr::MOVING_SENSITIVITY, config.moving, Ld2410::GATES);
  zb.setOctets(RadarAttr::STATIC_SENSITIVITY, config.stat, Ld2410::GATES);
  zb.setU8(RadarAttr::MAX_MOVING_GATE, config.maxMovingGate);
  zb.setU8(RadarAttr::MAX_STATIC_GATE, config.maxStaticGate);
  zb.setU16(RadarAttr::NO_ONE_DURATION, config.noOneDuration);
  zb.setU8(RadarAttr::RESOLUTION, resolution);
}

// The firmware string tells the front-end the radar's state: "none" when no radar is fitted,
// "" when a fitted radar is not answering or sends no measurements. Refused requests are
// reported back as off.
void Radar::publishAll() {
  if (presence == Presence::DETECTING) return;  // detection reports when it is done
  ZigbeeRadar& zb = ZigbeeManager::instance().radar();
  if (!ready || silent) {
    zb.setString(RadarAttr::FIRMWARE, presence == Presence::ABSENT ? "none" : "");
    zb.setBool(RadarAttr::LIVE, false);
    zb.setBool(RadarAttr::BLUETOOTH, bluetooth);
    return;
  }
  publishConfig();
  zb.setString(RadarAttr::FIRMWARE, firmware);
  zb.setBool(RadarAttr::BLUETOOTH, bluetooth);
  zb.setBool(RadarAttr::LIVE, live);
}

void Radar::printStatus() {
  const Ld2410::Target& t = radar.target();
  static const char* PRESENCE_NAMES[] = {"detecting", "absent", "present"};
  DBG_PRINTF("[Radar] %s ready=%d silent=%d connected=%d bytes=%lu frames=%lu state=%u moving=%ucm/%u static=%ucm/%u live=%d bt=%d fw=%s\n",
             PRESENCE_NAMES[(uint8_t)presence], ready, silent, radar.connected(), (unsigned long)radar.bytesReceived(), (unsigned long)radar.framesReceived(),
             t.state, t.movingDistance, t.movingEnergy, t.staticDistance, t.staticEnergy, live, bluetooth, firmware);
  printConfig("radar", config, resolution);
}

void Radar::printConfig(const char* label, const Ld2410::Config& c, uint8_t res) {
  DBG_PRINTF("[Radar] %s: gates %u/%u no-one %us res %u moving", label, c.maxMovingGate, c.maxStaticGate, c.noOneDuration, res);
  for (uint8_t i = 0; i < Ld2410::GATES; i++) DBG_PRINTF(" %u", c.moving[i]);
  DBG_PRINT(" static");
  for (uint8_t i = 0; i < Ld2410::GATES; i++) DBG_PRINTF(" %u", c.stat[i]);
  DBG_PRINTLN();
}

void Radar::printStorage() {
  Preferences p;
  p.begin(NVS_NAMESPACE, true);
  Ld2410::Config stored;
  bool has = p.getBytes("cfg", &stored, sizeof(stored)) == sizeof(stored);
  uint8_t res = p.getUChar("res", 0xFF);
  DBG_PRINTF("[Radar] nvs: bt=%d bt_applied=%u\n", p.getBool("bt", false), p.getUChar("bt_applied", 0xFF));
  p.end();
  if (has) printConfig("nvs", stored, res);
  else DBG_PRINTLN("[Radar] nvs: no stored config");
}
