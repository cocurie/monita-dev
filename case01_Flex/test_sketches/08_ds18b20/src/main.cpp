/**
 * Monita Flex v3.02 — 検証 Step8: DS18B20 診断スケッチ
 *
 * 切り分けモード（TEST_MODE で切り替え）:
 *
 *   TEST_MODE 0: D1 直結テスト（4052 MUX を使わない）← まずこれで確認
 *     配線: DS18B20 DQ → D1 + 4.7kΩプルアップ(D1〜3V3間)
 *           DS18B20 VDD → 3V3, GND → GND
 *     目的: センサ＋ライブラリが nRF52840 で動くか確認する
 *
 *   TEST_MODE 1: 4052 MUX + TCA9534 経由テスト（本来の基板経路）
 *     配線: DS18B20 DQ → JP の DOUT ピン + 4.7kΩプルアップ(DQ〜3V3SW間)
 *           DS18B20 VDD → JP の 3V3SW, GND → GND
 *     目的: 基板上の実際の経路で動くか確認する
 *
 * 手順:
 *   1. TEST_MODE 0 で D1 直結 → 温度が出ればライブラリはOK
 *   2. TEST_MODE 1 で MUX 経由 → 出れば基板経路もOK
 *   3. MODE 0 はOKで MODE 1 がNGなら → 4052経由の問題
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ============================================================
// ★ここを切り替えて使う★
// 0 = D1直結テスト（まずこれ）
// 1 = 4052 MUX + TCA9534 経由テスト
// ============================================================
#define TEST_MODE 0

static const uint8_t SW_POWER_PIN  = 10;

#if (TEST_MODE == 0)
// ── D1 直結テスト ──────────────────────────────────────────
static const uint8_t ONE_WIRE_PIN = 1;   // D1 に直接接続
OneWire           oneWire(ONE_WIRE_PIN);
DallasTemperature sensors(&oneWire);

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) yield();

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  Serial.println(F("\n[STEP8 MODE0] DS18B20 D1直結テスト（4052バイパス）"));
  Serial.println(F("配線: DQ→D1, 4.7kΩ(D1〜3V3), VDD→3V3, GND→GND"));
  Serial.println(F("---------------------------------------------------"));

  sensors.begin();

  uint8_t count = sensors.getDeviceCount();
  Serial.print(F("検出センサ数: "));
  Serial.println(count);

  if (count == 0) {
    Serial.println(F("→ 応答なし。配線・プルアップ抵抗を確認"));
  } else {
    Serial.println(F("→ センサ検出OK！ライブラリは動作しています"));
    DeviceAddress addr;
    for (uint8_t i = 0; i < count; i++) {
      if (sensors.getAddress(addr, i)) {
        Serial.print(F("  アドレス["));
        Serial.print(i);
        Serial.print(F("]: "));
        for (uint8_t b = 0; b < 8; b++) {
          if (addr[b] < 0x10) Serial.print('0');
          Serial.print(addr[b], HEX);
          if (b < 7) Serial.print(' ');
        }
        Serial.println();
      }
    }
  }
  Serial.println();
}

void loop() {
  uint8_t count = sensors.getDeviceCount();
  if (count == 0) {
    Serial.println(F("[DS18B20] 応答なし"));
    delay(2000);
    return;
  }
  sensors.requestTemperatures();
  float t = sensors.getTempCByIndex(0);
  Serial.print(F("[DS18B20] "));
  Serial.print(t, 2);
  Serial.println(F(" ℃"));
  delay(2000);
}

#else
// ── TEST_MODE 1: 4052 MUX + TCA9534 経由 ──────────────────
static const uint8_t TCA9534_ADDR  = 0x20;
static const uint8_t ONE_WIRE_PIN  = 7;   // D7 = DOUT ライン（4052経由）

OneWire           oneWire(ONE_WIRE_PIN);
DallasTemperature sensors(&oneWire);

static bool tca9534Init() {
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(0x03);  // Config reg
  Wire.write(0xFC);  // P0,P1 = 出力
  if (Wire.endTransmission() != 0) return false;
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(0x01);  // Output reg
  Wire.write(0x00);
  return Wire.endTransmission() == 0;
}

static bool muxSelect(uint8_t ch) {
  uint8_t idx = (ch - 1) & 0x03;
  uint8_t a = idx & 0x01;
  uint8_t b = (idx >> 1) & 0x01;
  uint8_t val = (uint8_t)(b | (a << 1));  // P0=B, P1=A
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(0x01);
  Wire.write(val & 0x03);
  return Wire.endTransmission() == 0;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) yield();

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  Wire.begin();

  Serial.println(F("\n[STEP8 MODE1] DS18B20 4052 MUX 経由テスト"));
  Serial.println(F("配線: JP の DOUT ピン → DQ + 4.7kΩ(DQ〜3V3SW)"));
  Serial.println(F("------------------------------------------"));

  if (!tca9534Init()) {
    Serial.println(F("[TCA9534] 応答なし"));
    while (true) delay(1000);
  }
  Serial.println(F("[TCA9534] OK"));
}

static uint32_t cycleCount = 0;

void loop() {
  cycleCount++;
  Serial.print(F("\n=== サイクル #"));
  Serial.print(cycleCount);
  Serial.println(F(" ==="));

  for (uint8_t ch = 1; ch <= 4; ch++) {
    Serial.print(F("[JP"));
    Serial.print(ch);
    Serial.print(F("] "));

    if (!muxSelect(ch)) {
      Serial.println(F("MUX失敗"));
      continue;
    }
    delay(50);  // MUX 安定待ち（長めに）

    // 1-Wire バス再初期化
    oneWire.reset_search();
    sensors.begin();
    delay(10);

    // raw reset で応答を直接確認
    bool presence = oneWire.reset();
    Serial.print(F("reset="));
    Serial.print(presence ? F("OK") : F("NG"));

    uint8_t count = sensors.getDeviceCount();
    Serial.print(F(" count="));
    Serial.print(count);

    if (count > 0) {
      sensors.requestTemperatures();
      float t = sensors.getTempCByIndex(0);
      Serial.print(F(" temp="));
      Serial.print(t, 2);
      Serial.print(F("℃ ✓"));
    }
    Serial.println();
    delay(50);
  }
  delay(3000);
}
#endif
