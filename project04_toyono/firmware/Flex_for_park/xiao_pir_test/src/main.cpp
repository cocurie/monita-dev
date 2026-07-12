/**
 * PIR センサ動作検証
 * 基板  : Seeed XIAO nRF52840
 * センサ: スイッチサイエンス ミニPIRモーションセンサ（品番7028）
 *
 * 配線:
 *   PIR VCC → XIAO 3.3V
 *   PIR GND → XIAO GND
 *   PIR OUT → XIAO D2
 *
 * シリアルモニタ出力例:
 *   [3s] PIR: HIGH  検出数: 1
 *   [4s] PIR: HIGH  検出数: 1   ← デバウンス中はカウントしない
 *   [8s] PIR: LOW
 *   [12s] PIR: HIGH  検出数: 2
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

// ── 設定 ─────────────────────────────────────────
static uint8_t  const PIR_PIN        = D2;      // PIR OUT を接続するピン
static uint32_t const PRINT_INTERVAL = 1000;    // シリアル出力間隔 (ms)
static uint32_t const DEBOUNCE_MS    = 2000;    // 同一検出の無視時間 (ms)
// ─────────────────────────────────────────────────

static uint32_t pirCount    = 0;    // 検出イベント累計
static uint32_t lastDetect  = 0;    // 最後に検出した時刻
static uint32_t lastPrint   = 0;
static bool     prevState   = LOW;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  pinMode(PIR_PIN, INPUT);

  Serial.println(F("\n--- PIR センサ 動作検証 ---"));
  Serial.print(F("PIR ピン : D"));
  Serial.println(PIR_PIN);
  Serial.print(F("デバウンス: "));
  Serial.print(DEBOUNCE_MS);
  Serial.println(F(" ms"));
  Serial.println(F("センサが安定するまで約30秒待ってください"));
  Serial.println(F("----------------------------\n"));
}

void loop() {
  uint32_t now      = millis();
  bool     curState = digitalRead(PIR_PIN);

  // ── 検出カウント（立ち上がりエッジ + デバウンス）──
  if (curState == HIGH && prevState == LOW) {
    if (now - lastDetect >= DEBOUNCE_MS) {
      pirCount++;
      lastDetect = now;
    }
  }
  prevState = curState;

  // ── 1秒ごとにシリアル出力 ───────────────────────
  if (now - lastPrint >= PRINT_INTERVAL) {
    lastPrint = now;

    Serial.print('[');
    Serial.print(now / 1000UL);
    Serial.print(F("s] PIR: "));

    if (curState == HIGH) {
      Serial.print(F("HIGH  検出数: "));
      Serial.println(pirCount);
    } else {
      Serial.println(F("LOW"));
    }
  }
}
