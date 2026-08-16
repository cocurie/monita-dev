#include "one_hal.h"

#include <InternalFileSystem.h>

#include <string.h>

using namespace Adafruit_LittleFS_Namespace;

namespace one {

// マスター通信はポーリング式なので専用オブジェクトで安全に利用できる。
// 既定WireをsetPins()で使い回さず、配線契約をオブジェクトに固定する。
TwoWire SensorWire(NRF_TWIM0, NRF_TWIS0,
                   SPIM0_SPIS0_TWIM0_TWIS0_SPI0_TWI0_IRQn,
                   ONE_I2C_SDA_PIN, ONE_I2C_SCL_PIN);

namespace {
constexpr uint32_t RADIO_BAUD = 9600;
constexpr uint32_t MODE_SETTLE_MS = 100;
constexpr uint8_t LORA_CONFIG[6] = {0x00, 0x00, 0x68, 0x01, 0x00, 0x80};
bool s_powerOn = false;

void drainRadio(uint32_t maxMs) {
  const uint32_t started = millis();
  while (Serial1.available() && millis() - started < maxMs) Serial1.read();
}

bool readLoRaConfig(uint8_t out[6]) {
  drainRadio(300);
  Serial1.write(static_cast<uint8_t>(0xC1));
  Serial1.write(static_cast<uint8_t>(0x00));
  Serial1.write(static_cast<uint8_t>(0x06));
  uint8_t response[9] = {};
  uint8_t count = 0;
  const uint32_t started = millis();
  while (count < sizeof(response) && millis() - started < 500U) {
    if (Serial1.available()) response[count++] = static_cast<uint8_t>(Serial1.read());
  }
  if (count != sizeof(response) || response[0] != 0xC1 || response[1] != 0 ||
      response[2] != 6) return false;
  memcpy(out, response + 3, 6);
  return true;
}

void writeLoRaConfig() {
  Serial1.write(static_cast<uint8_t>(0xC0));
  Serial1.write(static_cast<uint8_t>(0x00));
  Serial1.write(static_cast<uint8_t>(0x06));
  Serial1.write(LORA_CONFIG, sizeof(LORA_CONFIG));
  Serial1.flush();
  delay(200);
  drainRadio(300);
}

char hexDigit(uint8_t value) {
  return value < 10 ? static_cast<char>('0' + value)
                    : static_cast<char>('A' + value - 10);
}
}  // namespace

void halBegin() {
  // 起動直後から極性と方向を確定。P-chはLOW=ON/HIGH=OFF。
  pinMode(ONE_LORA_MODE_PIN, OUTPUT);
  digitalWrite(ONE_LORA_MODE_PIN, LOW);
  pinMode(ONE_MOSFET_GATE_PIN, OUTPUT);
  digitalWrite(ONE_MOSFET_GATE_PIN, HIGH);
  s_powerOn = false;
}

void setPeripheralPower(bool on) {
  if (on) {
    // 3V3_SWを上げる前にE220入力の方向とLOWを確定し、浮遊入力を作らない。
    pinMode(ONE_LORA_MODE_PIN, OUTPUT);
    digitalWrite(ONE_LORA_MODE_PIN, LOW);
    // OneはFlexと逆極性: LOWでP-ch MOSFET ON。
    digitalWrite(ONE_MOSFET_GATE_PIN, LOW);
    s_powerOn = true;
    delay(20);
  } else {
    // E220保護ダイオード経由の逆給電を防ぐため、必ず電源断より先にLOWへ。
    digitalWrite(ONE_LORA_MODE_PIN, LOW);
    Serial1.end();
    digitalWrite(ONE_MOSFET_GATE_PIN, HIGH);
    s_powerOn = false;
  }
}

bool peripheralPowerIsOn() { return s_powerOn; }

void beginSensorI2c() {
  SensorWire.begin();
  SensorWire.setClock(100000);
}

void endSensorI2c() { SensorWire.end(); }

void beginRadioUart() {
  Serial1.setPins(ONE_UART_RX_PIN, ONE_UART_TX_PIN);
  Serial1.begin(RADIO_BAUD);
}

void endRadioUart() { Serial1.end(); }

void setLoRaModeNormal() {
  digitalWrite(ONE_LORA_MODE_PIN, LOW);
  delay(MODE_SETTLE_MS);
}

void setLoRaModeSleep() {
  // PIR版では3V3_SWが常時ONなのでMode 3で外部無線だけを休止する。
  digitalWrite(ONE_LORA_MODE_PIN, HIGH);
  delay(MODE_SETTLE_MS);
}

bool checkAndConfigureLoRa() {
  setLoRaModeSleep();
  uint8_t current[6] = {};
  if (!readLoRaConfig(current)) {
    setLoRaModeNormal();
    return false;
  }
  if (memcmp(current, LORA_CONFIG, sizeof(current)) != 0) {
    bool verified = false;
    for (uint8_t attempt = 0; attempt < 2 && !verified; ++attempt) {
      writeLoRaConfig();
      verified = readLoRaConfig(current) &&
                 memcmp(current, LORA_CONFIG, sizeof(current)) == 0;
    }
    if (!verified) {
      setLoRaModeNormal();
      return false;
    }
  }
  setLoRaModeNormal();
  return true;
}

void sendLoRaFrame(const uint8_t *payload, uint8_t length) {
  uint8_t checksum = static_cast<uint8_t>(0xAAU + length);
  Serial1.write(static_cast<uint8_t>(0xAA));
  Serial1.write(length);
  for (uint8_t i = 0; i < length; ++i) {
    Serial1.write(payload[i]);
    checksum = static_cast<uint8_t>(checksum + payload[i]);
  }
  Serial1.write(checksum);
  Serial1.flush();
}

bool receiveLoRaFrame(uint8_t *payload, uint8_t capacity, uint8_t &length,
                      uint32_t timeoutMs) {
  enum State { WAIT_SYNC, WAIT_LENGTH, WAIT_BODY, WAIT_CHECKSUM, WAIT_RSSI };
  State state = WAIT_SYNC;
  uint8_t index = 0;
  uint8_t sum = 0;
  length = 0;
  const uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    if (!Serial1.available()) {
      delay(1);
      continue;
    }
    const uint8_t b = static_cast<uint8_t>(Serial1.read());
    switch (state) {
      case WAIT_SYNC:
        if (b == 0xAA) { sum = b; state = WAIT_LENGTH; }
        break;
      case WAIT_LENGTH:
        length = b;
        sum = static_cast<uint8_t>(sum + b);
        index = 0;
        state = (length > 0 && length <= capacity) ? WAIT_BODY : WAIT_SYNC;
        break;
      case WAIT_BODY:
        payload[index++] = b;
        sum = static_cast<uint8_t>(sum + b);
        if (index == length) state = WAIT_CHECKSUM;
        break;
      case WAIT_CHECKSUM:
        state = b == sum ? WAIT_RSSI : WAIT_SYNC;
        break;
      case WAIT_RSSI:
        (void)b;  // REG3=0x80のRSSI付加バイト。設定コマンドには渡さない。
        return true;
    }
  }
  return false;
}

bool sendSigfoxPayload(const uint8_t *payload, size_t length) {
  String command("AT$SF=");
  command.reserve(7 + length * 2);
  for (size_t i = 0; i < length; ++i) {
    command += hexDigit(payload[i] >> 4);
    command += hexDigit(payload[i] & 0x0f);
  }
  Serial1.print(command);
  Serial1.print('\r');
  String response;
  response.reserve(128);
  const uint32_t started = millis();
  while (millis() - started < 10000U) {
    while (Serial1.available()) {
      const char c = static_cast<char>(Serial1.read());
      if (response.length() < 2048U) response += c;
    }
    if (response.indexOf("OK") >= 0) return true;
    delay(1);
  }
  return false;
}

uint16_t readBatteryMv() {
  // variant.hの実値: VBAT_ENABLE=P0.14/D14、LOWで分圧回路が有効。
  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, LOW);
  delay(2);
  analogReadResolution(12);
  const uint32_t raw = analogRead(PIN_VBAT);
  digitalWrite(VBAT_ENABLE, HIGH);
  // Adafruit nRF52 coreのAR_DEFAULTは0.6V×6=3.6V full-scale。
  // 1510k/510kの分圧比を復元し、整数のround half upでmV化する。
  const uint64_t numerator = static_cast<uint64_t>(raw) * 3600ULL * 2020ULL;
  const uint32_t mv = static_cast<uint32_t>((numerator + 4095ULL * 510ULL / 2ULL) /
                                             (4095ULL * 510ULL));
  return mv > 65535U ? 65535U : static_cast<uint16_t>(mv);
}

int16_t readCpuTemperatureDeciC() {
  const float value = readCPUTemperature() * 10.0f;
  return static_cast<int16_t>(value >= 0 ? value + 0.5f : value - 0.5f);
}

uint32_t crc32(const void *data, size_t length) {
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  uint32_t crc = 0xffffffffUL;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320UL : 0U);
  }
  return ~crc;
}

bool readFile(const char *path, void *data, size_t length) {
  if (!InternalFS.begin()) return false;
  File file(InternalFS);
  if (!file.open(path, FILE_O_READ)) return false;
  const int readLength = file.read(static_cast<uint8_t *>(data), length);
  const bool ok = static_cast<size_t>(file.size()) == length && readLength >= 0 &&
                  static_cast<size_t>(readLength) == length;
  file.close();
  return ok;
}

bool writeFile(const char *path, const void *data, size_t length) {
  if (!InternalFS.begin()) return false;
  InternalFS.remove(path);
  File file(InternalFS);
  if (!file.open(path, FILE_O_WRITE)) return false;
  const bool ok = file.write(static_cast<const uint8_t *>(data), length) == length;
  file.close();
  return ok;
}

void watchdogBegin(uint32_t timeoutMs) {
  uint64_t ticks = static_cast<uint64_t>(timeoutMs) * 32768ULL / 1000ULL;
  if (ticks > 0xffffffffULL) ticks = 0xffffffffULL;
  NRF_WDT->CONFIG = WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos;
  NRF_WDT->CRV = static_cast<uint32_t>(ticks);
  NRF_WDT->RREN = WDT_RREN_RR0_Msk;
  NRF_WDT->TASKS_START = 1;
}

void watchdogFeed() { NRF_WDT->RR[0] = WDT_RR_RR_Reload; }

}  // namespace one
