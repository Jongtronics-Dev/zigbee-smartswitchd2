#pragma once

#include "Arduino.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include <string.h>

#define ECHO_TEST_TXD 16 // (CONFIG_EXAMPLE_UART_TXD)
#define ECHO_TEST_RXD 17 // (CONFIG_EXAMPLE_UART_RXD)
#define ECHO_TEST_RTS (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)

#define ECHO_UART_PORT_NUM   UART_NUM_1
#define ECHO_UART_BAUD_RATE  115200
#define ECHO_TASK_STACK_SIZE 2048

#define BUF_SIZE (128)
#define STATE_SIZE 8
#define TARGETS 3

#define UPDATE_INTERVAL 1000000 // 1s

class Mmwave {
  typedef std::function<void(int16_t x, int16_t y)> CallbackFunctionXY;
private:
  uint16_t twoByteToUint(char firstByte, char secondByte);
  void reportTargetInfo(int target, char *raw);
  void handlePeriodicData(char *buffer);
  void echo_task();

  CallbackFunctionXY value_change_cb;
  int16_t x, y;
  int64_t last_update_time;

public:
  Mmwave();
  void initialize(CallbackFunctionXY value_change_handler);
};
