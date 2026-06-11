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
#define TEST_MODE 1

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
static const uint8_t ONE_WIRE_PIN  = D6;   // D7 = DOUT ライン（4052経由）

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

// ★ 物理 JP コネクタ → TCA9534 出力値 マッピング
// 診断ログより: 物理JP1 = 旧コードch=2(0x02) が確定
// JP2=0x00, JP3=0x01, JP4=0x03 は推定（未接続時は LOW なので要確認）
static const uint8_t JP_TO_MUX[4] = {0x02, 0x00, 0x01, 0x03};

static bool muxSelect(uint8_t ch) {
  if (ch < 1 || ch > 4) return false;
  uint8_t val = JP_TO_MUX[ch - 1];

  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(0x01);
  Wire.write(val);
  if (Wire.endTransmission() != 0) return false;

  return true;
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
    delay(200);  // ★ MUX 安定待ちを延長（50ms → 200ms）

    // ピンの状態確認（プルアップ＆信号経路の診断）
    pinMode(ONE_WIRE_PIN, INPUT);
    int pinHigh = digitalRead(ONE_WIRE_PIN);
    Serial.printf("  [DIAG] pin%d idle: %s\n",
      ONE_WIRE_PIN, pinHigh ? "HIGH ✓" : "LOW ✗ (経路NG)");

    // ★ raw reset で存在確認してから sensors.begin() を呼ぶ
    oneWire.reset_search();
    bool presence = oneWire.reset();
    Serial.print(F("reset="));
    Serial.print(presence ? F("OK") : F("NG"));

    uint8_t count = 0;
    if (presence) {
      delay(10);

      // ★ SKIP ROM（1チャンネル1センサ専用）
      // ROM サーチを使わないため 4052 経由でも安定動作
      oneWire.write(0xCC);  // SKIP ROM
      oneWire.write(0x44);  // Convert T（温度変換開始）
      delay(750);           // 12bit 変換待ち（最大 750ms）

      bool resetOk2 = oneWire.reset();
      if (resetOk2) {
        oneWire.write(0xCC);  // SKIP ROM
        oneWire.write(0xBE);  // Read Scratchpad

        uint8_t lo = oneWire.read();
        uint8_t hi = oneWire.read();
        int16_t raw = (int16_t)(lo | (hi << 8));
        float t = raw / 16.0f;

        if (t > -55.0f && t < 125.0f) {
          Serial.printf("  temp=%.2f℃ ✓", t);
          count = 1;
        } else {
          Serial.printf("  temp=%.2f (範囲外 → 配線確認)", t);
        }
      } else {
        Serial.print(F("  2回目reset失敗"));
      }
    }
    Serial.print(F(" count="));
    Serial.println(count);
    delay(50);
  }
  delay(3000);
}
#endif
