#include "Debug.h"
#include "I2sOutput.h"

I2sOutput::I2sOutput(i2s_port_t i2s_port) : m_i2s_port(i2s_port), volume(0.5f), sample_rate(22050)
{
  frames_buffer = (int16_t *)malloc(2 * sizeof(int16_t) * NUM_FRAMES_TO_SEND);
}

void I2sOutput::initialize()
{
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(m_i2s_port, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer

  i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle);
  
  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = GPIO_NUM_0,
      .bclk = GPIO_NUM_6,
      .ws = GPIO_NUM_7,
      .dout = GPIO_NUM_2,
      .din = GPIO_NUM_3,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false,
      },
    },
  };
  // std_cfg.clk_cfg.mclk_multiple = (i2s_mclk_multiple_t)384; // If not using 24-bit data width, 256 should be enough

  i2s_channel_init_std_mode(tx_handle, &std_cfg);
  i2s_channel_init_std_mode(rx_handle, &std_cfg);

  i2s_channel_enable(tx_handle);
  i2s_channel_enable(rx_handle); 
}

void I2sOutput::start(uint32_t sample_rate)
{
  if (this->sample_rate != sample_rate)
  {
    i2s_channel_disable(tx_handle);
    i2s_channel_disable(rx_handle);

    this->sample_rate = sample_rate;
    i2s_std_clk_config_t i2s_std_clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    i2s_channel_reconfig_std_clock(tx_handle, &i2s_std_clk_cfg);
    i2s_channel_reconfig_std_clock(rx_handle, &i2s_std_clk_cfg);

    i2s_channel_enable(tx_handle);
    i2s_channel_enable(rx_handle); 
  }
}

void I2sOutput::write(int16_t *samples, int frames)
{
  esp_err_t ret = ESP_OK;
  // this will contain the prepared samples for sending to the I2S device
  int frame_index = 0;
  while (frame_index < frames)
  {
    // fill up the frames buffer with the next NUM_FRAMES_TO_SEND frames
    int frames_to_send = 0;
    for (int i = 0; i < NUM_FRAMES_TO_SEND && frame_index < frames; i++)
    {
      int left_sample = volume * float(samples[frame_index * 2]);
      int right_sample = volume * float(samples[frame_index * 2 + 1]);
      frames_buffer[i * 2] = left_sample;
      frames_buffer[i * 2 + 1] = right_sample;
      frames_to_send++;
      frame_index++;
    }
    // write data to the i2s peripheral - this will block until the data is sent
    size_t bytes_written = 0;
    ret = i2s_channel_write(tx_handle, frames_buffer, frames_to_send * sizeof(int16_t) * 2, &bytes_written, portMAX_DELAY);
    if (ret != ESP_OK)
    {
      /* Since we set timeout to 'portMAX_DELAY' in 'i2s_channel_write'
          so you won't reach here unless you set other timeout value,
          if timeout detected, it means write operation failed. */
      DBG_PRINTLN("i2s write failed");
    }
  }
}

void I2sOutput::set_volume(uint8_t volume)
{
  this->volume = volume / 100.0f;
}