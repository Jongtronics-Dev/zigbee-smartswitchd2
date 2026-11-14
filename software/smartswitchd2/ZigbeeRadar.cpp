#include "Debug.h"
#include "ZigbeeRadar.h"

ZigbeeRadar::ZigbeeRadar(uint8_t endpoint) : ZigbeeEP(endpoint) {
  _device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID;

  _cluster_list = esp_zb_zcl_cluster_list_create();
  esp_zb_cluster_list_add_basic_cluster(_cluster_list, esp_zb_basic_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_identify_cluster(_cluster_list, esp_zb_identify_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

  // String attributes are allocated at their initial length (first byte), so start them
  // at full size; later values may be shorter but never longer.
  uint8_t sensitivity[1 + RADAR_GATES] = {RADAR_GATES};
  uint8_t live_data[1 + RADAR_LIVE_DATA_SIZE] = {RADAR_LIVE_DATA_SIZE};
  char firmware[1 + RADAR_FIRMWARE_MAX + 1];
  firmware[0] = RADAR_FIRMWARE_MAX;
  memset(firmware + 1, ' ', RADAR_FIRMWARE_MAX);
  firmware[RADAR_FIRMWARE_MAX + 1] = '\0';
  uint8_t max_gate = 8, resolution = 0, command = 0;
  uint16_t duration = 5;
  bool off = false;

  const uint8_t rw = ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING;
  const uint8_t ro = ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING;

  esp_zb_attribute_list_t* radar = esp_zb_zcl_attr_list_create(RADAR_CLUSTER_ID);
  esp_zb_custom_cluster_add_custom_attr(radar, RadarAttr::MOVING_SENSITIVITY, ESP_ZB_ZCL_ATTR_TYPE_OCTET_STRING, rw, sensitivity);
  esp_zb_custom_cluster_add_custom_attr(radar, RadarAttr::STATIC_SENSITIVITY, ESP_ZB_ZCL_ATTR_TYPE_OCTET_STRING, rw, sensitivity);
  esp_zb_custom_cluster_add_custom_attr(radar, RadarAttr::MAX_MOVING_GATE, ESP_ZB_ZCL_ATTR_TYPE_U8, rw, &max_gate);
  esp_zb_custom_cluster_add_custom_attr(radar, RadarAttr::MAX_STATIC_GATE, ESP_ZB_ZCL_ATTR_TYPE_U8, rw, &max_gate);
  esp_zb_custom_cluster_add_custom_attr(radar, RadarAttr::NO_ONE_DURATION, ESP_ZB_ZCL_ATTR_TYPE_U16, rw, &duration);
  esp_zb_custom_cluster_add_custom_attr(radar, RadarAttr::RESOLUTION, ESP_ZB_ZCL_ATTR_TYPE_U8, rw, &resolution);
  esp_zb_custom_cluster_add_custom_attr(radar, RadarAttr::BLUETOOTH, ESP_ZB_ZCL_ATTR_TYPE_BOOL, rw, &off);
  esp_zb_custom_cluster_add_custom_attr(radar, RadarAttr::LIVE, ESP_ZB_ZCL_ATTR_TYPE_BOOL, rw, &off);
  esp_zb_custom_cluster_add_custom_attr(radar, RadarAttr::LIVE_DATA, ESP_ZB_ZCL_ATTR_TYPE_OCTET_STRING, ro, live_data);
  esp_zb_custom_cluster_add_custom_attr(radar, RadarAttr::FIRMWARE, ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING, ro, firmware);
  esp_zb_custom_cluster_add_custom_attr(radar, RadarAttr::COMMAND, ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &command);
  esp_zb_cluster_list_add_custom_cluster(_cluster_list, radar, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

  _ep_config = {.endpoint = _endpoint, .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID, .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID, .app_device_version = 0};
}

bool ZigbeeRadar::setOctets(uint16_t attr, const uint8_t* data, uint8_t len) {
  uint8_t value[1 + 64];
  if (len > 64) return false;
  value[0] = len;
  memcpy(value + 1, data, len);
  if (setClusterAttribute(RADAR_CLUSTER_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr, value) != ESP_ZB_ZCL_STATUS_SUCCESS) return false;
  return report(attr);
}

bool ZigbeeRadar::setString(uint16_t attr, const char* str) {
  size_t len = strnlen(str, RADAR_FIRMWARE_MAX);
  char value[1 + RADAR_FIRMWARE_MAX + 1];
  value[0] = len;
  memcpy(value + 1, str, len);
  value[len + 1] = '\0';
  if (setClusterAttribute(RADAR_CLUSTER_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr, value) != ESP_ZB_ZCL_STATUS_SUCCESS) return false;
  return report(attr);
}

bool ZigbeeRadar::setU8(uint16_t attr, uint8_t value) {
  if (setClusterAttribute(RADAR_CLUSTER_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr, &value) != ESP_ZB_ZCL_STATUS_SUCCESS) return false;
  return report(attr);
}

bool ZigbeeRadar::setU16(uint16_t attr, uint16_t value) {
  if (setClusterAttribute(RADAR_CLUSTER_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr, &value) != ESP_ZB_ZCL_STATUS_SUCCESS) return false;
  return report(attr);
}

bool ZigbeeRadar::setBool(uint16_t attr, bool value) {
  if (setClusterAttribute(RADAR_CLUSTER_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr, &value) != ESP_ZB_ZCL_STATUS_SUCCESS) return false;
  return report(attr);
}

// Report straight to the coordinator (0x0000, ep 1), so no binding of the custom
// cluster is needed.
bool ZigbeeRadar::report(uint16_t attr) {
  // Not joined yet: the value is stored locally and pushed again on connect.
  if (!Zigbee.connected()) return false;
  esp_zb_zcl_report_attr_cmd_t cmd = {};
  cmd.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
  cmd.zcl_basic_cmd.dst_endpoint = 1;
  cmd.zcl_basic_cmd.src_endpoint = _endpoint;
  cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  cmd.clusterID = RADAR_CLUSTER_ID;
  cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
  cmd.manuf_specific = 0;
  cmd.dis_default_resp = 1;
  cmd.attributeID = attr;
  return reportClusterAttribute(&cmd);
}

void ZigbeeRadar::zbAttributeSet(const esp_zb_zcl_set_attr_value_message_t* message) {
  if (message->info.cluster != RADAR_CLUSTER_ID || !_on_write || !message->attribute.data.value) return;

  const uint8_t* value = (const uint8_t*)message->attribute.data.value;
  uint8_t len = message->attribute.data.size;
  // ZCL strings carry their length in the first byte.
  if (message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_OCTET_STRING ||
      message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING) {
    len = value[0];
    value++;
  }
  _on_write(message->attribute.id, value, len);
}
