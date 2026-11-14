#pragma once

// Local OTA settings for the main firmware.
//
// Copy this file to "OtaConfig.h" (same folder) and set your ha_server URL.
// OtaConfig.h is git-ignored, so your address never reaches the repo.
//
// OTA_DEFAULT_SERVER_URL is used when an update trigger carries no URL (the Zigbee
// OTA trigger only carries the artifact selection). Leave it empty if you flash over
// USB only; the firmware builds and runs without OtaConfig.h.

// Base URL of ha_server on the LAN, no trailing slash. e.g. "http://192.168.1.10:8080"
#define OTA_DEFAULT_SERVER_URL ""
