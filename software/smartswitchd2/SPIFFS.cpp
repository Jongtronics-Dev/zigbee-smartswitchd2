#include "Debug.h"
#include "SPIFFS.h"

#define SPI_DMA_CHAN 1

SPIFFS::SPIFFS(const char *mount_point)
{
  m_mount_point = mount_point;

  esp_vfs_spiffs_conf_t mount_cofig = {
      .base_path = mount_point,
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = false};

  DBG_PRINT("Initializing SPIFFS ... ");

  esp_err_t ret = esp_vfs_spiffs_register(&mount_cofig);

  if (ret != ESP_OK)
  {
    DBG_PRINTLN("FAIL");
    return;
  }
  else
  {
    DBG_PRINTLN("OK");
  }
}

SPIFFS::~SPIFFS()
{
  esp_vfs_spiffs_unregister(NULL);
  DBG_PRINTLN("SPIFFS unmounted");
}