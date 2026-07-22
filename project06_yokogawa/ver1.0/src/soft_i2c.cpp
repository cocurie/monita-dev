#include "soft_i2c.h"

static uint8_t s_sda = 0;
static uint8_t s_scl = 0;
static constexpr uint16_t BIT_DELAY_US = 50;      // 約10kHz相当（ACK応答タイミングに余裕、切り分け用）
static constexpr uint16_t STRETCH_TIMEOUT_MS = 50; // クロックストレッチング最大待ち時間

// オープンドレイン動作：Lowは能動駆動、Highはプルアップ任せ（INPUT化）
static inline void sdaLow()  { pinMode(s_sda, OUTPUT); digitalWrite(s_sda, LOW); }
static inline void sdaHigh() { pinMode(s_sda, INPUT_PULLUP); }
static inline void sclLow()  { pinMode(s_scl, OUTPUT); digitalWrite(s_scl, LOW); }
static inline void sclHigh() { pinMode(s_scl, INPUT_PULLUP); }
static inline int  sdaRead() { return digitalRead(s_sda); }
static inline int  sclRead() { return digitalRead(s_scl); }

// SCLをHighに離し、相手がクロックストレッチングしていれば解除まで待つ
static bool sclReleaseAndWaitHigh() {
  sclHigh();
  delayMicroseconds(BIT_DELAY_US);
  uint32_t start = millis();
  while (sclRead() == LOW) {
    if (millis() - start > STRETCH_TIMEOUT_MS) return false; // タイムアウト
  }
  return true;
}

static void i2cStart() {
  sdaHigh();
  sclHigh();
  delayMicroseconds(BIT_DELAY_US);
  sdaLow();
  delayMicroseconds(BIT_DELAY_US);
  sclLow();
  delayMicroseconds(BIT_DELAY_US);
}

static void i2cRepeatedStart() {
  sdaHigh();
  delayMicroseconds(BIT_DELAY_US);
  sclReleaseAndWaitHigh();
  delayMicroseconds(BIT_DELAY_US);
  sdaLow();
  delayMicroseconds(BIT_DELAY_US);
  sclLow();
  delayMicroseconds(BIT_DELAY_US);
}

static void i2cStop() {
  sdaLow();
  delayMicroseconds(BIT_DELAY_US);
  sclReleaseAndWaitHigh();
  delayMicroseconds(BIT_DELAY_US);
  sdaHigh();
  delayMicroseconds(BIT_DELAY_US);
}

// 1バイト送信し、相手のACKビットを返す（true=ACK）
static bool i2cWriteByte(uint8_t data) {
  for (int8_t i = 7; i >= 0; i--) {
    if (data & (1 << i)) sdaHigh(); else sdaLow();
    delayMicroseconds(BIT_DELAY_US);
    if (!sclReleaseAndWaitHigh()) return false;
    delayMicroseconds(BIT_DELAY_US);
    sclLow();
  }
  // ACKビット読み取り（9bit目、SDAを離す）
  sdaHigh();
  delayMicroseconds(BIT_DELAY_US);
  if (!sclReleaseAndWaitHigh()) return false;
  bool ack = (sdaRead() == LOW);
  delayMicroseconds(BIT_DELAY_US);
  sclLow();
  return ack;
}

// 1バイト受信。sendAck=trueなら継続読み取り(ACK)、falseなら最終バイト(NACK)
static uint8_t i2cReadByte(bool sendAck) {
  uint8_t data = 0;
  sdaHigh(); // 受信中はマスタがSDAを離す
  for (uint8_t i = 0; i < 8; i++) {
    delayMicroseconds(BIT_DELAY_US);
    if (!sclReleaseAndWaitHigh()) { /* タイムアウトでも読めた分は返す */ }
    data = (data << 1) | (sdaRead() == HIGH ? 1 : 0);
    sclLow();
  }
  // ACK/NACK送出
  if (sendAck) sdaLow(); else sdaHigh();
  delayMicroseconds(BIT_DELAY_US);
  sclReleaseAndWaitHigh();
  delayMicroseconds(BIT_DELAY_US);
  sclLow();
  sdaHigh();
  return data;
}

void softI2CInit(uint8_t sdaPin, uint8_t sclPin) {
  s_sda = sdaPin;
  s_scl = sclPin;
  sdaHigh();
  sclHigh();
}

bool softI2CProbe(uint8_t addr7) {
  i2cStart();
  bool ack = i2cWriteByte((addr7 << 1) | 0); // Write方向
  i2cStop();
  return ack;
}

bool softI2CWriteReg(uint8_t addr7, uint8_t reg, uint8_t val) {
  i2cStart();
  if (!i2cWriteByte((addr7 << 1) | 0)) { i2cStop(); return false; }
  if (!i2cWriteByte(reg))              { i2cStop(); return false; }
  if (!i2cWriteByte(val))              { i2cStop(); return false; }
  i2cStop();
  return true;
}

bool softI2CReadReg(uint8_t addr7, uint8_t reg, uint8_t *buf, uint8_t len) {
  i2cStart();
  if (!i2cWriteByte((addr7 << 1) | 0)) { i2cStop(); return false; }
  if (!i2cWriteByte(reg))              { i2cStop(); return false; }
  i2cRepeatedStart();
  if (!i2cWriteByte((addr7 << 1) | 1)) { i2cStop(); return false; }
  for (uint8_t i = 0; i < len; i++) {
    bool more = (i < len - 1);
    buf[i] = i2cReadByte(more);
  }
  i2cStop();
  return true;
}
