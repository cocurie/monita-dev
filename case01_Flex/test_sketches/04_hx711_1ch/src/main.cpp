/**
 * Monita Flex v3.02 — 検証 Step4: HX711 1ch読み取り（MUX固定・CH1）
 *
 * 確認内容:
 *   TCA9534でMUXをCH1（A=0,B=0）に固定したまま
 *   HX711からデータが読み取れるか確認する
 *
 * 配線前提:
 *   HX711 SCK  → XIAO D6
 *   HX711 DOUT → XIAO D7
 *   JP1（CH1スロット）にひずみゲージを接続
 *
 * 期待出力例:
 *   [HX711 CH1] raw: 123456
 *   [HX711 CH1] raw: 123480
 *   ※ ゲージに荷重をかけると値が変化すればOK
 *   ※ センサ未接続でも何らかの数値が出ればHX711は動作中
 *
 * トラブル:
 *   [HX711] TIMEOUT → D6/D7配線、HX711への電源を確認
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <HX711.h>

static const uint8_t SW_POWER_PIN  = 10;
static const uint8_t HX711_SCK_PIN  = 6;
static const uint8_t HX711_DOUT_PIN = 7;
static const uint8_t TCA9534_ADDR   = 0x20;

static const uint8_t REG_OUTPUT = 0x01;
static const uint8_t REG_CONFIG = 0x03;

HX711 hx;

static bool tca9534WriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// CH1に固定（A=0, B=0 → P0=0, P1=0）
static bool muxFixCH1() {
  if (!tca9534WriteReg(REG_OUTPUT, 0x00)) return false;
  if (!tca9534WriteReg(REG_CONFIG, 0xFC)) return false;
  return true;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) yield();

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  Wire.begin();
  analogReadResolution(12);

  Serial.println(F("\n[STEP4] HX711 CH1 読み取りテスト"));
  Serial.println(F("MUX: CH1固定（TCA9534 P0=0,P1=0）"));
  Serial.println(F("-----------------------------------"));

  if (!muxFixCH1()) {
    Serial.println(F("[TCA9534] init FAILED → Step3を先に完了させること"));
    while (true) delay(1000);
  }
  Serial.println(F("[TCA9534] MUX → CH1 固定 OK"));

  delay(100);

  // HX711 power-down復帰: SCKをLOWに確定させてから初期化
  pinMode(HX711_SCK_PIN, OUTPUT);
  digitalWrite(HX711_SCK_PIN, LOW);
  delay(100);

  hx.begin(HX711_DOUT_PIN, HX711_SCK_PIN);

  // HX711の起動待ち（最大5秒）
  Serial.print(F("[HX711] 起動待ち..."));
  unsigned long t = millis();
  while (!hx.is_ready()) {
    if (millis() - t > 5000) {
      Serial.println(F(" TIMEOUT"));
      Serial.println(F("→ JP1配線(E+/E-/A+/A-)、HX711電源(3V3_SW)を確認"));
      while (true) delay(1000);
    }
    delay(100);
  }
  Serial.println(F(" OK"));

  // tare（ゼロ点補正）
  Serial.println(F("[HX711] tare実行中..."));
  hx.tare(10);
  Serial.println(F("[HX711] tare完了。ここを基準(0)とする"));
  Serial.println(F("荷重をかけると値が変化します。1秒ごとに出力します。\n"));
}

void loop() {
  if (!hx.is_ready()) {
    Serial.println(F("[HX711] not ready..."));
    delay(1000);
    return;
  }

  long raw = hx.read();

  Serial.print(F("[HX711 CH1] raw: "));
  Serial.println(raw);

  delay(1000);
}
