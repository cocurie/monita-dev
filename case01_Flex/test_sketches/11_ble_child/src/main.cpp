/**
 * Monita Flex v3.02 — 検証 Step11: BLE子機（アドバタイズ）
 *
 * 動作:
 *   10分間隔・30秒アドバタイズ → 約570秒スリープ → 繰り返し
 *   （検証用: スリープは delay() ベース。本番は RTC2 deepSleep に差し替え）
 *
 * Manufacturer Data フォーマット（16バイト）:
 *   [0-1]  Company ID  0xFF 0xFF
 *   [2]    Pkt type    0x01（Monita Flex BLE）
 *   [3]    Device ID   0x01 = "test01"
 *   [4-5]  CH1 ひずみ  int16_t LE（生値 / 100）
 *   [6-7]  CH2 ひずみ  int16_t LE
 *   [8-9]  CH3 ひずみ  int16_t LE
 *   [10-11] CH4 ひずみ int16_t LE
 *   [12-13] バッテリー uint16_t LE（mV）
 *   [14-15] 次回計測まで uint16_t LE（秒）
 *
 * LED:
 *   Green 点灯   : アドバタイズ中
 *   Blue 点灯    : スリープ中（待機）
 *   Red 点灯     : エラー（TCA9534 / HX711）
 *
 * 配線（Flex v3.02 基板上）:
 *   HX711 SCK: D6 / DOUT: D7（SN74LV4052 MUX 経由）
 *   TCA9534: I2C 0x20（MUX A/B 制御）
 *   SW_POWER: D10（3V3_SW）
 *   BATT: A3（分圧後電池電圧）
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <HX711.h>
#include <bluefruit.h>
#include <nrf.h>

// ══════════════════════════════════════════════
// ▼ 設定（ここを変更する）
// ══════════════════════════════════════════════
static const uint8_t  DEVICE_ID       = 0x01;   // "test01"
static const uint16_t ADV_SECONDS     = 30;      // アドバタイズ時間（秒）
static const uint16_t SLEEP_SECONDS   = 150;     // スリープ時間（秒）。ADV + SLEEP ≈ 3分
static const uint16_t NEXT_WAKE_SEC   = 120;     // 親機へ通知する「最後パケットからスキャン開始までの秒数」
                                                  // = SLEEP_SECONDS(150) - スキャン前マージン(30)

// ══════════════════════════════════════════════
// ピン定義（Flex v3.02）
// ══════════════════════════════════════════════
static const uint8_t SW_POWER_PIN  = 10;
static const uint8_t HX711_SCK    = 6;
static const uint8_t HX711_DOUT   = 7;
static const uint8_t BATT_PIN     = A3;

// ══════════════════════════════════════════════
// TCA9534（SN74LV4052 MUX A/B 制御、I2C 0x20）
// ══════════════════════════════════════════════
static const uint8_t TCA9534_ADDR = 0x20;

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
  if (!tca9534Write(0x02, 0x00)) return false;   // Polarity: none
  if (!tca9534Write(0x03, 0xFC)) return false;   // P0,P1=出力、P2-P7=入力
  uint8_t out = 0;
  if (!tca9534Read(0x01, &out)) return false;
  return tca9534Write(0x01, out & 0xFC);          // A=0,B=0 で初期化
}

// ch: 1〜4 → SN74LV4052 の A/B ビットに写像
static bool muxSelect(uint8_t ch) {
  uint8_t idx = (ch - 1) & 0x03;
  uint8_t a   = idx & 0x01;
  uint8_t b   = (idx >> 1) & 0x01;
  uint8_t bits = (uint8_t)(b | (a << 1));  // P0=B, P1=A
  uint8_t out = 0;
  if (!tca9534Read(0x01, &out)) return false;
  return tca9534Write(0x01, (uint8_t)((out & 0xFC) | (bits & 0x03)));
}

// ══════════════════════════════════════════════
// HX711（1インスタンスを MUX 切替で共用）
// ══════════════════════════════════════════════
static HX711 hx;

// チャネル選択 → begin。SCK を LOW に確定してから begin する
static void hxBegin(uint8_t ch) {
  muxSelect(ch);
  delay(10);
  pinMode(HX711_SCK, OUTPUT);
  digitalWrite(HX711_SCK, LOW);
  delay(10);
  hx.begin(HX711_DOUT, HX711_SCK);
}

// HX711 から1回読み取る。タイムアウト 1500ms で 0 を返す
static int32_t hxRead() {
  unsigned long t = millis();
  while (!hx.is_ready()) {
    if (millis() - t > 1500) return 0;
  }
  return hx.read();
}

// ══════════════════════════════════════════════
// バッテリー電圧（A3 分圧後 → 実効 mV）
// ══════════════════════════════════════════════
static uint16_t readBattMv() {
  int raw = analogRead(BATT_PIN);
  float v = raw * (3.3f / 4095.0f);
  v *= 1.8696f;  // 分圧比補正（v3.02 回路図に準拠）
  return (uint16_t)(v * 1000.0f);
}

// ══════════════════════════════════════════════
// LED ヘルパー（アクティブ LOW）
// ══════════════════════════════════════════════
static void ledInit() {
  pinMode(LED_RED,   OUTPUT); digitalWrite(LED_RED,   HIGH);
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, HIGH);
  pinMode(LED_BLUE,  OUTPUT); digitalWrite(LED_BLUE,  HIGH);
}
static void ledOff()              { digitalWrite(LED_RED,HIGH); digitalWrite(LED_GREEN,HIGH); digitalWrite(LED_BLUE,HIGH); }
static void pinOn(uint8_t pin)    { ledOff(); digitalWrite(pin, LOW); }
static void ledBlink(uint8_t pin, int n) {
  for (int i = 0; i < n; i++) { digitalWrite(pin,LOW); delay(150); digitalWrite(pin,HIGH); if(i<n-1) delay(150); }
}

// ══════════════════════════════════════════════
// Manufacturer Data 組み立て & BLE アドバタイズ
// ══════════════════════════════════════════════
static const uint8_t MFR_COMPANY_ID[2] = {0xFF, 0xFF};
static const uint8_t PKT_TYPE = 0x01;

// BLE 初期化（一度だけ呼ぶ）
static void bleInit() {
  Bluefruit.begin();
  Bluefruit.setName("Monita-test01");
}

static void advertise(int16_t ch1, int16_t ch2, int16_t ch3, int16_t ch4,
                      uint16_t battMv, uint16_t nextWakeSec) {
  uint8_t buf[16];
  buf[0]  = MFR_COMPANY_ID[0];
  buf[1]  = MFR_COMPANY_ID[1];
  buf[2]  = PKT_TYPE;
  buf[3]  = DEVICE_ID;
  // int16_t LE
  buf[4]  = (uint8_t)(ch1 & 0xFF);       buf[5]  = (uint8_t)((ch1 >> 8) & 0xFF);
  buf[6]  = (uint8_t)(ch2 & 0xFF);       buf[7]  = (uint8_t)((ch2 >> 8) & 0xFF);
  buf[8]  = (uint8_t)(ch3 & 0xFF);       buf[9]  = (uint8_t)((ch3 >> 8) & 0xFF);
  buf[10] = (uint8_t)(ch4 & 0xFF);       buf[11] = (uint8_t)((ch4 >> 8) & 0xFF);
  buf[12] = (uint8_t)(battMv & 0xFF);    buf[13] = (uint8_t)((battMv >> 8) & 0xFF);
  buf[14] = (uint8_t)(nextWakeSec & 0xFF); buf[15] = (uint8_t)((nextWakeSec >> 8) & 0xFF);

  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addManufacturerData(buf, sizeof(buf));
  Bluefruit.ScanResponse.addName();

  // interval: 1000ms = 1600 × 0.625ms
  Bluefruit.Advertising.setInterval(1600, 1600);
  Bluefruit.Advertising.setFastTimeout(0);
  Bluefruit.Advertising.start(0);  // 0 = 永続（手動で stop する）
}

// ══════════════════════════════════════════════
// Arduino エントリ
// ══════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  ledInit();
  ledBlink(LED_BLUE, 2);  // 起動確認

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  Wire.begin();

  Serial.println(F("\n[STEP11] BLE子機起動 (Monita Flex v3.02)"));
  Serial.print(F("Device ID : 0x0")); Serial.println(DEVICE_ID, HEX);
  Serial.print(F("Adv time  : ")); Serial.print(ADV_SECONDS); Serial.println(F(" sec"));
  Serial.print(F("Sleep time: ")); Serial.print(SLEEP_SECONDS); Serial.println(F(" sec"));

  if (!tca9534Configure()) {
    Serial.println(F("[ERROR] TCA9534 init failed"));
    while (true) { ledBlink(LED_RED, 2); delay(1000); }
  }
  Serial.println(F("[TCA9534] OK"));

  bleInit();
  Serial.println(F("[BLE] init OK"));
}

void loop() {
  Serial.println(F("\n=== 計測開始 ==="));
  ledBlink(LED_BLUE, 1);

  // HX711 4CH 読み取り（生値 / 100 で int16_t に収める）
  int16_t ch[4] = {0, 0, 0, 0};
  for (uint8_t i = 0; i < 4; i++) {
    hxBegin(i + 1);
    int32_t raw = hxRead();
    ch[i] = (int16_t)(raw / 100L);
    Serial.print(F("  CH")); Serial.print(i + 1);
    Serial.print(F(": raw=")); Serial.print(raw);
    Serial.print(F("  payload=")); Serial.println(ch[i]);
  }

  uint16_t batt = readBattMv();
  Serial.print(F("  BATT: ")); Serial.print(batt); Serial.println(F(" mV"));

  // アドバタイズ開始
  Serial.println(F("\n[BLE] アドバタイズ開始"));
  advertise(ch[0], ch[1], ch[2], ch[3], batt, NEXT_WAKE_SEC);
  pinOn(LED_GREEN);  // Green = アドバタイズ中

  // ADV_SECONDS 間アドバタイズを継続
  uint32_t advStart = millis();
  while (millis() - advStart < (uint32_t)ADV_SECONDS * 1000UL) {
    // 5秒ごとにドット表示
    if ((millis() - advStart) % 5000 < 50) {
      Serial.print('.');
    }
    yield();
  }
  Serial.println();

  Bluefruit.Advertising.stop();
  Serial.println(F("[BLE] アドバタイズ停止"));
  ledOff();

  // スリープ（検証用: delay ベース。本番は RTC2 deepSleep に差し替え）
  Serial.print(F("[SLEEP] ")); Serial.print(SLEEP_SECONDS); Serial.println(F(" sec..."));
  pinOn(LED_BLUE);  // Blue = スリープ中

  for (uint32_t i = 0; i < SLEEP_SECONDS; i++) {
    delay(1000);
    yield();
  }

  ledOff();
  Serial.println(F("[WAKEUP]"));
}
