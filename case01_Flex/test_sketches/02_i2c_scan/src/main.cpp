/**
 * Monita Flex v3.02 — 検証 Step2: I2Cスキャン
 *
 * 確認内容:
 *   I2Cバス上に期待するデバイスが見えるか
 *
 * 期待するアドレス:
 *   0x20 … TCA9534（4052 MUX A/B制御）
 *   0x70 … TCA9546A（I2Cバス切替。MPU用）
 *
 * 注意:
 *   3V3_SW（D10）をHIGHにしてセンサ側レールを起動した状態でスキャンする。
 *   D10をLOWのままだとTCA9534/TCA9546Aが電源なしで応答しない。
 *
 * 期待出力例:
 *   [I2C Scan] 開始...
 *   0x20: TCA9534 候補
 *   0x70: TCA9546A 候補
 *   検出: 2個
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>

static const uint8_t SW_POWER_PIN = 10;

// 既知アドレスに名前をつける
static const char* knownDevice(uint8_t addr) {
  switch (addr) {
    case 0x20: return "TCA9534 (4052 MUX制御)";
    case 0x70: return "TCA9546A (I2Cバス切替)";
    case 0x68: return "MPU6050 (加速度センサ)";
    default:   return "";
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) yield();

  // 3V3_SWをON（TCAはこのレールで動く）
  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);  // レール安定待ち

  Wire.begin();

  Serial.println(F("\n[I2C Scan] 開始..."));
  Serial.println(F("期待: 0x20=TCA9534, 0x70=TCA9546A"));
  Serial.println(F("----------------------------"));
}

void loop() {
  int found = 0;

  for (uint8_t addr = 0x01; addr < 0x7F; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();

    if (err == 0) {
      Serial.print(F("0x"));
      if (addr < 0x10) Serial.print('0');
      Serial.print(addr, HEX);

      const char* name = knownDevice(addr);
      if (strlen(name) > 0) {
        Serial.print(F(" : "));
        Serial.print(name);
      } else {
        Serial.print(F(" : 未知のデバイス"));
      }
      Serial.println();
      found++;
    }
  }

  Serial.print(F("検出: "));
  Serial.print(found);
  Serial.println(F("個"));

  if (found == 0) {
    Serial.println(F("✗ デバイスなし → 配線/はんだ/3V3_SWを確認"));
  } else {
    bool tca9534  = false;
    bool tca9546a = false;
    // 再チェック
    Wire.beginTransmission(0x20); tca9534  = (Wire.endTransmission() == 0);
    Wire.beginTransmission(0x70); tca9546a = (Wire.endTransmission() == 0);

    Serial.print(F("TCA9534  (0x20): ")); Serial.println(tca9534  ? F("✓ OK") : F("✗ 見えない"));
    Serial.print(F("TCA9546A (0x70): ")); Serial.println(tca9546a ? F("✓ OK") : F("✗ 見えない"));
  }

  Serial.println(F("--- 5秒後に再スキャン ---\n"));
  delay(5000);
}
