/**
 * Monita Flex v3.03 — 検証 Step13: HX711 4ch ひずみ確認 / ゲージファクター計測
 *
 * 目的:
 *   HX711 の raw 値をそのままシリアルに出して、
 *   ひずみ発生装置との対応を確認する。補正・スリープなし。
 *
 * 使い方:
 *   1. ひずみゼロの状態で起動し、raw 値を記録する（ZERO_RAW に設定）
 *   2. ひずみを与えて raw 変化量を確認する
 *   3. STRAIN_SCALE = (Δraw) / (既知ひずみ[µε]) で係数を求める
 *
 * 出力例:
 *   === 計測 #1 ===
 *   [CH1] raw:   -5046342  strain:       0
 *
 * 配線（Flex v3.03 基板）:
 *   HX711 SCK: D6 / DOUT: D7（SN74LV4052 MUX 経由）
 *   TCA9534: I2C 0x20（MUX A/B 制御、P0=B, P1=A）
 *   SW_POWER: D10（3V3_SW）
 *   ※ v3.02 と同じピン配置
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <HX711.h>

// ══════════════════════════════════════════════
// ▼ 設定（ここを変更する）
// ══════════════════════════════════════════════
static const float    STRAIN_SCALE = 1110.0f;    // ひずみ補正係数（gain=64 実測値）
static const int32_t  ZERO_RAW     = 0;  // ゼロ点 raw 値（2026-06-03 実測）
static const uint32_t INTERVAL_MS  = 1000;      // 計測間隔（ms）
static const int      SAMPLES      = 5;         // 中央値用サンプル数
// HX711 ゲイン: 128（デフォルト）または 64（ゼロオフセットが大きい場合）
static const uint8_t  HX711_GAIN   = 128;

// 使用するCHを true/false で切り替え（未接続は false にするとスキップ）
static const bool CH_ACTIVE[4] = {true, true, true, true};

// ══════════════════════════════════════════════
// ピン定義（Flex v3.02）
// ══════════════════════════════════════════════
static const uint8_t SW_POWER_PIN  = 10;
static const uint8_t HX711_SCK    = 6;
static const uint8_t HX711_DOUT   = 7;
static const uint8_t TCA9534_ADDR = 0x20;

// ══════════════════════════════════════════════
// TCA9534（SN74LV4052 MUX A/B 制御、I2C 0x20）
// ══════════════════════════════════════════════
static bool tca9534Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool tca9534Read(uint8_t reg, uint8_t* out) {
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TCA9534_ADDR, (uint8_t)1) != 1) return false;
  *out = Wire.read();
  return true;
}

static bool tca9534Configure() {
  if (!tca9534Write(0x02, 0x00)) return false;
  if (!tca9534Write(0x03, 0xFC)) return false;
  uint8_t out = 0;
  if (!tca9534Read(0x01, &out)) return false;
  return tca9534Write(0x01, out & 0xFC);
}

static bool muxSelect(uint8_t ch) {
  uint8_t idx  = (ch - 1) & 0x03;
  uint8_t a    = idx & 0x01;
  uint8_t b    = (idx >> 1) & 0x01;
  uint8_t bits = (uint8_t)(b | (a << 1));
  uint8_t out  = 0;
  if (!tca9534Read(0x01, &out)) return false;
  return tca9534Write(0x01, (uint8_t)((out & 0xFC) | (bits & 0x03)));
}

// ══════════════════════════════════════════════
// HX711（1インスタンスを MUX 切替で共用）
// ══════════════════════════════════════════════
static HX711 hx;

static void hxBegin(uint8_t ch) {
  muxSelect(ch);
  delay(10);
  pinMode(HX711_SCK, OUTPUT);
  digitalWrite(HX711_SCK, LOW);
  delay(10);
  hx.begin(HX711_DOUT, HX711_SCK);
  hx.set_gain(HX711_GAIN);
}

static int32_t hxRead(bool* ok) {
  unsigned long t = millis();
  while (!hx.is_ready()) {
    if (millis() - t > 1500) { *ok = false; return 0; }
  }
  float buf[5];
  int n = min(SAMPLES, 5);
  for (int i = 0; i < n; i++) buf[i] = hx.read();
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - i - 1; j++)
      if (buf[j] > buf[j+1]) { float tmp = buf[j]; buf[j] = buf[j+1]; buf[j+1] = tmp; }
  *ok = true;
  return (int32_t)buf[n / 2];
}

// ══════════════════════════════════════════════
// Arduino エントリ
// ══════════════════════════════════════════════
static uint32_t s_cycle = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  Wire.begin();

  Serial.println(F("\n[STEP13] HX711 4ch ゲージファクター計測 (Monita Flex v3.03)"));
  Serial.print(F("STRAIN_SCALE = ")); Serial.println(STRAIN_SCALE);
  Serial.print(F("ZERO_RAW     = ")); Serial.println(ZERO_RAW);
  Serial.print(F("HX711_GAIN   = ")); Serial.println(HX711_GAIN);
  Serial.print(F("SAMPLES      = ")); Serial.println(SAMPLES);
  Serial.println(F("──────────────────────────────────"));
  Serial.println(F("※ ひずみゼロ時の raw 値を確認し ZERO_RAW に設定してください"));
  Serial.println(F("──────────────────────────────────"));

  if (!tca9534Configure()) {
    Serial.println(F("[ERROR] TCA9534 init failed"));
    while (true) delay(1000);
  }
  Serial.println(F("[TCA9534] OK\n"));
}

void loop() {
  s_cycle++;
  Serial.print(F("=== 計測 #")); Serial.print(s_cycle); Serial.println(F(" ==="));

  for (uint8_t i = 0; i < 4; i++) {
    Serial.print(F("[CH")); Serial.print(i + 1); Serial.print(F("] "));

    if (!CH_ACTIVE[i]) {
      Serial.println(F("skip"));
      continue;
    }

    hxBegin(i + 1);
    bool ok = false;
    int32_t raw = hxRead(&ok);

    if (!ok) {
      Serial.println(F("raw: ---  TIMEOUT（未接続？）"));
      continue;
    }

    int32_t strain = (int32_t)((float)(raw - ZERO_RAW) / STRAIN_SCALE);

    char buf[32];
    Serial.print(F("raw: "));
    snprintf(buf, sizeof(buf), "%10ld", (long)raw);
    Serial.print(buf);
    Serial.print(F("  strain: "));
    snprintf(buf, sizeof(buf), "%8ld", (long)strain);
    Serial.print(buf);
    Serial.print(F("  (zero=")); Serial.print(ZERO_RAW);
    Serial.print(F(", scale=")); Serial.print(STRAIN_SCALE, 1);
    Serial.println(F(")"));
  }

  Serial.println();
  delay(INTERVAL_MS);
}
