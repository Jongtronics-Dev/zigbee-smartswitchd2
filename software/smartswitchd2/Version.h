#pragma once

// Single source of truth for the firmware version string.
// Used for the serial banner AND reported to zigbee2mqtt as the ZCL swBuildId
// (Basic cluster attr 0x4000), which z2m shows as "Firmware rev.".
#define SOFTWARE_VERSION "0.3.9"
