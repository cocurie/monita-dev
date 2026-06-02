/**
 * Monita Flex v3.02 — 検証 Step5: HX711 4ch（MUX切り替え）
 *
 * 確認内容:
 *   TCA9534でMUXを CH1→CH2→CH3→CH4 と順に切り替えながら
 *   各スロットのHX711値を読み取れるか確認する
 *
 * 配線前提:
 *   HX711 SCK  → XIAO D6
 *   HX711 DOUT → XIAO D7
 *   JP1〜JP4（使うスロット）にひずみゲージを接続
 *   接続していないスロットは TIMEOUT になることがある（正常）
 *
 * 期待出力例:
 *   === 計測サイクル ===
 *   [CH1] raw: 123456   OK
 *   [CH2] raw: 234567   OK
 *   [CH3] TIMEOUT（未接続）
 *   [CH4] TIMEOUT（未接続）
 *   ===================
 *
 * 注意:
 *   接続しているスロットだけ有効にしたい場合は
 *   CH_ACTIVE[] を false に変更する
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <HX711.h>

static const uint8_t SW_POWER_PIN   = 10;
static const uint8_t HX711_SCK_PIN  = 6;
static const uint8_t HX711_DOUT_PIN = 7;
static const uint8_t TCA9534_ADDR   = 0x20;

// 使用するチャネルをここで管理（接続していないスロットは false に）
static const bool CH_ACTIVE[4] = {true, true, true, true};

static const uint8_t REG_OUTPUT = 0x01;
static const uint8_t REG_CONFIG = 0x03;

HX711 hx;

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

static bool tca9534Init() {
  if (!tca9534WriteReg(REG_OUTPUT, 0x00)) return false;
  if (!tca9534WriteReg(REG_CONFIG, 0xFC)) return false;
  return true;
}

// ch: 1〜4 → TCA9534 P0(MUX_B), P1(MUX_A) を設定
static bool muxSelect(uint8_t ch) {
  uint8_t idx = (ch - 1) & 0x03;
  uint8_t a   = idx & 0x01;
  uint8_t b   = (idx >> 1) & 0x01;
  uint8_t twoBits = (uint8_t)(b | (a << 1));  // P0=B, P1=A

  uint8_t out = 0;
  if (!tca9534ReadReg(REG_OUTPUT, &out)) return false;
  out = (out & 0xFC) | (twoBits & 0x03);
  return tca9534WriteReg(REG_OUTPUT, out);
}

// 指定chのHX711を読んで値を返す。タイムアウトは INT32_MIN で示す
static long readHX711(uint8_t ch) {
  if (!muxSelect(ch)) return INT32_MIN;
  delay(20);  // MUX切り替え安定待ち

  hx.begin(HX711_DOUT_PIN, HX711_SCK_PIN);

  unsigned long t = millis();
  while (!hx.is_ready()) {
    if (millis() - t > 1000) return INT32_MIN;
    delay(10);
  }

  return hx.read();
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) yield();

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  Wire.begin();
  analogReadResolution(12);

  Serial.println(F("\n[STEP5] HX711 4ch 読み取りテスト"));
  Serial.println(F("-----------------------------------"));

  if (!tca9534Init()) {
    Serial.println(F("[TCA9534] init FAILED"));
    while (true) delay(1000);
  }
  Serial.println(F("[TCA9534] init OK"));
  Serial.println(F("3秒後に計測開始します\n"));
  delay(3000);
}

static uint32_t cycleCount = 0;

void loop() {
  cycleCount++;
  Serial.print(F("=== 計測サイクル #"));
  Serial.print(cycleCount);
  Serial.println(F(" ==="));

  for (uint8_t ch = 1; ch <= 4; ch++) {
    Serial.print(F("[CH"));
    Serial.print(ch);
    Serial.print(F("] "));

    if (!CH_ACTIVE[ch - 1]) {
      Serial.println(F("スキップ（CH_ACTIVE=false）"));
      continue;
    }

    long val = readHX711(ch);

    if (val == INT32_MIN) {
      Serial.println(F("TIMEOUT（未接続 or 配線不良）"));
    } else {
      Serial.print(F("raw: "));
      Serial.print(val);
      Serial.println(F("   OK"));
    }
  }

  Serial.println(F("===================\n"));
  delay(2000);
}
