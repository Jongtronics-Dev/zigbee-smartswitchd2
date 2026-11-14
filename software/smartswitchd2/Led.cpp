#include "Debug.h"
#include "Led.h"

Led::Led()
{
  timer = 0;
  state = false;
}

void Led::initialize()
{
  pinMode(LED_PIN, OUTPUT);
  setBlinkInterval(INTERVAL_DEFAULT);
}

void Led::loop()
{
  if (millis() - timer > blink_interval)
  {
    timer = millis();
    state = !state;
    digitalWrite(LED_PIN, state);
  }
}

void Led::setDebugMode(bool debug_mode)
{
  setBlinkInterval(INTERVAL_FAST);
}

void Led::setBlinkInterval(BlinkInterval interval)
{
  switch(interval) {
    case INTERVAL_FAST:
      blink_interval = INTERVAL_FAST;
      DBG_PRINTF("Setting LED blink interval to FAST\n");
      break;
    case INTERVAL_DEFAULT:
    default:
      blink_interval = INTERVAL_DEFAULT;
      DBG_PRINTF("Setting LED blink interval to DEFAULT\n");
      break;
  }
}
