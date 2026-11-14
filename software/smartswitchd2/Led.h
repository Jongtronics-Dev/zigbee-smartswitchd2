#pragma once

#include "Arduino.h"

#define LED_PIN  14

enum BlinkInterval {
  INTERVAL_FAST = 50,     // 50ms
  INTERVAL_DEFAULT = 500  //500ms
};

class Led {
private:
  unsigned long blink_interval;
  unsigned long timer;
  bool state;
  void setBlinkInterval(BlinkInterval interval);

public:
  Led();
  void initialize();
  void loop();
  void setDebugMode(bool debug_mode);
};
