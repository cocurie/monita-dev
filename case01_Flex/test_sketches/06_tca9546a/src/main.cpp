/**
 * Monita Flex v3.02 — 検証 Step6: TCA9546A チャンネル切り替え確認
 *
 * 確認内容:
 *   TCA9546A（0x70）の CH0〜CH3 を順に有効にして
 *   各チャンネル下流のI2Cバスをスキャンし、デバイスが見えるか確認する
 *
 * TCA9546A の仕組み:
 *   0x70 に 1バイト書くだけでチャンネルを選択する
 *   0x01 = CH0, 0x02 = CH1, 0x04 = CH2, 0x08 = CH3
 *   0x00 = 全チャンネル切断
 *
 * 期待出力例（MPU6050が各CHに繋がっている場合）:
 *   [CH0] I2Cスキャン中...
 *     0x68: MPU6050 候補
 *     検出: 1個
 *   [CH1] I2Cスキャン中...
 *     0x68: MPU6050 候補
 *     検出: 1個
 *   ...
 *
 * 未接続のチャンネルは「検出: 0個」になる（正常）
 *
 * 注意:
 *   TCA9546A は 3V3_SW 側の電源で動く。
 *   D10 HIGH にしてから Wire.begin() すること。
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>

static const uint8_t SW_POWER_PIN = 10;
static const uint8_t TCA9546A_ADDR = 0x70;

// 既知アドレスに名前をつける
static const char* knownDevice(uint8_t addr) {
  switch (addr) {
    case 0x68: return "MPU6050/MPU9250 (加速度センサ)";
    case 0x69: return "MPU6050/MPU9250 (AD0=HIGH)";
    case 0x20: return "TCA9534 (メインバス側。ここに出たら配線確認)";
    default:   return "";
  }
}

// TCA9546A: チャンネル ch（0〜3）を有効にする
static bool tcaSelect(uint8_t ch) {
  Wire.beginTransmission(TCA9546A_ADDR);
  Wire.write(1 << ch);
  return Wire.endTransmission() == 0;
}

// TCA9546A: 全チャンネル切断
static void tcaDisable() {
  Wire.beginTransmission(TCA9546A_ADDR);
  Wire.write(0x00);
  Wire.endTransmission();
}

// I2Cスキャン（現在選択中のチャンネルをスキャン）
static int i2cScan() {
  int found = 0;
  for (uint8_t addr = 0x01; addr < 0x7F; addr++) {
    // TCA9546A 自身と TCA9534 はスキップ（メインバス側のデバイス）
    if (addr == TCA9546A_ADDR || addr == 0x20) continue;

    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("    0x"));
      if (addr < 0x10) Serial.print('0');
      Serial.print(addr, HEX);

      const char* name = knownDevice(addr);
      if (strlen(name) > 0) {
        Serial.print(F(": "));
        Serial.print(name);
      } else {
        Serial.print(F(": 未知のデバイス"));
      }
      Serial.println();
      found++;
    }
  }
  return found;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) yield();

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  Wire.begin();

  Serial.println(F("\n[STEP6] TCA9546A チャンネル切り替え確認"));
  Serial.println(F("各チャンネル下流のI2Cデバイスをスキャンします"));
  Serial.println(F("-------------------------------------------"));

  // TCA9546A 自体の応答確認
  Wire.beginTransmission(TCA9546A_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println(F("[TCA9546A] 応答なし → Step2で0x70が見えているか確認"));
    while (true) delay(1000);
  }
  Serial.println(F("[TCA9546A] 応答OK"));
  tcaDisable();
  delay(100);
}

static uint32_t cycleCount = 0;

void loop() {
  cycleCount++;
  Serial.print(F("\n=== スキャンサイクル #"));
  Serial.print(cycleCount);
  Serial.println(F(" ==="));

  for (uint8_t ch = 0; ch < 4; ch++) {
    Serial.print(F("[CH"));
    Serial.print(ch);
    Serial.println(F("] I2Cスキャン中..."));

    if (!tcaSelect(ch)) {
      Serial.println(F("  TCA9546A チャンネル選択失敗"));
      continue;
    }
    delay(10);

    int found = i2cScan();

    if (found == 0) {
      Serial.println(F("  検出: 0個（未接続 or デバイス電源オフ）"));
    } else {
      Serial.print(F("  検出: "));
      Serial.print(found);
      Serial.println(F("個 ✓"));
    }

    tcaDisable();
    delay(50);
  }

  Serial.println(F("--- 5秒後に再スキャン ---"));
  delay(5000);
}
