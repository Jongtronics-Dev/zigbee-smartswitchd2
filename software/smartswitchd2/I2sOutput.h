#pragma once

#include <freertos/FreeRTOS.h>
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include <Arduino.h>

#define GPIO_OUTPUT_IO_1 (gpio_num_t)1
#define GPIO_OUTPUT_PIN_SEL (1ULL<<GPIO_OUTPUT_IO_1)

// number of frames to try and send at once (a frame is a left and right sample)
const int NUM_FRAMES_TO_SEND = 256;

class I2sOutput
{
private:
  i2s_port_t m_i2s_port;
  int16_t *frames_buffer;

  float volume;
  uint32_t sample_rate;

  i2s_chan_handle_t tx_handle = NULL;
  i2s_chan_handle_t rx_handle = NULL;

public:
  I2sOutput(i2s_port_t i2s_port);
  void initialize();
  void start(uint32_t sample_rate);

  // NOTE - a frame consists of both a left and a right sample
  void write(int16_t *samples, int frames);
  
  // volume is a value between 0 and 100
  void set_volume(u_int8_t volume);
};
