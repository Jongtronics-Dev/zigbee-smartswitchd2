#pragma once

// Template for the updater's local WiFi/server fallback config.
//
// Copy this file to "OtaConfig.h" (same folder) and fill in the values for bench
// testing. OtaConfig.h is git-ignored so real credentials never reach the repo.
//
// In production these are NOT needed here: the WiFi credentials are stored in NVS
// (provisioned once by the main app / front-end), and the updater reads NVS first.
// These #defines are only used as a fallback when NVS has no value.

#define OTA_FALLBACK_WIFI_SSID   ""   // e.g. "MyHomeWifi"  (leave empty to use NVS only)
#define OTA_FALLBACK_WIFI_PASS   ""   // e.g. "supersecret"

// Base URL of ha_server on the LAN, no trailing slash. e.g. "http://192.168.1.10:8080"
#define OTA_FALLBACK_SERVER_URL  ""
