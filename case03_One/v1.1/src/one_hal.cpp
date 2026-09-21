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
// AUX待ちの上限。v1.00で実績のある固定待ちは下限として残し、AUXはその後の確認に使う。
// AUXが来ない（Sigfox実装機・未配線・故障）場合も上限で抜けて従来どおり動く。
constexpr uint32_t AUX_MODE_TIMEOUT_MS = 1000;
constexpr uint32_t AUX_TX_START_MS = 50;      // 書込み後にAUXがLOWへ落ちるまでの猶予
constexpr uint32_t AUX_TX_TIMEOUT_MS = 3000;  // エアタイム込みの送信完了待ち上限
constexpr uint32_t AUX_SETTLE_MS = 2;         // AUX=HIGH後、次の操作までの間隔
constexpr uint32_t CHARGE_PULLUP_SETTLE_MS = 2;
constexpr size_t LORA_FRAME_OVERHEAD = 3;     // 0xAA, LEN, checksum
bool s_powerOn = false;

#ifdef COMM_MODE_LORA
constexpr bool USE_LORA_AUX = true;
#else
constexpr bool USE_LORA_AUX = false;  // Sigfox実装機ではD2に何もつながっていない
#endif

// nRF52のリセット既定（入力・入力バッファ切断・プル無し）へ戻す。
// OFF中の外部回路へ電流を流さず、浮いた入力で貫通電流も生まない。
void disconnectPin(uint8_t pin) {
  nrf_gpio_cfg_default(g_ADigitalPinMap[pin]);
}

void drainRadio(uint32_t maxMs) {
  const uint32_t started = millis();
  while (Serial1.available() && millis() - started < maxMs) Serial1.read();
}

bool readLoRaConfig(uint8_t out[6]) {
  drainRadio(300);
  const uint8_t command[3] = {0xC1, 0x00, 0x06};
  Serial1.write(command, sizeof(command));
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
  uint8_t command[3 + sizeof(LORA_CONFIG)] = {0xC0, 0x00, 0x06};
  memcpy(command + 3, LORA_CONFIG, sizeof(LORA_CONFIG));
  Serial1.write(command, sizeof(command));
  Serial1.flush();
  delay(200);
  waitLoRaIdle(AUX_MODE_TIMEOUT_MS);
  drainRadio(300);
}

char hexDigit(uint8_t value) {
  return value < 10 ? static_cast<char>('0' + value)
                    : static_cast<char>('A' + value - 10);
}
}  // namespace

void halBegin() {
  // 起動直後から極性と方向を確定。P-chはLOW=ON/HIGH=OFF。
  // ★出力ラッチを先に書いてからOUTPUTにする。逆順だとリセット直後のラッチ(LOW)が
  // 一瞬出て3V3_SWがONになる（v1.01 MD §D-5）。nRF52のdigitalWriteは入力中でもラッチを書く。
  digitalWrite(ONE_LORA_MODE_PIN, LOW);
  pinMode(ONE_LORA_MODE_PIN, OUTPUT);
  digitalWrite(ONE_MOSFET_GATE_PIN, HIGH);
  pinMode(ONE_MOSFET_GATE_PIN, OUTPUT);
  s_powerOn = false;
  // ★VBAT_ENABLE(P0.14)は常時LOW。分圧は BAT+ ─1MΩ─ P0.31 ─510kΩ─ P0.14。
  // P0.14をHIGH(3.3V)にするとP0.31は 3.3+(4.2-3.3)×510/1510 ≒ 3.60V となり、
  // 満充電時にnRF52840の絶対最大定格(VDD+0.3V)の上限へ達する(Seeed Wikiの注意)。
  // LOW固定時の分圧電流は約3µA(4.2V/1.51MΩ)で、消費への影響は無視できる。
  digitalWrite(VBAT_ENABLE, LOW);
  pinMode(VBAT_ENABLE, OUTPUT);
  // v1.1追加の3本は使うときだけ設定する。既定は切断（プル無し）。
  disconnectPin(ONE_ST_CHRG_PIN);
  disconnectPin(ONE_ST_DONE_PIN);
  disconnectPin(ONE_LORA_AUX_PIN);
}

void setPeripheralPower(bool on) {
  if (on) {
    // 3V3_SWを上げる前にE220入力の方向とLOWを確定し、浮遊入力を作らない。
    digitalWrite(ONE_LORA_MODE_PIN, LOW);
    pinMode(ONE_LORA_MODE_PIN, OUTPUT);
    // OneはFlexと逆極性: LOWでP-ch MOSFET ON。
    digitalWrite(ONE_MOSFET_GATE_PIN, LOW);
    s_powerOn = true;
    delay(20);
    // E220のAUXはプッシュプル出力。プル無し入力で受ける（通電中だけ）。
    if (USE_LORA_AUX) pinMode(ONE_LORA_AUX_PIN, INPUT);
  } else {
    // E220保護ダイオード経由の逆給電を防ぐため、必ず電源断より先にLOWへ。
    digitalWrite(ONE_LORA_MODE_PIN, LOW);
    SensorWire.end();
    Serial1.end();
    // ★電源断より先に、OFFになる回路へつながる信号をすべて切り離す（v1.01 MD §D-4）。
    // HX711のpower_down()はPD_SCKをHIGHで残す。ラッチもLOWへ戻し、次回begin()で
    // OUTPUTにした瞬間にHIGHが出てHX711をパワーダウンさせないようにする。
    digitalWrite(ONE_PD_SCK_PIN, LOW);
    disconnectPin(ONE_PD_SCK_PIN);
    disconnectPin(ONE_DOUT_PIN);
    disconnectPin(ONE_I2C_SDA_PIN);
    disconnectPin(ONE_I2C_SCL_PIN);
    disconnectPin(ONE_UART_TX_PIN);
    disconnectPin(ONE_UART_RX_PIN);
    disconnectPin(ONE_LORA_AUX_PIN);
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
  // setPeripheralPower(false)で切断したピンを戻す。コアのUart::begin()はPSELを設定するだけで
  // GPIOの入力バッファを接続し直さないため、RXは明示的に入力へ、TXはアイドルHIGHへ戻す。
  pinMode(ONE_UART_RX_PIN, INPUT);
  digitalWrite(ONE_UART_TX_PIN, HIGH);
  pinMode(ONE_UART_TX_PIN, OUTPUT);
  Serial1.setPins(ONE_UART_RX_PIN, ONE_UART_TX_PIN);
  Serial1.begin(RADIO_BAUD);
}

void endRadioUart() { Serial1.end(); }

bool waitLoRaIdle(uint32_t timeoutMs) {
  if (!USE_LORA_AUX) return true;
  if (!s_powerOn) return false;
  const uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    if (digitalRead(ONE_LORA_AUX_PIN) == HIGH) {
      // 立上り直後の一瞬のHIGHで抜けないよう、間隔を置いて再確認する。
      delay(AUX_SETTLE_MS);
      if (digitalRead(ONE_LORA_AUX_PIN) == HIGH) return true;
    } else {
      delay(1);
    }
  }
  Serial.println("[LORA] AUX待ちタイムアウト（固定待ちで続行）");
  return false;
}

void setLoRaModeNormal() {
  waitLoRaIdle(AUX_MODE_TIMEOUT_MS);
  digitalWrite(ONE_LORA_MODE_PIN, LOW);
  delay(MODE_SETTLE_MS);
  waitLoRaIdle(AUX_MODE_TIMEOUT_MS);
}

void setLoRaModeSleep() {
  // PIR版では3V3_SWが常時ONなのでMode 3で外部無線だけを休止する。
  waitLoRaIdle(AUX_MODE_TIMEOUT_MS);
  digitalWrite(ONE_LORA_MODE_PIN, HIGH);
  delay(MODE_SETTLE_MS);
  waitLoRaIdle(AUX_MODE_TIMEOUT_MS);
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
  // フレームは1回のブロック書込みで送る（1バイトずつだとDMA転送が細切れになる。
  // 開発メモ 20260807_LoRaダウンリンク_子機設定変更_E220送信の落とし穴.md ②）。
  uint8_t frame[LORA_FRAME_OVERHEAD + 255];
  uint8_t checksum = static_cast<uint8_t>(0xAAU + length);
  frame[0] = 0xAA;
  frame[1] = length;
  for (uint8_t i = 0; i < length; ++i) {
    frame[2 + i] = payload[i];
    checksum = static_cast<uint8_t>(checksum + payload[i]);
  }
  frame[2 + length] = checksum;
  Serial1.write(frame, LORA_FRAME_OVERHEAD + length);
  Serial1.flush();
  if (!USE_LORA_AUX || !s_powerOn) return;
  // 書込み直後はAUXがまだHIGHのことがある。LOWへ落ちるのを短く待ってから完了を待つ。
  const uint32_t started = millis();
  while (digitalRead(ONE_LORA_AUX_PIN) == HIGH && millis() - started < AUX_TX_START_MS)
    delay(1);
  waitLoRaIdle(AUX_TX_TIMEOUT_MS);
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
  // ★読取後もHIGHに戻さない(halBegin()の注意を参照)。念のためここでもLOWを確定する。
  digitalWrite(VBAT_ENABLE, LOW);
  pinMode(VBAT_ENABLE, OUTPUT);
  delay(2);
  analogReadResolution(12);
  const uint32_t raw = analogRead(PIN_VBAT);
  // Adafruit nRF52 coreのAR_DEFAULTは0.6V×6=3.6V full-scale。
  // 分圧 1MΩ(上)/510kΩ(下) の比 (1000k+510k)/510k を復元し、整数のround half upでmV化する。
  // ★v1.00〜FW 0x05初版は 2020/510 で計算しており約1.34倍高く読んでいた(2026-09-16修正)。
  const uint64_t numerator = static_cast<uint64_t>(raw) * 3600ULL * 1510ULL;
  const uint32_t mv = static_cast<uint32_t>((numerator + 4095ULL * 510ULL / 2ULL) /
                                             (4095ULL * 510ULL));
  return mv > 65535U ? 65535U : static_cast<uint16_t>(mv);
}

int16_t readCpuTemperatureDeciC() {
  const float value = readCPUTemperature() * 10.0f;
  return static_cast<int16_t>(value >= 0 ? value + 0.5f : value - 0.5f);
}

ChargeState readChargeState() {
  // ★プルアップは読む瞬間だけ有効にする。常時有効だと、日照がなくCN3063のVINが0Vの間に
  // ダイオード→CHRG/DONE端子経由で約250µAが流れ続ける可能性がある（要実測）。
  pinMode(ONE_ST_CHRG_PIN, INPUT_PULLUP);
  pinMode(ONE_ST_DONE_PIN, INPUT_PULLUP);
  delay(CHARGE_PULLUP_SETTLE_MS);
  const bool chrgLow = digitalRead(ONE_ST_CHRG_PIN) == LOW;
  const bool doneLow = digitalRead(ONE_ST_DONE_PIN) == LOW;
  disconnectPin(ONE_ST_CHRG_PIN);
  disconnectPin(ONE_ST_DONE_PIN);
  return decodeChargeState(chrgLow, doneLow);
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
