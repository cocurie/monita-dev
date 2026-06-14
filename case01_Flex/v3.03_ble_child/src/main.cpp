/**
 * Monita Flex v3.03 — BLE 子機スケッチ
 *
 * 動作:
 *   - 20分ごとに起床して全チャンネル計測
 *   - DS3231 の時刻が「毎時0分」のとき BLE アドバタイズを開始
 *   - ADV_DURATION_MIN 分間アドバタイズして停止 → ディープスリープ
 *
 * タイミング例:
 *   :00 起床 → 計測 → アドバタイズ開始（10分）
 *   :10 アドバタイズ停止 → スリープ（20分）
 *   :30 起床 → 計測のみ → スリープ（20分）
 *   :50 起床 → 計測のみ → スリープ（20分）
 *   次の :00 でアドバタイズ
 *
 * BLE Manufacturer Data フォーマット（16バイト）:
 *   [0-1]  Company ID : 0xFF 0xFF
 *   [2]    Pkt type   : 0x03（Monita Flex v3.03）
 *   [3]    Device ID  : DEVICE_ID
 *   [4-5]  CH1        : int16_t LE（ひずみ με、温度×10、等）
 *   [6-7]  CH2        : int16_t LE
 *   [8-9]  CH3        : int16_t LE
 *   [10-11] CH4       : int16_t LE
 *   [12-13] BATT      : uint16_t LE（mV）
 *   [14]   Hour       : uint8_t（DS3231 時）
 *   [15]   Minute     : uint8_t（DS3231 分）
 *
 * 将来拡張:
 *   親機のアドバタイズを受信して時刻同期する機能を追加予定
 *
 * 対象ハード: Monita Flex v3.03 基板（XIAO nRF52840）
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <HX711.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <bluefruit.h>
#include <nrf.h>

// ============================================================
// ▼ アプリ設定（ここを変更する）
// ============================================================

#define DEBUG_MODE        1       // 1: シリアルログ有効（本番は 0）
#define DEBUG_NO_SLEEP    0       // 1: ディープスリープをスキップ（USB 接続デバッグ用）

static const uint8_t  DEVICE_ID           = 0x01;  // 子機 ID（複数台時は変える）
static const uint32_t MEASURE_INTERVAL_MIN = 20;   // 計測間隔（分）
static const uint32_t ADV_DURATION_MIN     = 10;   // アドバタイズ継続時間（分）
// アドバタイズ開始条件: DS3231 の 分 が ADV_TRIGGER_MIN 以下
// （スリープ誤差で :01〜:02 に起床しても対応できるよう少し余裕を持たせる）
static const uint8_t  ADV_TRIGGER_MIN      = 2;    // 毎時 :00〜:02 のときアドバタイズ

// ── ひずみ補正係数 ──────────────────────────────────────────
// STRAIN_SCALE=1.0f のとき生値そのまま（未キャリブレーション）
// キャリブレーション後: STRAIN_SCALE = 生値変化量 / 既知ひずみ[με]
// v3.02 実測参考値: 1110.0f
static const float STRAIN_SCALE = 1.0f;

// ── チャンネル割り当て ──────────────────────────────────────
//   1 = HX711（ひずみ・荷重）
//   2 = TCA9546A 経由 I2C（DS3231 温度など、0x68）
//   3 = DS18B20（1-Wire、4.7kΩプルアップ必須）
const uint8_t CH_ASSIGN[4] = {1, 1, 1, 1};

// ── DS3231 設定（初回のみ時刻設定が必要な場合） ────────────
#define DS3231_SET_TIME  0     // 1: 起動時に時刻書き込み（書き込み後 0 に戻す）
#define DS3231_YEAR      26    // 西暦下2桁
#define DS3231_MONTH     6
#define DS3231_DAY       15
#define DS3231_HOUR      9
#define DS3231_MIN       0
#define DS3231_SEC       0

// ============================================================
// ピン定義
// ============================================================
#define HX711_SCK_PIN   6
#define HX711_DOUT_PIN  7
#define BATT_ANALOG_PIN A3
#define SW_POWER_PIN    10
#define TCA9534_ADDR    0x20
#define TCA9546A_ADDR   0x70
#define DS3231_ADDR     0x68

// ============================================================
// RTC2 ディープスリープ
// ============================================================
#define RTC2_PRESCALER        4095U
#define RTC2_TICKS_PER_SECOND 8U
#define RTC2_COUNTER_MASK     0x00FFFFFFU

static volatile bool s_rtc2Wake = false;

extern "C" void RTC2_IRQHandler(void) {
  if (NRF_RTC2->EVENTS_COMPARE[0]) {
    NRF_RTC2->EVENTS_COMPARE[0] = 0;
    (void)NRF_RTC2->EVENTS_COMPARE[0];
    NRF_RTC2->INTENCLR = RTC_INTENCLR_COMPARE0_Msk;
    NRF_RTC2->TASKS_STOP = 1;
    s_rtc2Wake = true;
  }
}

static void deepSleep(uint32_t minutes) {
#if DEBUG_MODE && DEBUG_NO_SLEEP
  Serial.println(F("[Sleep SKIP]"));
  delay(3000);
  return;
#endif
  digitalWrite(SW_POWER_PIN, LOW);
  Serial1.end();

#if DEBUG_MODE
  Serial.printf("[Sleep] %u 分\n", minutes);
  Serial.flush();
  delay(100);
#endif

  if (minutes == 0) minutes = 1;
  uint64_t ticks64 = (uint64_t)minutes * 60ULL * RTC2_TICKS_PER_SECOND;
  if (ticks64 > RTC2_COUNTER_MASK) ticks64 = RTC2_COUNTER_MASK;
  uint32_t ticks = (uint32_t)ticks64;

  s_rtc2Wake = false;
  NRF_RTC2->TASKS_STOP  = 1;
  NRF_RTC2->TASKS_CLEAR = 1;
  NRF_RTC2->PRESCALER   = RTC2_PRESCALER;
  NRF_RTC2->EVTENCLR    = 0xFFFFFFFFU;
  NRF_RTC2->EVENTS_COMPARE[0] = 0;
  (void)NRF_RTC2->EVENTS_COMPARE[0];
  NRF_RTC2->CC[0]       = ticks;
  NRF_RTC2->INTENCLR    = 0xFFFFFFFFU;
  NRF_RTC2->INTENSET    = RTC_INTENSET_COMPARE0_Msk;
  NVIC_SetPriority(RTC2_IRQn, 7);
  NVIC_ClearPendingIRQ(RTC2_IRQn);
  NVIC_EnableIRQ(RTC2_IRQn);
  NRF_RTC2->TASKS_START = 1;

  while (!s_rtc2Wake) { __DSB(); __WFI(); }

  // 復帰後: 周辺電源 ON
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);
  Wire.begin();
}

// ============================================================
// DS3231 時刻
// ============================================================
struct RtcTime { uint8_t hour, min, sec; };

static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

static bool ds3231GetTime(RtcTime &t) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(DS3231_ADDR, (uint8_t)3) < 3) return false;
  t.sec  = bcd2dec(Wire.read() & 0x7F);
  t.min  = bcd2dec(Wire.read() & 0x7F);
  t.hour = bcd2dec(Wire.read() & 0x3F);
  return true;
}

static void ds3231SetTime(uint8_t yr2, uint8_t mo, uint8_t dy,
                          uint8_t hr, uint8_t mn, uint8_t sc) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  Wire.write(dec2bcd(sc));
  Wire.write(dec2bcd(mn));
  Wire.write(dec2bcd(hr));
  Wire.write(0x01);          // day of week（未使用）
  Wire.write(dec2bcd(dy));
  Wire.write(dec2bcd(mo));
  Wire.write(dec2bcd(yr2));
  Wire.endTransmission();
}

// DS3231 内蔵温度（×10、例: 245 = 24.5℃）
static int16_t ds3231GetTemp() {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x11);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(DS3231_ADDR, (uint8_t)2) < 2) return 0;
  int8_t  msb = (int8_t)Wire.read();
  uint8_t lsb = Wire.read();
  return (int16_t)(msb * 10 + (int16_t)((lsb >> 6) * 25 / 10));
}

// ============================================================
// TCA9534（SN74LV4052 MUX A/B 制御）
// ============================================================
static bool tca9534Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool tca9534Read(uint8_t reg, uint8_t *out) {
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TCA9534_ADDR, (uint8_t)1) < 1) return false;
  *out = Wire.read();
  return true;
}

static bool tca9534Init() {
  if (!tca9534Write(0x02, 0x00)) return false;
  if (!tca9534Write(0x03, 0xFC)) return false;
  uint8_t out = 0;
  if (!tca9534Read(0x01, &out)) return false;
  return tca9534Write(0x01, out & 0xFC);
}

static bool muxSelect(uint8_t ch) {   // ch: 1〜4
  uint8_t idx  = (ch - 1) & 0x03;
  uint8_t a    = idx & 0x01;
  uint8_t b    = (idx >> 1) & 0x01;
  uint8_t bits = (uint8_t)(b | (a << 1));
  uint8_t out  = 0;
  if (!tca9534Read(0x01, &out)) return false;
  return tca9534Write(0x01, (out & 0xFC) | (bits & 0x03));
}

// ============================================================
// TCA9546A（I2C マルチプレクサ）
// ============================================================
static bool tcaSelect(uint8_t ch) {   // ch: 0〜3
  Wire.beginTransmission(TCA9546A_ADDR);
  Wire.write((uint8_t)(1 << ch));
  return Wire.endTransmission() == 0;
}

static void tcaDisable() {
  Wire.beginTransmission(TCA9546A_ADDR);
  Wire.write(0x00);
  Wire.endTransmission();
}

// ============================================================
// HX711
// ============================================================
static HX711 hx;

static void hxBegin(uint8_t ch) {
  muxSelect(ch);
  delay(10);
  pinMode(HX711_SCK_PIN, OUTPUT);
  digitalWrite(HX711_SCK_PIN, LOW);
  delay(10);
  hx.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
}

static bool hxRead(int32_t *out) {
  unsigned long t = millis();
  while (!hx.is_ready()) {
    if (millis() - t > 1500) { *out = 0; return false; }
  }
  *out = hx.read();
  return true;
}

// ============================================================
// DS18B20（1-Wire）
// ============================================================
static OneWire      s_ow(HX711_SCK_PIN);
static DallasTemperature s_ds(&s_ow);

static void ds18b20Init() {
  s_ow = OneWire(HX711_SCK_PIN);
  s_ds = DallasTemperature(&s_ow);
  s_ds.begin();
  s_ds.setResolution(12);
}

// ch: 1〜4 → MUX 切替後に DS18B20 読み取り（×10）
static int16_t ds18b20Read(uint8_t ch) {
  muxSelect(ch);
  delay(10);
  s_ds.requestTemperatures();
  delay(750);  // 12bit 変換待ち
  float t = s_ds.getTempCByIndex(0);
  if (t == DEVICE_DISCONNECTED_C) return 0;
  return (int16_t)(t * 10.0f);
}

// ============================================================
// バッテリー電圧
// ============================================================
static uint16_t readBattMv() {
  int raw = analogRead(BATT_ANALOG_PIN);
  float v  = raw * (3.3f / 4095.0f) * 1.8696f;
  return (uint16_t)(v * 1000.0f);
}

// ============================================================
// BLE アドバタイズ
// ============================================================
static const uint8_t MFR_COMPANY[2] = {0xFF, 0xFF};
static const uint8_t PKT_TYPE       = 0x03;  // v3.03

static void bleAdvertise(int16_t ch[4], uint16_t battMv,
                         uint8_t hour, uint8_t min_val) {
  uint8_t buf[16];
  buf[0]  = MFR_COMPANY[0];
  buf[1]  = MFR_COMPANY[1];
  buf[2]  = PKT_TYPE;
  buf[3]  = DEVICE_ID;
  for (int i = 0; i < 4; i++) {
    buf[4 + i * 2]     = (uint8_t)(ch[i] & 0xFF);
    buf[4 + i * 2 + 1] = (uint8_t)((ch[i] >> 8) & 0xFF);
  }
  buf[12] = (uint8_t)(battMv & 0xFF);
  buf[13] = (uint8_t)((battMv >> 8) & 0xFF);
  buf[14] = hour;
  buf[15] = min_val;

  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addManufacturerData(buf, sizeof(buf));
  Bluefruit.ScanResponse.addName();

  // 1000ms 間隔（省電力重視）
  Bluefruit.Advertising.setInterval(1600, 1600);
  Bluefruit.Advertising.setFastTimeout(0);
  Bluefruit.Advertising.start(0);
}

// ============================================================
// Arduino エントリ
// ============================================================
void setup() {
#if DEBUG_MODE
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();
  Serial.println(F("\n[v3.03 BLE子機] 起動"));
  Serial.printf("  Device ID      : 0x%02X\n", DEVICE_ID);
  Serial.printf("  計測間隔       : %u 分\n", MEASURE_INTERVAL_MIN);
  Serial.printf("  アドバタイズ   : 毎時 :00〜:02 → %u 分間\n", ADV_DURATION_MIN);
#endif

  analogReadResolution(12);

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  Wire.begin();

  // TCA9534 初期化
  if (!tca9534Init()) {
#if DEBUG_MODE
    Serial.println(F("[ERROR] TCA9534 init failed"));
#endif
    while (true) delay(1000);
  }

  // DS3231 時刻設定（DS3231_SET_TIME=1 のときのみ）
#if DS3231_SET_TIME
  ds3231SetTime(DS3231_YEAR, DS3231_MONTH, DS3231_DAY,
                DS3231_HOUR, DS3231_MIN, DS3231_SEC);
  Serial.println(F("[DS3231] 時刻設定完了。DS3231_SET_TIME を 0 に戻してください。"));
#endif

  // BLE 初期化
  Bluefruit.begin();
  char name[16];
  snprintf(name, sizeof(name), "Monita-%02X", DEVICE_ID);
  Bluefruit.setName(name);

#if DEBUG_MODE
  Serial.println(F("[BLE] init OK"));
#endif
}

void loop() {
  // ── 周辺電源 ON & I2C 初期化 ──
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);
  Wire.begin();
  tca9534Init();

  // ── DS3231 時刻読み取り ──
  RtcTime rtc;
  bool rtcOk = ds3231GetTime(rtc);

#if DEBUG_MODE
  if (rtcOk) {
    Serial.printf("\n[時刻] %02d:%02d:%02d\n", rtc.hour, rtc.min, rtc.sec);
  } else {
    Serial.println(F("[DS3231] 読み取り失敗"));
  }
#endif

  // ── 全チャンネル計測 ──
  int16_t ch[4] = {0, 0, 0, 0};
  bool ds18Inited = false;

  for (int i = 0; i < 4; i++) {
    if (CH_ASSIGN[i] == 1) {
      // HX711（ひずみ）
      hxBegin((uint8_t)(i + 1));
      int32_t raw = 0;
      hxRead(&raw);
      ch[i] = (int16_t)((float)raw / STRAIN_SCALE);
#if DEBUG_MODE
      Serial.printf("  CH%d [HX711] raw=%ld → %d με\n", i + 1, raw, ch[i]);
#endif

    } else if (CH_ASSIGN[i] == 2) {
      // TCA9546A 経由 DS3231 温度
      if (tcaSelect((uint8_t)i)) {
        ch[i] = ds3231GetTemp();
        tcaDisable();
#if DEBUG_MODE
        Serial.printf("  CH%d [DS3231] %d (×10℃)\n", i + 1, ch[i]);
#endif
      }

    } else if (CH_ASSIGN[i] == 3) {
      // DS18B20（1-Wire）
      if (!ds18Inited) { ds18b20Init(); ds18Inited = true; }
      ch[i] = ds18b20Read((uint8_t)(i + 1));
#if DEBUG_MODE
      Serial.printf("  CH%d [DS18B20] %d (×10℃)\n", i + 1, ch[i]);
#endif
    }
  }

  uint16_t batt = readBattMv();
#if DEBUG_MODE
  Serial.printf("  BATT: %u mV\n", batt);
#endif

  // ── アドバタイズ判定（毎時 :00〜ADV_TRIGGER_MIN 分） ──
  bool doAdv = rtcOk && (rtc.min <= ADV_TRIGGER_MIN);

  if (doAdv) {
#if DEBUG_MODE
    Serial.printf("[BLE] アドバタイズ開始（%u 分間）\n", ADV_DURATION_MIN);
#endif
    bleAdvertise(ch, batt, rtc.hour, rtc.min);

    // ADV_DURATION_MIN 分間アドバタイズ継続
    uint32_t advMs = (uint32_t)ADV_DURATION_MIN * 60000UL;
    uint32_t start = millis();
    while (millis() - start < advMs) {
      delay(1000);
      yield();
    }

    Bluefruit.Advertising.stop();
#if DEBUG_MODE
    Serial.println(F("[BLE] アドバタイズ停止"));
#endif
  }

  // ── ディープスリープ（20分）──
#if DEBUG_MODE
  Serial.printf("[Sleep] %u 分\n", MEASURE_INTERVAL_MIN);
  Serial.flush();
#endif
  deepSleep(MEASURE_INTERVAL_MIN);
}
