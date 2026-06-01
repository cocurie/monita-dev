/**
 * Monita Flex v3.02 — 検証 Step1: 電源・LED・3V3_SW
 *
 * 確認内容:
 *   1. シリアル通信が通っているか
 *   2. RGB LEDが3色光るか
 *   3. D10（SW_POWER_PIN）でスイッチングレールをON/OFFできるか
 *
 * 期待出力:
 *   [STEP1] 電源・LED・3V3_SW 確認
 *   [LED] RED
 *   [LED] GREEN
 *   [LED] BLUE
 *   [LED] OFF
 *   [3V3_SW] HIGH (ON)
 *   [3V3_SW] LOW  (OFF)
 *   --- ループ繰り返し ---
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

// D10 = MOSFET_GATE → 3V3_SW ON/OFF
static const uint8_t SW_POWER_PIN = 10;

// LEDは内部でアクティブLow → analogWrite(255)=最大輝度, 0=消灯
static void ledRed()   { analogWrite(LED_RED, 255); analogWrite(LED_GREEN, 0);   analogWrite(LED_BLUE, 0);   }
static void ledGreen() { analogWrite(LED_RED, 0);   analogWrite(LED_GREEN, 255); analogWrite(LED_BLUE, 0);   }
static void ledBlue()  { analogWrite(LED_RED, 0);   analogWrite(LED_GREEN, 0);   analogWrite(LED_BLUE, 255); }
static void ledOff()   { analogWrite(LED_RED, 0);   analogWrite(LED_GREEN, 0);   analogWrite(LED_BLUE, 0);   }

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) yield();

  pinMode(LED_RED,   OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE,  OUTPUT);
  pinMode(SW_POWER_PIN, OUTPUT);

  Serial.println(F("\n[STEP1] 電源・LED・3V3_SW 確認"));
  Serial.println(F("LEDが RED → GREEN → BLUE → OFF の順に光れば正常"));
  Serial.println(F("3V3_SWはシリアルログで確認（テスターがあればD10電圧も確認）"));
}

void loop() {
  // ── LED確認 ───────────────────────────────────────
  Serial.println(F("[LED] RED"));
  ledRed();
  delay(1000);

  Serial.println(F("[LED] GREEN"));
  ledGreen();
  delay(1000);

  Serial.println(F("[LED] BLUE"));
  ledBlue();
  delay(1000);

  Serial.println(F("[LED] OFF"));
  ledOff();
  delay(500);

  // ── 3V3_SW 確認 ────────────────────────────────────
  Serial.println(F("[3V3_SW] HIGH (ON)  ← D10をテスターで計測するなら今"));
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(2000);

  Serial.println(F("[3V3_SW] LOW  (OFF) ← センサ側レールが落ちるはず"));
  digitalWrite(SW_POWER_PIN, LOW);
  delay(2000);

  Serial.println(F("--- ループ ---\n"));
}
