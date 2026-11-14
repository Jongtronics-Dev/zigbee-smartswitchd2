#include "Mmwave.h"

Mmwave::Mmwave() : last_update_time(0)
{}

void Mmwave::initialize(CallbackFunctionXY value_change_handler)
{
  value_change_cb = value_change_handler;
  // xTaskCreate(echo_task, "uart_echo_task", ECHO_TASK_STACK_SIZE, NULL, 7, NULL);
  auto task = [](void* arg) { static_cast<Mmwave*>(arg)->echo_task(); };
  xTaskCreate(task, "uart_echo_task", ECHO_TASK_STACK_SIZE, this, 5, NULL);
}

uint16_t Mmwave::twoByteToUint(char firstByte, char secondByte)
{
  return (uint16_t)(secondByte << 8) + firstByte;
}

void Mmwave::reportTargetInfo(int target, char *raw)
{
  int16_t newX, newY, newSpeed;
  // uint16_t newResolution;

  newX = twoByteToUint(raw[0], raw[1] & 0x7F);
  newX = (raw[1] >> 7 != 0x1) ? (0 - newX / 10) : newX / 10;

  newY = twoByteToUint(raw[2], raw[3] & 0x7F);
  newY = (raw[3] >> 7 != 0x1) ? (0 - newY / 10) : (newY / 10);
  
  newSpeed = twoByteToUint(raw[4], raw[5] & 0x7F);
  if (raw[5] >> 7 != 0x1)
    newSpeed = 0 - newSpeed;
  
  // newResolution = twoByteToUint(raw[6], raw[7]);

  // printf("x = %d \t\t y = %d \t\t s = %d \t\t r = %d \n", newX, newY, newSpeed);

  int64_t now = esp_timer_get_time();
  if (target == 0 && (x != newX || y != newY) && last_update_time + UPDATE_INTERVAL < now)
  {
    x = newX;
    y = newY;

    last_update_time = now;
    
    value_change_cb(x, y);
  }
}

void Mmwave::handlePeriodicData(char *buffer)
{
  char stateBytes[STATE_SIZE];

  for (int i = 0; i < TARGETS; i++)
  {
    memcpy(stateBytes, &buffer[4 + i * STATE_SIZE], STATE_SIZE);
    reportTargetInfo(i, stateBytes);
  }
}

void Mmwave::echo_task()
{
  uart_config_t uart_config = {
    .baud_rate  = ECHO_UART_BAUD_RATE,
    .data_bits  = UART_DATA_8_BITS,
    .parity     = UART_PARITY_DISABLE,
    .stop_bits  = UART_STOP_BITS_1,
    .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
  };
  int intr_alloc_flags = 0;

  ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
  ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, ECHO_TEST_TXD, ECHO_TEST_RXD, ECHO_TEST_RTS, ECHO_TEST_CTS));

  // Configure a temporary buffer for the incoming data
  static char buffer[BUF_SIZE];

  while (1)
  {
    int len = uart_read_bytes(ECHO_UART_PORT_NUM, buffer, BUF_SIZE, 40 / portTICK_PERIOD_MS);

    if (len)
    {
      handlePeriodicData(buffer);
    }
  }
}
