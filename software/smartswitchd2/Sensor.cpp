#include "Debug.h"
#include "Sensor.h"

Sensor::Sensor() : i2c(0), timer(0)
{
  bme = Adafruit_BME280();
}

void Sensor::initialize(CallbackFunction value_changed_handler)
{
  value_update_cb = value_changed_handler;
  i2c.begin(BME_I2C_SDA_PIN, BME_I2C_SCL_PIN, 1000000);
  
  if(bme.begin(BME_I2C_ADDRESS, &i2c))
  {
    DBG_PRINTF("Found BME280 on I2C address 0x%X\n", BME_I2C_ADDRESS);
  }
  else
  {
    DBG_PRINTF("*ERROR* No BME280 found\n");
  }
}

void Sensor::loop()
{
  if (millis() > timer + VALUE_UPDATE_INTERVAL)
  {
    timer = millis();
    if (value_update_cb) value_update_cb();
  }
}

float Sensor::getTemperature()
{
  return bme.readTemperature();
}

float Sensor::getHumidity()
{
  return bme.readHumidity();
}

float Sensor::getPressure()
{
  return bme.readPressure() / 100.0f;
}
