#pragma once

#include <Arduino.h>

// Serial debug switch.
//   1 = development: Serial is initialized and debug prints are emitted.
//   0 = production:  Serial.begin() and all DBG_* prints compile to nothing,
//                    so there is zero serial output and, importantly, no USB-CDC
//                    TX-timeout blocking that makes the device sluggish when no
//                    serial monitor is attached.
#define DEBUG_SERIAL 0

#if DEBUG_SERIAL
  #define DBG_BEGIN()       do { Serial.begin(115200); Serial.setTxTimeoutMs(0); } while (0)
  #define DBG_PRINT(...)    Serial.print(__VA_ARGS__)
  #define DBG_PRINTLN(...)  Serial.println(__VA_ARGS__)
  #define DBG_PRINTF(...)   Serial.printf(__VA_ARGS__)
#else
  #define DBG_BEGIN()       do {} while (0)
  #define DBG_PRINT(...)    do {} while (0)
  #define DBG_PRINTLN(...)  do {} while (0)
  #define DBG_PRINTF(...)   do {} while (0)
#endif
