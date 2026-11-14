#pragma once

#include "Arduino.h"
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

#define BME_I2C_ADDRESS 0x77
#define BME_I2C_SCL_PIN   19
#define BME_I2C_SDA_PIN   20

#define VALUE_UPDATE_INTERVAL 60000 // 60s (push temperature/humidity every minute)

class Sensor {
  typedef std::function<void()> CallbackFunction;  
public:
  Sensor();
  void initialize(CallbackFunction value_update_handler);
  void loop();
  float getTemperature();
  float getHumidity();
  float getPressure();
private:
  CallbackFunction value_update_cb = NULL;

  TwoWire i2c;
  Adafruit_BME280 bme;
  unsigned long timer;
};
