#include "Debug.h"
#include "Ld2410.h"

static const uint8_t CMD_HEADER[4] = {0xFD, 0xFC, 0xFB, 0xFA};
static const uint8_t CMD_TAIL[4] = {0x04, 0x03, 0x02, 0x01};
static const uint8_t DATA_HEADER[4] = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t DATA_TAIL[4] = {0xF8, 0xF7, 0xF6, 0xF5};

static const uint16_t CMD_ENABLE_CONFIG = 0x00FF;
static const uint16_t CMD_END_CONFIG = 0x00FE;
static const uint16_t CMD_SET_MAX_VALUES = 0x0060;
static const uint16_t CMD_READ_CONFIG = 0x0061;
static const uint16_t CMD_ENGINEERING_ON = 0x0062;
static const uint16_t CMD_ENGINEERING_OFF = 0x0063;
static const uint16_t CMD_SET_GATE_SENSITIVITY = 0x0064;
static const uint16_t CMD_READ_FIRMWARE = 0x00A0;
static const uint16_t CMD_FACTORY_RESET = 0x00A2;
static const uint16_t CMD_RESTART = 0x00A3;
static const uint16_t CMD_BLUETOOTH = 0x00A4;
static const uint16_t CMD_SET_RESOLUTION = 0x00AA;
static const uint16_t CMD_READ_RESOLUTION = 0x00AB;

static uint16_t u16(const uint8_t* p) { return p[0] | (p[1] << 8); }

// Parameter word + 32-bit value, the layout used by the 0x0060 / 0x0064 commands.
static uint8_t* putParam(uint8_t* p, uint16_t word, uint32_t value) {
  *p++ = word & 0xFF;
  *p++ = word >> 8;
  for (int i = 0; i < 4; i++) *p++ = (value >> (8 * i)) & 0xFF;
  return p;
}

void Ld2410::begin(HardwareSerial& uart, int rx_pin, int tx_pin) {
  _uart = &uart;
  _uart->setRxBufferSize(1024);
  _uart->begin(256000, SERIAL_8N1, rx_pin, tx_pin);
}

bool Ld2410::loop() {
  bool fresh = false;
  while (_uart && _uart->available()) {
    if (feed(_uart->read())) fresh = true;
  }
  return fresh;
}

bool Ld2410::connected() const {
  return _last_data != 0 && millis() - _last_data < 1000;
}

// Byte-wise frame assembler. Resyncs on the 4-byte header and uses the length field
// to know where the frame ends. Returns true when a data frame was decoded.
bool Ld2410::feed(uint8_t b) {
  _bytes++;
  _last_byte = millis() | 1;
  if (_pos < 4) {
    const uint8_t* header = nullptr;
    if (_pos == 0) {
      header = b == CMD_HEADER[0] ? CMD_HEADER : b == DATA_HEADER[0] ? DATA_HEADER : nullptr;
    } else {
      header = _frame[0] == CMD_HEADER[0] ? CMD_HEADER : DATA_HEADER;
    }
    if (header && b == header[_pos]) {
      _frame[_pos++] = b;
    } else {
      // Mismatch: this byte may itself start a new header.
      _pos = 0;
      if (b == CMD_HEADER[0] || b == DATA_HEADER[0]) _frame[_pos++] = b;
    }
    return false;
  }

  _frame[_pos++] = b;
  if (_pos == 6) {
    _expected = 10 + u16(&_frame[4]);
    if (_expected > MAX_FRAME) {
      _pos = 0;
      return false;
    }
  }
  if (_pos < 6 || _pos < _expected) return false;

  bool fresh = parseFrame();
  _pos = 0;
  return fresh;
}

bool Ld2410::parseFrame() {
  const uint8_t* tail = &_frame[_expected - 4];
  const uint8_t* payload = &_frame[6];
  size_t len = _expected - 10;

  if (_frame[0] == DATA_HEADER[0]) {
    if (memcmp(tail, DATA_TAIL, 4) != 0) return false;
    return parseData(payload, len);
  }

  if (memcmp(tail, CMD_TAIL, 4) != 0 || len < 4) return false;
  _ack_cmd = u16(payload);
  _ack_len = len - 2;
  memcpy(_ack, payload + 2, _ack_len);
  _ack_ready = true;
  return false;
}

bool Ld2410::parseData(const uint8_t* p, size_t len) {
  // type(1) AA(1) state(1) movDist(2) movE(1) statDist(2) statE(1) detDist(2) ... 55 00
  if (len < 13 || p[1] != 0xAA || p[len - 2] != 0x55) return false;

  Target t;
  t.state = p[2];
  t.movingDistance = u16(&p[3]);
  t.movingEnergy = p[5];
  t.staticDistance = u16(&p[6]);
  t.staticEnergy = p[8];
  t.detectionDistance = u16(&p[9]);

  if (p[0] == 0x01 && len >= 13 + 2) {
    // Engineering: maxMovGate N(1) maxStatGate M(1) movGates[N+1] statGates[M+1] light out
    uint8_t n = p[11], m = p[12];
    size_t needed = 13 + (n + 1) + (m + 1) + 2 + 2;
    if (n < GATES && m < GATES && len >= needed) {
      const uint8_t* q = &p[13];
      for (uint8_t i = 0; i <= n; i++) t.movingGates[i] = *q++;
      for (uint8_t i = 0; i <= m; i++) t.staticGates[i] = *q++;
      t.light = *q++;
      t.out = *q++;
      t.engineering = true;
    }
  }

  _target = t;
  _last_data = millis();
  _frames++;
  return true;
}

bool Ld2410::command(uint16_t cmd, const uint8_t* value, size_t value_len,
                     uint8_t* ack, size_t* ack_len, uint32_t timeout_ms) {
  if (!_uart) return false;

  uint16_t len = 2 + value_len;
  uint8_t head[8] = {CMD_HEADER[0], CMD_HEADER[1], CMD_HEADER[2], CMD_HEADER[3],
                     (uint8_t)(len & 0xFF), (uint8_t)(len >> 8),
                     (uint8_t)(cmd & 0xFF), (uint8_t)(cmd >> 8)};
  _ack_ready = false;
  _uart->write(head, sizeof(head));
  if (value_len) _uart->write(value, value_len);
  _uart->write(CMD_TAIL, sizeof(CMD_TAIL));
  _uart->flush();

  unsigned long start = millis();
  while (millis() - start < timeout_ms) {
    while (_uart->available()) {
      feed(_uart->read());
      if (_ack_ready && _ack_cmd == (cmd | 0x0100)) {
        bool ok = _ack_len >= 2 && u16(_ack) == 0;
        if (ack && ack_len) {
          *ack_len = _ack_len - 2;
          memcpy(ack, _ack + 2, *ack_len);
        }
        return ok;
      }
    }
    delay(1);
  }
  DBG_PRINTF("[LD2410] no ACK for command 0x%04X\n", cmd);
  return false;
}

bool Ld2410::enterConfig() {
  const uint8_t value[2] = {0x01, 0x00};
  // The first command after the radar was busy streaming is occasionally dropped.
  return command(CMD_ENABLE_CONFIG, value, 2) || command(CMD_ENABLE_CONFIG, value, 2);
}

bool Ld2410::leaveConfig() {
  return command(CMD_END_CONFIG);
}

bool Ld2410::configCommand(uint16_t cmd, const uint8_t* value, size_t value_len,
                           uint8_t* ack, size_t* ack_len) {
  if (!enterConfig()) return false;
  bool ok = command(cmd, value, value_len, ack, ack_len);
  leaveConfig();
  return ok;
}

bool Ld2410::readConfig(Config& config) {
  uint8_t ack[MAX_FRAME];
  size_t len = 0;
  // AA N maxMov maxStat mov[9] stat[9] duration(2)
  if (!configCommand(CMD_READ_CONFIG, nullptr, 0, ack, &len) || len < 24 || ack[0] != 0xAA) {
    return false;
  }
  config.maxMovingGate = ack[2];
  config.maxStaticGate = ack[3];
  memcpy(config.moving, &ack[4], GATES);
  memcpy(config.stat, &ack[13], GATES);
  config.noOneDuration = u16(&ack[22]);
  return true;
}

bool Ld2410::writeConfig(const Config& wanted, const Config& current) {
  if (!enterConfig()) return false;
  bool ok = true;

  if (wanted.maxMovingGate != current.maxMovingGate || wanted.maxStaticGate != current.maxStaticGate ||
      wanted.noOneDuration != current.noOneDuration) {
    uint8_t value[18];
    uint8_t* p = putParam(value, 0x0000, wanted.maxMovingGate);
    p = putParam(p, 0x0001, wanted.maxStaticGate);
    putParam(p, 0x0002, wanted.noOneDuration);
    ok &= command(CMD_SET_MAX_VALUES, value, sizeof(value));
  }

  for (uint8_t gate = 0; gate < GATES; gate++) {
    if (wanted.moving[gate] == current.moving[gate] && wanted.stat[gate] == current.stat[gate]) continue;
    uint8_t value[18];
    uint8_t* p = putParam(value, 0x0000, gate);
    p = putParam(p, 0x0001, wanted.moving[gate]);
    putParam(p, 0x0002, wanted.stat[gate]);
    if (!command(CMD_SET_GATE_SENSITIVITY, value, sizeof(value))) {
      DBG_PRINTF("[LD2410] gate %u sensitivity rejected\n", gate);
      ok = false;
    }
  }

  leaveConfig();
  return ok;
}

bool Ld2410::readFirmware(char* out, size_t len) {
  uint8_t ack[MAX_FRAME];
  size_t ack_len = 0;
  // type(2) major(2) minor(4)
  if (!configCommand(CMD_READ_FIRMWARE, nullptr, 0, ack, &ack_len) || ack_len < 8) return false;
  uint16_t major = u16(&ack[2]);
  uint32_t minor = u16(&ack[4]) | ((uint32_t)u16(&ack[6]) << 16);
  snprintf(out, len, "V%X.%02X.%08lX", major >> 8, major & 0xFF, (unsigned long)minor);
  return true;
}

bool Ld2410::readResolution(uint8_t& resolution) {
  uint8_t ack[MAX_FRAME];
  size_t len = 0;
  if (!configCommand(CMD_READ_RESOLUTION, nullptr, 0, ack, &len) || len < 2) return false;
  resolution = ack[0];
  return true;
}

bool Ld2410::setResolution(uint8_t resolution) {
  const uint8_t value[2] = {resolution, 0x00};
  return configCommand(CMD_SET_RESOLUTION, value, 2);
}

bool Ld2410::setBluetooth(bool on) {
  const uint8_t value[2] = {(uint8_t)(on ? 0x01 : 0x00), 0x00};
  return configCommand(CMD_BLUETOOTH, value, 2);
}

bool Ld2410::setEngineering(bool on) {
  return configCommand(on ? CMD_ENGINEERING_ON : CMD_ENGINEERING_OFF);
}

bool Ld2410::restart() {
  // No leaveConfig(): the radar reboots and comes back in normal mode.
  return enterConfig() && command(CMD_RESTART);
}

bool Ld2410::factoryReset() {
  return configCommand(CMD_FACTORY_RESET);
}
