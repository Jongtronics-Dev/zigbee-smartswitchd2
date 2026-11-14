// SmartSwitchD2 — WiFi OTA updater image.
//
// This is a SEPARATE, small firmware that lives in the second OTA slot (ota_1). The
// main (Zigbee) app has no WiFi; when an update is requested it stores the intent in
// NVS, switches the boot partition to this updater, and reboots. This image then:
//   1. brings up WiFi (creds from NVS, or OtaConfig.h fallback for bench testing),
//   2. calls home to ha_server,
//   3. pulls the sound image and/or the new main firmware over plain HTTP,
//   4. flashes spiffs (in place) and/or the main app (into ota_0, setting boot there),
//   5. clears the intent and reboots into the main app.
// Any failure/timeout falls back to booting the main app, so the device never sticks
// in the updater. See README.md, section 4.
//
// Build: ESP32C6 Dev Module, Partition Scheme "Zigbee ZCZR 4MB with spiffs",
//        Zigbee Mode: Disabled, USB CDC On Boot: Enabled.

#include <WiFi.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <SPIFFS.h>
#if __has_include("OtaConfig.h")
#include "OtaConfig.h"   // local, git-ignored; see OtaConfig.example.h
#endif
#ifndef OTA_FALLBACK_WIFI_SSID
#define OTA_FALLBACK_WIFI_SSID   ""
#endif
#ifndef OTA_FALLBACK_WIFI_PASS
#define OTA_FALLBACK_WIFI_PASS   ""
#endif
#ifndef OTA_FALLBACK_SERVER_URL
#define OTA_FALLBACK_SERVER_URL  ""
#endif

// The updater is a dev/bootstrap image; always log over USB CDC.
#define ULOG(...)   Serial.printf(__VA_ARGS__)

// NVS namespaces/keys — MUST match OtaManager.cpp in the main app.
static const char* NS_OTA  = "ota";
static const char* K_ART   = "artifacts";
static const char* K_URL   = "url";
static const char* K_TOKEN = "token";
static const char* NS_WIFI = "otawifi";
static const char* K_SSID  = "ssid";
static const char* K_PASS  = "pass";

static const uint8_t ARTIFACT_APP    = 0x01;
static const uint8_t ARTIFACT_SPIFFS = 0x02;

static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 120000UL;
static const unsigned long DOWNLOAD_STALL_MS       = 15000UL;

// Minimal http:// URL split. Declared before any function so the Arduino auto-generated
// prototypes (which reference it) come after its definition.
struct Url { String host; uint16_t port; String path; };

static String deviceId() {
  // Full 8-byte EUI-64, matching the Zigbee IEEE address (see OtaManager.cpp).
  uint64_t mac = ESP.getEfuseMac();
  uint8_t b[8];
  for (int i = 0; i < 8; i++) b[i] = (uint8_t)((mac >> (8 * i)) & 0xFF);
  char buf[17];
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
  return String(buf);
}

// Boot back into the main app (ota_0) and restart. From the updater (ota_1) the
// "next" update partition is ota_0. (No [[noreturn]] attribute: Arduino's auto-
// generated prototype would not carry it, causing a declaration mismatch.)
static void bootMainApp(const char* reason) {
  ULOG("[UPD] boot main app: %s\n", reason);
  const esp_partition_t* main = esp_ota_get_next_update_partition(NULL);
  if (main != nullptr) {
    esp_ota_set_boot_partition(main);
  }
  delay(200);
  ESP.restart();
  while (true) delay(1000);
}

static bool connectWifi() {
  String ssid, pass;
  Preferences p;
  p.begin(NS_WIFI, true);
  ssid = p.getString(K_SSID, "");
  pass = p.getString(K_PASS, "");
  p.end();
  if (ssid.length() == 0) {
    ssid = OTA_FALLBACK_WIFI_SSID;
    pass = OTA_FALLBACK_WIFI_PASS;
  }
  if (ssid.length() == 0) {
    ULOG("[UPD] no WiFi credentials\n");
    return false;
  }
  ULOG("[UPD] connecting to WiFi '%s'\n", ssid.c_str());
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      ULOG("[UPD] WiFi timeout\n");
      return false;
    }
    delay(250);
  }
  ULOG("[UPD] WiFi ok, ip=%s\n", WiFi.localIP().toString().c_str());
  return true;
}

static bool parseHttpUrl(const String& url, Url& out) {
  const char* prefix = "http://";
  if (!url.startsWith(prefix)) return false;
  int start = strlen(prefix);
  int slash = url.indexOf('/', start);
  String hostport = (slash < 0) ? url.substring(start) : url.substring(start, slash);
  out.path = (slash < 0) ? "/" : url.substring(slash);
  int colon = hostport.indexOf(':');
  if (colon < 0) { out.host = hostport; out.port = 80; }
  else { out.host = hostport.substring(0, colon); out.port = (uint16_t)hostport.substring(colon + 1).toInt(); }
  return out.host.length() > 0;
}

// Read status line + headers; return status code, set contentLen. Leaves client at body.
static int readHttpHeader(WiFiClient& client, long& contentLen) {
  contentLen = -1;
  String status = client.readStringUntil('\n');
  int sp = status.indexOf(' ');
  int code = (sp > 0) ? status.substring(sp + 1, sp + 4).toInt() : 0;
  while (true) {
    String line = client.readStringUntil('\n');
    if (line.length() == 0 || line == "\r") break;
    String lower = line; lower.toLowerCase();
    if (lower.startsWith("content-length:")) contentLen = line.substring(line.indexOf(':') + 1).toInt();
  }
  return code;
}

static bool callHome(const String& baseUrl, const String& token) {
  Url u;
  if (!parseHttpUrl(baseUrl + "/api/ota/ready", u)) return false;

  // Report real SPIFFS content usage so the front-end can show how full the sounds
  // partition is. Mount read-only; unmount before we later raw-write it (U_SPIFFS).
  size_t spiffsUsed = 0, spiffsTotal = 0;
  if (SPIFFS.begin(false)) {
    spiffsUsed = SPIFFS.usedBytes();
    spiffsTotal = SPIFFS.totalBytes();
    SPIFFS.end();
  }

  WiFiClient client;
  client.setTimeout(5000);
  if (!client.connect(u.host.c_str(), u.port)) return false;
  String body = String("{\"id\":\"") + deviceId() +
                "\",\"ip\":\"" + WiFi.localIP().toString() +
                "\",\"spiffs_used\":" + String((unsigned)spiffsUsed) +
                ",\"spiffs_total\":" + String((unsigned)spiffsTotal) + "}";
  client.printf("POST %s HTTP/1.1\r\n", u.path.c_str());
  client.printf("Host: %s\r\n", u.host.c_str());
  client.print("Content-Type: application/json\r\n");
  if (token.length()) client.printf("X-OTA-Token: %s\r\n", token.c_str());
  client.printf("Content-Length: %u\r\n", (unsigned)body.length());
  client.print("Connection: close\r\n\r\n");
  client.print(body);
  long ignored;
  int code = readHttpHeader(client, ignored);
  client.stop();
  ULOG("[UPD] call-home -> %d\n", code);
  return code >= 200 && code < 400;
}

static bool flashArtifact(const String& url, int updateCommand, const String& token) {
  ULOG("[UPD] downloading %s\n", url.c_str());
  Url u;
  if (!parseHttpUrl(url, u)) { ULOG("[UPD] bad url\n"); return false; }

  WiFiClient client;
  client.setTimeout(10000);
  if (!client.connect(u.host.c_str(), u.port)) { ULOG("[UPD] connect failed\n"); return false; }

  client.printf("GET %s HTTP/1.1\r\n", u.path.c_str());
  client.printf("Host: %s\r\n", u.host.c_str());
  if (token.length()) client.printf("X-OTA-Token: %s\r\n", token.c_str());
  client.print("Connection: close\r\n\r\n");

  long len = -1;
  int code = readHttpHeader(client, len);
  if (code != 200) { ULOG("[UPD] GET status %d\n", code); client.stop(); return false; }
  if (len <= 0) { ULOG("[UPD] missing Content-Length\n"); client.stop(); return false; }
  ULOG("[UPD] size=%ld cmd=%d\n", len, updateCommand);

  if (!Update.begin((size_t)len, updateCommand)) {
    ULOG("[UPD] Update.begin failed: %s\n", Update.errorString());
    client.stop();
    return false;
  }

  uint8_t buf[1024];
  long remaining = len;
  unsigned long lastData = millis();
  while (remaining > 0) {
    int avail = client.available();
    if (avail > 0) {
      int want = (int)((remaining < (long)sizeof(buf)) ? remaining : (long)sizeof(buf));
      int n = client.read(buf, want);
      if (n > 0) {
        if (Update.write(buf, n) != (size_t)n) {
          ULOG("[UPD] Update.write failed: %s\n", Update.errorString());
          Update.abort(); client.stop(); return false;
        }
        remaining -= n;
        lastData = millis();
      }
    } else {
      if (!client.connected()) break;
      if (millis() - lastData > DOWNLOAD_STALL_MS) {
        ULOG("[UPD] download stalled\n");
        Update.abort(); client.stop(); return false;
      }
      delay(1);
    }
  }
  client.stop();

  if (remaining != 0) { ULOG("[UPD] incomplete (%ld left)\n", remaining); Update.abort(); return false; }
  // end(true): finalize; for U_FLASH this also sets ota_0 as the new boot partition.
  if (!Update.end(true)) { ULOG("[UPD] Update.end failed: %s\n", Update.errorString()); return false; }
  ULOG("[UPD] artifact flashed OK\n");
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(200);
  ULOG("\n\n[UPD] SmartSwitchD2 updater\n");

  // If this updater image was itself just flashed, confirm it so it isn't rolled back.
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (running && esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
  }

  Preferences p;
  p.begin(NS_OTA, true);
  uint8_t artifacts = p.getUChar(K_ART, 0);
  String  baseUrl   = p.getString(K_URL, "");
  String  token     = p.getString(K_TOKEN, "");
  p.end();

  if (baseUrl.length() == 0) baseUrl = OTA_FALLBACK_SERVER_URL;
  while (baseUrl.endsWith("/")) baseUrl.remove(baseUrl.length() - 1);

  if (artifacts == 0)      bootMainApp("no artifacts requested");
  if (baseUrl.length() == 0) bootMainApp("no server url");
  if (!connectWifi())      bootMainApp("wifi failed");

  callHome(baseUrl, token); // best-effort

  const String otaBase = baseUrl + "/api/ota/" + deviceId();
  bool appRequested = artifacts & ARTIFACT_APP;

  // Sounds first (in place), then the app (which sets the boot slot last).
  if (artifacts & ARTIFACT_SPIFFS) {
    if (!flashArtifact(otaBase + "/spiffs.bin", U_SPIFFS, token)) bootMainApp("spiffs flash failed");
  }
  if (appRequested) {
    if (!flashArtifact(otaBase + "/app.bin", U_FLASH, token)) bootMainApp("app flash failed");
  }

  // Clear the intent so a stray reboot into the updater just returns to the main app.
  Preferences pc;
  pc.begin(NS_OTA, false);
  pc.remove(K_ART);
  pc.remove(K_URL);
  pc.remove(K_TOKEN);
  pc.end();

  if (!appRequested) {
    // Only sounds were flashed — no app write set the boot slot, so do it explicitly.
    bootMainApp("sounds updated");
  }

  ULOG("[UPD] done, rebooting into new main app\n");
  delay(300);
  ESP.restart();
}

void loop() {}
