#pragma once

#include <Arduino.h>
#include <string>
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_spiffs.h"

class SPIFFS
{
private:
  std::string m_mount_point;
public:
  SPIFFS(const char *mount_point);
  ~SPIFFS();
};