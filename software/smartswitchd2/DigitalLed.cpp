#include "DigitalLed.h"

DigitalLed::DigitalLed(uint8_t _pin) : pin(_pin)
{
}

void DigitalLed::initialize()
{
  neopixel = new Adafruit_NeoPixel(1, pin, NEO_RGB + NEO_KHZ400); // NEO_GRB + NEO_KHZ800);
  neopixel->begin();
  neopixel->clear();
  neopixel->show();
}

void DigitalLed::loop()
{
  unsigned long now = millis();
  if (now - last_frame < FRAME_MS) return;
  last_frame = now;

  uint8_t r, g, b, l;
  bool blink;
  portENTER_CRITICAL(&mux);
  if (off_at != 0 && (long)(now - off_at) >= 0)
  {
    off_at = 0;
    level = 0;
  }
  r = red;
  g = green;
  b = blue;
  l = level;
  blink = blinking;
  portEXIT_CRITICAL(&mux);

  if (!blink)
  {
    blink_on = true;
  }
  else if (now - blink_timer >= BLINKING_INTERVAL)
  {
    blink_timer = now;
    blink_on = !blink_on;
  }

  // A new target restarts the fade from wherever the LED is now.
  float ratio = blink_on ? (float)l / 255 : 0;
  uint8_t next[3] = {(uint8_t)(r * ratio + 0.5f), (uint8_t)(g * ratio + 0.5f), (uint8_t)(b * ratio + 0.5f)};
  if (memcmp(next, target, sizeof(target)) != 0)
  {
    memcpy(target, next, sizeof(target));
    memcpy(from, shown, sizeof(from));
    fade_start = now;
  }

  float t = min(1.0f, (float)(now - fade_start) / FADE_MS);
  float eased = t * t * (3 - 2 * t);  // smoothstep: eases in and out
  uint8_t out[3];
  for (int i = 0; i < 3; i++)
  {
    shown[i] = from[i] + (target[i] - from[i]) * eased;
    out[i] = (uint8_t)(shown[i] + 0.5f);
  }

  if (memcmp(out, written, sizeof(written)) != 0)
  {
    memcpy(written, out, sizeof(written));
    neopixel->setPixelColor(0, neopixel->Color(out[0], out[1], out[2]));
    neopixel->show();
  }
}

void DigitalLed::setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t _level)
{
  portENTER_CRITICAL(&mux);
  red = r;
  green = g;
  blue = b;
  level = _level;
  off_at = 0;
  portEXIT_CRITICAL(&mux);
}

void DigitalLed::setRGB(uint8_t r, uint8_t g, uint8_t b)
{
  setColor(r, g, b, 255);
}

void DigitalLed::setLevel(uint8_t _level)
{
  portENTER_CRITICAL(&mux);
  level = _level;
  portEXIT_CRITICAL(&mux);
}

void DigitalLed::setBlinking(bool _blinking)
{
  portENTER_CRITICAL(&mux);
  blinking = _blinking;
  portEXIT_CRITICAL(&mux);
}

void DigitalLed::setOffTimeout(uint16_t timeout_ms)
{
  portENTER_CRITICAL(&mux);
  // Was a uint16_t holding millis() + timeout, which wrapped after ~65 s of uptime.
  off_at = (millis() + timeout_ms) | 1;
  portEXIT_CRITICAL(&mux);
}
