#pragma once

#include "Arduino.h"
#include <Adafruit_NeoPixel.h>

#define BLINKING_INTERVAL 800 // ms

// The WS2811 status LED.
//
// Every change fades in over FADE_MS. A Zigbee colour change does not arrive as one value: the
// library reports X and Y separately (and the level on its own), often several times during a
// transition, and showing each step made the LED flash through wrong in-between colours -
// worst towards white, whose X/Y is far from any saturated colour. With the fade those
// in-betweens last milliseconds and are never seen.
//
// The setters only record what is wanted and may be called from the Zigbee task; the LED is
// written from loop() alone.
class DigitalLed
{
public:
  DigitalLed(uint8_t _pin);
  void initialize();
  void loop();

  // Colour and brightness in one step, so no intermediate state is ever shown.
  void setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t level);
  // Colour at full brightness.
  void setRGB(uint8_t r, uint8_t g, uint8_t b);
  void setLevel(uint8_t level);
  void setBlinking(bool blinking);
  // Fade out after timeout_ms, unless a new colour is set first.
  void setOffTimeout(uint16_t timeout_ms);

private:
  static constexpr unsigned long FADE_MS = 200;
  static constexpr unsigned long FRAME_MS = 10;

  uint8_t pin;
  Adafruit_NeoPixel *neopixel = nullptr;

  // What is wanted (setters, any task).
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  uint8_t red = 0, green = 0, blue = 0, level = 0;
  bool blinking = false;
  unsigned long off_at = 0; // 0 = no off timeout pending

  // What is shown (loop() only).
  bool blink_on = true;
  unsigned long blink_timer = 0;
  unsigned long last_frame = 0;
  unsigned long fade_start = 0;
  float from[3] = {0, 0, 0};
  float shown[3] = {0, 0, 0};
  uint8_t target[3] = {0, 0, 0};
  uint8_t written[3] = {0, 0, 0};
};
