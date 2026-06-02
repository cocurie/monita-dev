/**
 * Monita Flex v3.02 — 検証 Step3: TCA9534 動作確認
 *
 * 確認内容:
 *   TCA9534（0x20）のP0/P1出力を切り替えて
 *   4052 MUX の A/B ラインが変化するか確認する
 *
 * TCA9534 ピン割当（v3.02正本より）:
 *   P0 = MUX_B  (SN74LV4052 B入力)
 *   P1 = MUX_A  (SN74LV4052 A入力)
 *   P2〜P7 = 入力（未使用）
 *
 * MUXチャネルとA/Bの対応:
 *   CH1: A=0, B=0  (idx=0)
 *   CH2: A=1, B=0  (idx=1)
 *   CH3: A=0, B=1  (idx=2)
 *   CH4: A=1, B=1  (idx=3)
 *
 * 確認方法:
 *   テスターで4052のA/Bピンを計測し、シリアルログと一致するか見る。
 *   テスターがなければシリアルログでI2C応答だけ確認してもOK。
 *
 * 期待出力例:
 *   [TCA9534] init OK
 *   [MUX] CH1: A=0 B=0 → P0=0 P1=0
 *   [MUX] CH2: A=1 B=0 → P0=0 P1=1
 *   [MUX] CH3: A=0 B=1 → P0=1 P1=0
 *   [MUX] CH4: A=1 B=1 → P0=1 P1=1
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>

static const uint8_t SW_POWER_PIN  = 10;
static const uint8_t TCA9534_ADDR  = 0x20;

// TCA9534 レジスタ
static const uint8_t REG_OUTPUT = 0x01;
static const uint8_t REG_CONFIG = 0x03;

static bool tca9534WriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool tca9534ReadReg(uint8_t reg, uint8_t *out) {
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TCA9534_ADDR, (uint8_t)1) != 1) return false;
  *out = Wire.read();
  return true;
}

// P0=MUX_B, P1=MUX_A を出力設定し、A=0 B=0 の初期状態にする
static bool tca9534Init() {
  if (!tca9534WriteReg(REG_OUTPUT, 0x00)) return false;  // 出力0クリア
  if (!tca9534WriteReg(REG_CONFIG, 0xFC)) return false;  // P0/P1=出力, P2〜7=入力
  return true;
}

// ch: 1〜4 → P0(MUX_B), P1(MUX_A) を設定
static bool muxSelect(uint8_t ch) {
  uint8_t idx = (ch - 1) & 0x03;
  uint8_t a   = (idx     ) & 0x01;  // LSB = A
  uint8_t b   = (idx >> 1) & 0x01;  // bit1 = B
  // P0=B, P1=A
  uint8_t twoBits = (uint8_t)(b | (a << 1));

  uint8_t out = 0;
  if (!tca9534ReadReg(REG_OUTPUT, &out)) return false;
  out = (out & 0xFC) | (twoBits & 0x03);
  return tca9534WriteReg(REG_OUTPUT, out);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) yield();

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  Wire.begin();

  Serial.println(F("\n[STEP3] TCA9534 動作確認"));
  Serial.println(F("テスター: 4052のAピン・Bピンの電圧をシリアルログと照合"));
  Serial.println(F("-----------------------------------"));

  if (tca9534Init()) {
    Serial.println(F("[TCA9534] init OK"));
  } else {
    Serial.println(F("[TCA9534] init FAILED → I2Cアドレス/配線を確認"));
    while (true) delay(1000);  // ここで停止
  }
}

void loop() {
  for (uint8_t ch = 1; ch <= 4; ch++) {
    uint8_t idx = ch - 1;
    uint8_t a   = idx & 0x01;
    uint8_t b   = (idx >> 1) & 0x01;

    bool ok = muxSelect(ch);

    Serial.print(F("[MUX] CH"));
    Serial.print(ch);
    Serial.print(F(": A="));
    Serial.print(a);
    Serial.print(F(" B="));
    Serial.print(b);
    Serial.print(F(" → P1="));
    Serial.print(a);
    Serial.print(F(" P0="));
    Serial.print(b);
    Serial.println(ok ? F("  OK") : F("  ✗ I2C ERROR"));

    delay(2000);  // テスターで測る時間
  }

  Serial.println(F("--- 1周完了 ---\n"));
}
