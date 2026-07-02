/**
 * Monita Flex v3.03 — 検証 Step13: HX711 4ch ゲージファクター計測（手動操作モード）
 *
 * シリアルモニタからコマンドを入力してチャネル切替・1回計測を行う。
 * ひずみ値を変えながら1点ずつ記録していくゲージファクター確認用。
 *
 * コマンド一覧:
 *   1 / 2 / 3 / 4  → 計測チャネルを選択（例: "2" で CH2 に切替）
 *   m              → 現在のチャネルを1回計測
 *   a              → 全チャネルを1回計測
 *   z              → 現在のチャネルのゼロ点を設定（今の raw をゼロ基準にする）
 *   scale <値>     → STRAIN_SCALE を変更（例: "scale 1050.5"）
 *   ?              → コマンド一覧表示
 *
 * ゲージファクター算出手順:
 *   1. "z" でゼロ点を設定
 *   2. 既知ひずみ（例: 1000 µε）を与えて "m" で計測
 *   3. STRAIN_SCALE = Δraw ÷ 既知ひずみ[µε] → "scale <算出値>" で設定
 *   4. 再度計測して strain 値が既知ひずみと一致するか確認
 *
 * 配線（Flex v3.03 基板）:
 *   HX711 SCK: D6 / DOUT: D7（SN74LV4052 MUX 経由）
 *   TCA9534: I2C 0x20（MUX A/B 制御、P0=B, P1=A）
 *   SW_POWER: D10（3V3_SW）
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <HX711.h>

// ══════════════════════════════════════════════
// ▼ 初期設定（起動時のデフォルト値）
// ══════════════════════════════════════════════
static float   s_strainScale = 1110.0f;   // ゲージファクター（"scale <値>" で変更可）
static int32_t s_zeroRaw[4]  = {0,0,0,0}; // チャネルごとのゼロ点 raw 値（"z" で設定）
static uint8_t s_currentCh   = 1;          // 選択中チャネル（1〜4）
static int     s_measCount    = 0;          // 計測回数カウンタ

// HX711 ゲイン: 128（デフォルト）または 64
static const uint8_t HX711_GAIN  = 128;
// 中央値取得のサンプル数
static const int     SAMPLES     = 5;

// ══════════════════════════════════════════════
// ピン定義（Flex v3.03）
// ══════════════════════════════════════════════
static const uint8_t SW_POWER_PIN  = 10;
static const uint8_t HX711_SCK     = 6;
static const uint8_t HX711_DOUT    = 7;
static const uint8_t TCA9534_ADDR  = 0x20;

// ══════════════════════════════════════════════
// TCA9534（SN74LV4052 MUX A/B 制御）
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
  uint8_t bits = (uint8_t)(b | (a << 1));  // P0=B, P1=A
  uint8_t out  = 0;
  if (!tca9534Read(0x01, &out)) return false;
  return tca9534Write(0x01, (uint8_t)((out & 0xFC) | (bits & 0x03)));
}

// ══════════════════════════════════════════════
// HX711
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

static bool hxReadRaw(int32_t* out) {
  unsigned long t = millis();
  while (!hx.is_ready()) {
    if (millis() - t > 1500) return false;
  }
  float buf[5];
  int n = min(SAMPLES, 5);
  for (int i = 0; i < n; i++) buf[i] = hx.read();
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - i - 1; j++)
      if (buf[j] > buf[j+1]) { float tmp = buf[j]; buf[j] = buf[j+1]; buf[j+1] = tmp; }
  *out = (int32_t)buf[n / 2];
  return true;
}

// ══════════════════════════════════════════════
// 計測・表示
// ══════════════════════════════════════════════
static void measureCh(uint8_t ch) {
  s_measCount++;
  Serial.print(F("\n--- 計測 #")); Serial.print(s_measCount);
  Serial.print(F("  CH")); Serial.print(ch); Serial.println(F(" ---"));

  hxBegin(ch);
  int32_t raw = 0;
  if (!hxReadRaw(&raw)) {
    Serial.println(F("[TIMEOUT] HX711 応答なし（未接続？）"));
    return;
  }

  int32_t zero   = s_zeroRaw[ch - 1];
  int32_t dRaw   = raw - zero;
  int32_t strain = (int32_t)((float)dRaw / s_strainScale);

  char buf[14];
  Serial.print(F("  raw    : ")); snprintf(buf, sizeof(buf), "%11ld", (long)raw);   Serial.println(buf);
  Serial.print(F("  zero   : ")); snprintf(buf, sizeof(buf), "%11ld", (long)zero);  Serial.println(buf);
  Serial.print(F("  Δraw   : ")); snprintf(buf, sizeof(buf), "%11ld", (long)dRaw);  Serial.println(buf);
  Serial.print(F("  strain : ")); snprintf(buf, sizeof(buf), "%11ld", (long)strain); Serial.print(buf);
  Serial.println(F(" µε"));
  Serial.print(F("  scale  : ")); Serial.println(s_strainScale, 2);
}

static void measureAll() {
  for (uint8_t ch = 1; ch <= 4; ch++) measureCh(ch);
}

static void setZero(uint8_t ch) {
  hxBegin(ch);
  int32_t raw = 0;
  if (!hxReadRaw(&raw)) {
    Serial.println(F("[TIMEOUT] HX711 応答なし"));
    return;
  }
  s_zeroRaw[ch - 1] = raw;
  Serial.print(F("[ZERO] CH")); Serial.print(ch);
  Serial.print(F(" ゼロ点を設定しました → zero_raw = ")); Serial.println(raw);
}

static void printHelp() {
  Serial.println(F("\n┌─ コマンド一覧 ──────────────────────┐"));
  Serial.println(F("│ 1 / 2 / 3 / 4  チャネル選択          │"));
  Serial.println(F("│ m              現在CHを1回計測        │"));
  Serial.println(F("│ a              全CH（1〜4）を1回計測  │"));
  Serial.println(F("│ z              現在CHのゼロ点を設定   │"));
  Serial.println(F("│ scale <値>     ゲージファクターを変更 │"));
  Serial.println(F("│ ?              このヘルプを表示       │"));
  Serial.println(F("└────────────────────────────────────┘"));
  Serial.print(F("現在: CH")); Serial.print(s_currentCh);
  Serial.print(F("  scale=")); Serial.print(s_strainScale, 2);
  Serial.print(F("  zero="));  Serial.println(s_zeroRaw[s_currentCh - 1]);
}

// ══════════════════════════════════════════════
// シリアル入力処理
// ══════════════════════════════════════════════
static void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  // チャネル選択: "1" "2" "3" "4"
  if (cmd.length() == 1 && cmd[0] >= '1' && cmd[0] <= '4') {
    s_currentCh = cmd[0] - '0';
    Serial.print(F("[CH] CH")); Serial.print(s_currentCh); Serial.println(F(" を選択しました"));
    Serial.print(F("  zero_raw = ")); Serial.println(s_zeroRaw[s_currentCh - 1]);
    return;
  }

  // m: 現在CHを1回計測
  if (cmd == "m") {
    measureCh(s_currentCh);
    return;
  }

  // a: 全CH計測
  if (cmd == "a") {
    measureAll();
    return;
  }

  // z: ゼロ点設定
  if (cmd == "z") {
    Serial.print(F("[ZERO] CH")); Serial.print(s_currentCh); Serial.println(F(" のゼロ点を計測中..."));
    setZero(s_currentCh);
    return;
  }

  // scale <値>: ゲージファクター変更
  if (cmd.startsWith("scale ")) {
    float val = cmd.substring(6).toFloat();
    if (val > 0.0f) {
      s_strainScale = val;
      Serial.print(F("[SCALE] STRAIN_SCALE = ")); Serial.println(s_strainScale, 2);
    } else {
      Serial.println(F("[SCALE] 値が不正です（正の数を入力してください）"));
    }
    return;
  }

  // ?: ヘルプ
  if (cmd == "?") {
    printHelp();
    return;
  }

  Serial.print(F("[?] 不明なコマンド: ")); Serial.println(cmd);
  Serial.println(F("  \"?\" でコマンド一覧を表示"));
}

// ══════════════════════════════════════════════
// setup / loop
// ══════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  Wire.begin();

  if (!tca9534Configure()) {
    Serial.println(F("[ERROR] TCA9534 init failed — 配線を確認してください"));
    while (true) delay(1000);
  }

  Serial.println(F("\n============================================="));
  Serial.println(F("  Step13: HX711 ゲージファクター計測"));
  Serial.println(F("  Monita Flex v3.03"));
  Serial.println(F("============================================="));
  Serial.print(F("初期 STRAIN_SCALE = ")); Serial.println(s_strainScale, 2);
  Serial.println(F("TCA9534 OK"));
  printHelp();
  Serial.println(F("\nコマンドを入力してください >"));
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    handleCommand(cmd);
    Serial.println(F("\n> "));
  }
}
