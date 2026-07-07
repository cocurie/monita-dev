/**
 * Monita Flex v3.04 — 計測＋SDカード記録＋Sigfox/BLE 送信（PlatformIO / XIAO nRF52840）
 *
 * 【対象ハード】
 *   Monita Flex **v3.04 基板**。v3.03 と計測内容は同一だが、基板改版により以下が変更:
 *     - 3V3_SW（周辺電源）制御: D10 直結GPIO → TCA9534 P2（I2C経由）へ移動
 *     - D10 は SD カード SPI SCK へ転用（D1=MOSI, D2=MISO, CS=TCA9534 P3）
 *     - SD カードスロット(J2)追加。毎サイクル CSV 記録
 *     - 電池電圧センス: A3(=D3/P0.29) は物理同一ピンのまま
 *   ※ v3.03 以前の基板には書き込まないこと（3V3_SW/SDピン割当が異なる）。
 *
 * 【役割の概要】
 *   - HX711 最大 4ch（4052 MUX 経由）から荷重／ひずみを取得
 *   - DS3231 の温度レジスタから基板温度を取得
 *   - 電池電圧をアナログで取得
 *   - SD カード /log.csv に「datetime,ch1..4,temp,batt」を毎サイクル追記
 *   - Sigfox（UART）または BLE アドバタイズでクラウド送信
 *   - 送信後、3V3_SW を落とし、内蔵 RTC2 で指定分スリープして繰り返し
 *
 * 【I2C はビットバン（Wire 不使用）】
 *   BLE モードでは SoftDevice が Wire.begin() より前に有効化され、TWIM の IRQ 優先度が
 *   SoftDevice の予約帯と競合してハングし得る。GPIO ビットバン I2C（SDA=D4/SCL=D5）で
 *   TCA9534(0x20) と DS3231(0x68) のみ制御することで Sigfox/BLE 両モードで安定動作させる。
 *   ※ CH_ASSIGN に 2(TCA9546A経由MPU) / 3(DS18B20) を使う場合は、それらの経路を
 *      ビットバン/OneWire で再実装し、lib_deps を復活させること（現状 {1,1,1,1} で未使用）。
 *
 * 【SD カード初期化（cold boot 対策）】
 *   xiao_ble_sd_flex で実証済み。sd.begin() の単純リトライでは cold boot で失敗するが、
 *   各リトライ前に tca9534Init() を再実行して P2(3V3_SW)/P3(CS) を再アサートすると安定する。
 *
 * 【スリープの方式】
 *   nRF52840 内蔵 RTC2 ＋ LFCLK。RTC1 は FreeRTOS 用のためアプリでは触らない。
 *
 * 【DS3231 時刻設定】
 *   DS3231_SET_TIME=1 にして書き込む → 起動時に下記定数を DS3231 へ書込 → 0 に戻して再ビルド。
 */

/*
 * =============================================================================
 * エラー処理一覧（フラグ s_errors／対応 LED／サイクル動作）
 * =============================================================================
 * | ビット／名前       | 発生条件                                | LED   | 動作         |
 * | ERR_HX711_TIMEOUT  | HX711 が is_ready で 1000 ms 超待ち     | 赤    | 送信しない→sleep |
 * | ERR_TCA9534_I2C    | TCA9534 の I²C 失敗                     | 赤    | 同上         |
 * | ERR_DS3231_I2C     | DS3231 の I²C 失敗（温度・時刻）        | 赤    | 同上         |
 * | ERR_SIGFOX_AT      | AT$SF= の応答に OK なし／タイムアウト   | 赤    | そのサイクル赤→sleep |
 * =============================================================================
 */

#include <Arduino.h>
// nRF レジスタ直接操作（RTC2 割り込み・スリープ待ち用）
#include <nrf.h>
#include <math.h>
#include <string.h>
// SD カード（SPIM2 使用。CS は TCA9534 P3 が能動駆動）
#include <SPI.h>
#include <SdFat.h>
// 重量センサ用 ADC ブリッジ
#include <HX711.h>
// コア同梱。タレオフセットのフラッシュ保存（リセット後も保持）に使用
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
// USB CDC（Serial）。DEBUG_MODE 時のログ出力に使用
#include <Adafruit_TinyUSB.h>
// BLE モード時のみ使用
#ifdef COMM_MODE_BLE
#include <bluefruit.h>
#endif

// ============================================================
// 通信モード選択（platformio.ini の build_flags で指定）
//   -D COMM_MODE_SIGFOX  … Sigfox 送信モード
//   -D COMM_MODE_BLE     … BLE アドバタイズモード
// ============================================================
#if !defined(COMM_MODE_SIGFOX) && !defined(COMM_MODE_BLE)
  #error "platformio.ini の build_flags に -D COMM_MODE_SIGFOX または -D COMM_MODE_BLE を指定してください"
#endif

// ============================================================
// BLE モード設定（COMM_MODE_BLE 時のみ有効）
// ============================================================
#ifdef COMM_MODE_BLE
static const uint8_t  DEVICE_ID           = 0x01;  // 子機 ID（複数台時は変える: 0x01〜0xFF）
static const uint32_t MEASURE_INTERVAL_MIN = 20;   // 計測間隔（分）
static const uint32_t ADV_DURATION_MIN     = 10;   // アドバタイズ継続時間（分）
static const uint8_t  ADV_TRIGGER_MIN      = 2;    // 毎時 :00〜:02 のときアドバタイズ
// 1: 時刻条件・スリープをスキップして常時アドバタイズ（Gateway テスト用）。本番は 0。
#define TEST_ADV_MODE  1
#endif

// ============================================================
// アプリ設定（ここを主に編集する）
// ============================================================

#define DEBUG_MODE           1        // 1: USB Serial デバッグログ有効。本番は 0
#define DEBUG_NO_SLEEP       1        // 1: deepSleep をスキップして即 loop() に戻る（DEBUG_MODE 1 時のみ有効）
#define DEBUG_NO_SIGFOX      1        // 1: AT$SF= を送らずログだけ出す（デューティサイクル節約）
#define SLEEP_MINUTES        15       // 1サイクル後のスリープ時間（分）
#define BOOT_BLUE_MS         500      // 電源 ON 後の青点灯時間（ms）
#define BUTTON_LONG_PRESS_MS 5000UL  // D0 長押し閾値（ms）: 以上で tare、未満でリセット

// ── DS3231 RTC 設定 ────────────────────────────────────────────
// DS3231_SET_TIME=1 にすると起動時（setup）に以下の時刻を DS3231 に書き込む。
// 書き込み後は必ず 0 に戻してビルドし直すこと（毎起動で時刻が上書きされるため）。
#define DS3231_SET_TIME      0        // 1: 起動時に DS3231 へ時刻を書き込む。書き込み後は 0 に戻す
#define DS3231_INIT_YEAR     2026     // 書き込む年（西暦 4 桁）
#define DS3231_INIT_MONTH    7        // 書き込む月（1〜12）
#define DS3231_INIT_DAY      7        // 書き込む日（1〜31）
#define DS3231_INIT_HOUR     14       // 書き込む時（0〜23、24h 形式）
#define DS3231_INIT_MIN      0        // 書き込む分（0〜59）
#define DS3231_INIT_SEC      0        // 書き込む秒（0〜59）

// USE_DS3231_TIMESTAMP=1 にすると各サイクルで DS3231 の現在時刻を読み出しログ出力する。
#define USE_DS3231_TIMESTAMP 1        // 1: 各サイクルで時刻を読み出す

// ── Sigfox 設定 ────────────────────────────────────────────
#define SIGFOX_TX_PIN 8    // D8: XIAO TX → BRKLSM100 RX
#define SIGFOX_RX_PIN 9    // D9: XIAO RX ← BRKLSM100 TX
#define SIGFOX_BAUD   9600

// ── ステータス LED ─────────────────────────────────────────────
#define USE_WS2812_STATUS_LED 0

#if USE_WS2812_STATUS_LED
#include <Adafruit_NeoPixel.h>
#define NEOPIXEL_PIN 16
#define NEOPIXEL_COUNT 1
#endif

// 各スロット i（0〜3）が CH(i+1) に相当。
//   1 = HX711（ひずみ・荷重）
//   2 = TCA9546A 経由 I2C センサ ※ v3.04 では未対応（ビットバン再実装が必要）
//   3 = DS18B20（1-Wire 温度）    ※ v3.04 では未対応（OneWire 復活が必要）
const uint8_t CH_ASSIGN[4] = {1, 1, 1, 1};

// ── ひずみ補正係数（キャリブレーション） ──────────────────────────
//   送信値（με）= HX711生値 / STRAIN_SCALE
//   v3.02 実測値: STRAIN_SCALE = 1110.0f（環境・ゲージにより異なる）
static const float STRAIN_SCALE = 1110.0f;  // 1.0f = 生値そのまま出力（未キャリブレーション）

// HX711 1ch あたりの生サンプル数（中央値をとる前の個数）
#define DATA_NUM 5

#ifndef PI
#define PI 3.14159265358979323846
#endif

// ============================================================
// ピン番号（Arduino ピン番号 = XIAO の Dx。正本 v3.04 ネットリスト）
// ============================================================

// HX711: PD_SCK=D6, DOUT=D7（SN74LV4052 MUX 経由で各 JP に接続）
#define HX711_SCK_PIN  6
#define HX711_DOUT_PIN 7

// 電池分圧センス = A3（= D3 = P0.29）。v3.03 と物理同一ピン。解像度は setup で 12bit
#define BATT_ANALOG_PIN A3

// ── I2C（ビットバン） ────────────────────────────────────────
#define I2C_SDA       D4   // P0.04
#define I2C_SCL       D5   // P0.05
#define TCA9534_ADDR  0x20 // SN74LV4052 A/B + 3V3_SW + SD CS を駆動
#define DS3231_ADDR   0x68 // RTC + 温度センサ（固定）

// ── SD カード SPI（v3.04） ───────────────────────────────────
#define PIN_SD_MISO       D2   // P0.28 → J2 DAT0
#define PIN_SD_SCK        D10  // P1.15 → J2 CLK
#define PIN_SD_MOSI       D1   // P0.03 → J2 CMD
// SdFat は GPIO CS を前提とするが、v3.04 の SD CS は TCA9534 P3 が能動駆動する。
// sdCsInit/sdCsWrite を no-op にするため、この番号は SdFat の内部管理用でしかなく
// 実際にはどのピンも操作しない（D9=Sigfox RX と衝突させないため物理設定はしない）。
#define PIN_SD_CS_DUMMY   D9
#define SPI_SPEED_MHZ     4    // 失敗時は 1 に落とす

// D0 = タクトスイッチ（GND ショート、内部プルアップ）
#define USER_BUTTON_PIN 0

// ── ログ ─────────────────────────────
static bool       const ENABLE_LOGGING = true;
static char const*      LOG_FILE       = "/log.csv";

// ============================================================
// エラーフラグ
// ============================================================

enum : uint32_t {
  ERR_NONE = 0,
  ERR_HX711_TIMEOUT = 1u << 0,
  ERR_SIGFOX_AT = 1u << 3,
  ERR_TCA9534_I2C = 1u << 4,
  ERR_DS3231_I2C = 1u << 5,
};

static uint32_t s_errors;

// TCA9534 出力レジスタキャッシュ（唯一の真実。P0/P1=MUX, P2=3V3_SW, P3=SD CS を同居管理）
static uint8_t s_tca9534Out = 0x00;

// SD カード初期化済みフラグ
static bool s_sdReady = false;

// D0 ボタン: ISR からメインへ「押下エッジあり」を伝えるフラグ
static volatile bool s_btnFlag = false;
// deepSleep() がボタン長押しで抜けたとき、次の loop() で tare を実行する
static bool s_pendingTare = false;

static void btnISR() {
  s_btnFlag = true;
}

// SD / DS3231 前方宣言
static bool tca9534Init();
static void getTimestamp(char *buf, size_t len);

// ============================================================
// ステータス LED（離散 RGB／または NeoPixel）
// ============================================================

#if USE_WS2812_STATUS_LED
static Adafruit_NeoPixel s_px(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_RGB + NEO_KHZ800);

static void rgbHwBegin() {
  s_px.begin();
  s_px.setBrightness(255);
  s_px.clear();
  s_px.show();
}

static void rgbHwShow(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness8) {
  s_px.setBrightness(brightness8);
  s_px.setPixelColor(0, r, g, b);
  s_px.show();
}

#else

static void rgbHwBegin() {
#ifdef LED_RED
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
#endif
}

// XIAO nRF52840 の RGB LED はアクティブ LOW（LED_STATE_ON=0）のため値を反転する。
static void rgbHwShow(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness8) {
#ifdef LED_RED
  uint16_t rr = (uint16_t)r * brightness8 / 255;
  uint16_t gg = (uint16_t)g * brightness8 / 255;
  uint16_t bb = (uint16_t)b * brightness8 / 255;
  analogWrite(LED_RED,   255 - (int)rr);
  analogWrite(LED_GREEN, 255 - (int)gg);
  analogWrite(LED_BLUE,  255 - (int)bb);
#endif
}

#endif

#define RGB_BRIGHT_FULL 255
#define RGB_BRIGHT_SLEEP_BLUE 16

static void rgbOff() {
#if USE_WS2812_STATUS_LED
  rgbHwShow(0, 0, 0, 1);
#else
#ifdef LED_RED
  analogWrite(LED_RED,   255);
  analogWrite(LED_GREEN, 255);
  analogWrite(LED_BLUE,  255);
#endif
#endif
}

static void statusBootBlueStrong() { rgbHwShow(0, 0, 255, RGB_BRIGHT_FULL); }
static void statusIdleBlueDim()    { rgbHwShow(0, 0, 255, RGB_BRIGHT_SLEEP_BLUE); }
static void statusMeasureGreen()   { rgbHwShow(0, 255, 0, RGB_BRIGHT_FULL); }
static void statusErrorRed()       { rgbHwShow(255, 0, 0, RGB_BRIGHT_FULL); }

// Sigfox 送信中の緑点滅（ノンブロッキング）
static unsigned long s_sigfoxBlinkLastMs;
static bool s_sigfoxBlinkOn;

static void statusSigfoxBlinkReset() {
  s_sigfoxBlinkLastMs = millis();
  s_sigfoxBlinkOn = true;
  rgbHwShow(0, 255, 0, RGB_BRIGHT_FULL);
}

static void statusSigfoxBlinkTick() {
  unsigned long now = millis();
  if (now - s_sigfoxBlinkLastMs >= 200) {
    s_sigfoxBlinkLastMs = now;
    s_sigfoxBlinkOn = !s_sigfoxBlinkOn;
    if (s_sigfoxBlinkOn) rgbHwShow(0, 255, 0, RGB_BRIGHT_FULL);
    else                 rgbOff();
  }
}

// ============================================================
// I2C（GPIO ビットバン。Wire/TWIM 不使用）
// ─ SDA はオープンドレイン / SCL はプッシュプル ─
// ============================================================

static void sdaHi() { pinMode(I2C_SDA, INPUT_PULLUP); delayMicroseconds(5); }
static void sdaLo() { pinMode(I2C_SDA, OUTPUT); digitalWrite(I2C_SDA, LOW); delayMicroseconds(5); }
static void sclHi() { digitalWrite(I2C_SCL, HIGH); delayMicroseconds(5); }
static void sclLo() { digitalWrite(I2C_SCL, LOW);  delayMicroseconds(5); }

static void i2cInit() {
  pinMode(I2C_SCL, OUTPUT);
  sclHi(); sdaHi();
  delayMicroseconds(50);
}

static void i2cStart() { sdaHi(); sclHi(); sdaLo(); sclLo(); }
static void i2cStop()  { sdaLo(); sclHi(); sdaHi(); }

static bool i2cWriteByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    if ((b >> i) & 1) sdaHi(); else sdaLo();
    sclHi(); sclLo();
  }
  sdaHi();  // SDA 解放 → スレーブが ACK=LOW を返す
  sclHi();
  bool ack = (digitalRead(I2C_SDA) == LOW);
  sclLo();
  return ack;
}

static uint8_t i2cReadByte(bool sendAck) {
  uint8_t b = 0;
  sdaHi();
  for (int i = 7; i >= 0; i--) {
    sclHi();
    b = (uint8_t)((b << 1) | (digitalRead(I2C_SDA) ? 1 : 0));
    sclLo();
  }
  if (sendAck) sdaLo(); else sdaHi();
  sclHi(); sclLo(); sdaHi();
  return b;
}

// ============================================================
// TCA9534 — SN74LV4052 A/B（P0/P1）＋ 3V3_SW（P2）＋ SD CS（P3）
//
// v3.04 では 1 バイトの出力レジスタに 4 役を同居させる:
//   P0 = 4052 MUX B セレクト
//   P1 = 4052 MUX A セレクト
//   P2 = MOSFET_GATE（3V3_SW ON/OFF）
//   P3 = SD CS（常時 LOW アサート）
// 出力キャッシュ s_tca9534Out を唯一の真実とし、各操作はビット単位で更新する。
// ============================================================

static bool tca9534WriteReg(uint8_t reg, uint8_t val) {
  i2cStart();
  bool ok = i2cWriteByte((uint8_t)((TCA9534_ADDR << 1) | 0x00));
  ok &= i2cWriteByte(reg);
  ok &= i2cWriteByte(val);
  i2cStop();
  return ok;
}

static bool __attribute__((unused)) tca9534ReadReg(uint8_t reg, uint8_t *out) {
  i2cStart();
  bool ok = i2cWriteByte((uint8_t)((TCA9534_ADDR << 1) | 0x00));
  ok &= i2cWriteByte(reg);
  i2cStart();
  ok &= i2cWriteByte((uint8_t)((TCA9534_ADDR << 1) | 0x01));
  *out = i2cReadByte(false);
  i2cStop();
  return ok;
}

/**
 * TCA9534 初期化（冪等。3V3_SW 復帰後も毎回呼べる）
 *   極性 0x02 = 0x00（正論理）
 *   方向 0x03 = 0xF0（P0〜P3 出力、P4〜P7 入力）
 *   出力 0x01 = 0x04（P2=1:3V3_SW ON, P3=0:CS アサート, P0/P1=0:MUX ch1）
 */
static bool tca9534Init() {
  if (!tca9534WriteReg(0x02, 0x00)) return false;
  if (!tca9534WriteReg(0x03, 0xF0)) return false;
  s_tca9534Out = 0x04;
  return tca9534WriteReg(0x01, s_tca9534Out);
}

/** ビット単位でポート出力を変更しキャッシュを更新する */
static bool tca9534SetBit(uint8_t bit, uint8_t val) {
  if (val) s_tca9534Out |=  (uint8_t)(1u << bit);
  else     s_tca9534Out &= ~(uint8_t)(1u << bit);
  return tca9534WriteReg(0x01, s_tca9534Out);
}

/**
 * 4052 MUX チャネル選択（P0/P1 のみ更新。P2=3V3_SW / P3=SD CS は保持）
 * ch: 1〜4。idx 0〜3 → P0=B, P1=A
 */
static void muxSelect(uint8_t ch) {
  uint8_t idx = (uint8_t)((ch - 1) & 0x03);
  const uint8_t a = idx & 0x01;
  const uint8_t b = (uint8_t)((idx >> 1) & 0x01);
  const uint8_t twoBits = (uint8_t)(b | (a << 1));  // bit0=B, bit1=A
  s_tca9534Out = (uint8_t)((s_tca9534Out & (uint8_t)~0x03U) | (twoBits & 0x03U));
  if (!tca9534WriteReg(0x01, s_tca9534Out)) {
    s_errors |= ERR_TCA9534_I2C;
    statusErrorRed();
  }
}

// ============================================================
// DS3231 — RTC ＋ 温度センサ（ビットバン I2C 直叩き）
//   温度: 0x11(MSB, signed) / 0x12(LSB, bit7:6 = 0.25℃)
//   時刻: 0x00〜0x06（BCD）
// ============================================================

static uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10U + (b & 0x0FU)); }
static uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10U) << 4) | (d % 10U)); }

struct Ds3231Time {
  uint8_t  sec;    // 0〜59
  uint8_t  min;    // 0〜59
  uint8_t  hour;   // 0〜23
  uint8_t  day;    // 1〜31
  uint8_t  month;  // 1〜12
  uint16_t year;   // 西暦 4 桁
};

// DS3231 に時刻を書き込む（DS3231_SET_TIME=1 のとき setup で呼ぶ）
static bool __attribute__((unused)) ds3231SetTime(uint8_t yr2, uint8_t mo, uint8_t day,
                          uint8_t hr, uint8_t mn, uint8_t sc) {
  i2cStart();
  bool ok = i2cWriteByte((uint8_t)((DS3231_ADDR << 1) | 0x00));
  ok &= i2cWriteByte(0x00);           // レジスタ先頭（seconds）
  ok &= i2cWriteByte(dec2bcd(sc));    // 0x00
  ok &= i2cWriteByte(dec2bcd(mn));    // 0x01
  ok &= i2cWriteByte(dec2bcd(hr));    // 0x02（24h）
  ok &= i2cWriteByte(0x01);           // 0x03 曜日（固定）
  ok &= i2cWriteByte(dec2bcd(day));   // 0x04
  ok &= i2cWriteByte(dec2bcd(mo));    // 0x05
  ok &= i2cWriteByte(dec2bcd(yr2));   // 0x06
  i2cStop();
  return ok;
}

// DS3231 から現在時刻を読み出す。成功時 true
static bool ds3231GetTime(Ds3231Time &t) {
  i2cStart();
  if (!i2cWriteByte((uint8_t)((DS3231_ADDR << 1) | 0x00))) { i2cStop(); return false; }
  if (!i2cWriteByte(0x00))                                 { i2cStop(); return false; }
  i2cStop();
  i2cStart();
  if (!i2cWriteByte((uint8_t)((DS3231_ADDR << 1) | 0x01))) { i2cStop(); return false; }
  t.sec   = bcd2dec(i2cReadByte(true)  & 0x7FU);
  t.min   = bcd2dec(i2cReadByte(true)  & 0x7FU);
  t.hour  = bcd2dec(i2cReadByte(true)  & 0x3FU);
  (void)i2cReadByte(true);                             // 曜日スキップ
  t.day   = bcd2dec(i2cReadByte(true)  & 0x3FU);
  t.month = bcd2dec(i2cReadByte(true)  & 0x1FU);
  t.year  = (uint16_t)(2000U + bcd2dec(i2cReadByte(false)));
  i2cStop();
  return (t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31 &&
          t.hour <= 23 && t.min <= 59 && t.sec <= 59);
}

// 温度（DS3231 内蔵センサ）。戻り値: 温度×10 を int（例: 253 = 25.3℃）
static int measureTemp() {
  i2cStart();
  if (!i2cWriteByte((uint8_t)((DS3231_ADDR << 1) | 0x00))) { i2cStop(); goto err; }
  if (!i2cWriteByte(0x11))                                 { i2cStop(); goto err; }
  i2cStop();
  {
    i2cStart();
    if (!i2cWriteByte((uint8_t)((DS3231_ADDR << 1) | 0x01))) { i2cStop(); goto err; }
    int8_t  msb = (int8_t)i2cReadByte(true);
    uint8_t lsb = i2cReadByte(false);
    i2cStop();
    float temp = (float)msb + (float)(lsb >> 6) * 0.25f;
    return (int)(temp * 10.0f);
  }
err:
  s_errors |= ERR_DS3231_I2C;
#if DEBUG_MODE
  Serial.println("[DS3231] temp read error");
#endif
  return 0;
}

// ============================================================
// 電池電圧（分圧後の ADC を実効電圧に戻す）
//   戻り値: mV 相当 int。1.8696 は分圧比に応じた係数（回路図 R 値と要整合）
// ============================================================

static int measureBatt() {
  int raw = analogRead(BATT_ANALOG_PIN);
  float v = raw * (3.3 / 4095.0);
  v *= 1.8696;
  return (int)(v * 1000);
}

// ============================================================
// SD カード（SdFat v2 / SPIM2）
//   CS は TCA9534 P3 が能動駆動。SdFat の CS 操作は no-op 化し D9 は触らない。
// ============================================================

// SPIM2 使用: SPIM0/1 は BSP で無効。SPIM3 はデフォルト SPI が使用。
static SPIClass SD_SPI(NRF_SPIM2, PIN_SD_MISO, PIN_SD_SCK, PIN_SD_MOSI);
static SdFat    sd;

// SdFat CS オーバーライド: CS は TCA9534 P3=LOW 固定済み。SdFat の CS 操作は無視する。
void sdCsInit(SdCsPin_t pin)            { (void)pin; }
void sdCsWrite(SdCsPin_t pin, bool lvl) { (void)pin; (void)lvl; }

// SD 電源投入前に SCK=LOW / MOSI=HIGH を確定させる（D10 フローティングノイズ対策）。
// D10 は BSP デフォルト SPI MOSI と共用のため、浮くと SD の CLK にノイズが乗り
// native 初期化が完了しない。
static void sdPrePinConfig() {
  pinMode(PIN_SD_SCK,  OUTPUT); digitalWrite(PIN_SD_SCK,  LOW);
  pinMode(PIN_SD_MOSI, OUTPUT); digitalWrite(PIN_SD_MOSI, HIGH);
}

/**
 * SD カード初期化（cold boot 対策込み。xiao_ble_sd_flex で実証済み）
 * 呼び出し前提: tca9534Init() 済み（3V3_SW ON / CS アサート）、sdPrePinConfig() 済み。
 */
static bool initSd() {
  pinMode(PIN_SD_MISO, INPUT_PULLUP);
  {
    uint32_t t0 = millis();
    while (!digitalRead(PIN_SD_MISO) && millis() - t0 < 3000) delay(10);
#if DEBUG_MODE
    Serial.print("[SD] MISO=");
    Serial.print(digitalRead(PIN_SD_MISO) ? "HIGH(OK)" : "LOW(still!)");
    Serial.print("  after "); Serial.print(millis() - t0); Serial.println("ms");
#endif
  }
  // CS assert（tca9534Init 内で実施）から sd.begin() まで 500ms 置く。
  delay(500);

  // 最大3回リトライ。cold boot 直後は tca9534Init() 一発目がラッチされないことが
  // あるため、各リトライ前に tca9534Init() を再実行して P2/P3 を再アサートする。
  for (int attempt = 1; attempt <= 3; attempt++) {
    tca9534Init();
    delay(10);
    SdSpiConfig cfg(PIN_SD_CS_DUMMY, DEDICATED_SPI, SD_SCK_MHZ(SPI_SPEED_MHZ), &SD_SPI);
    if (sd.begin(cfg)) {
#if DEBUG_MODE
      Serial.print("[SD] init OK  capacity=");
      Serial.print((uint32_t)(0.000512f * sd.card()->sectorCount()));
      Serial.println("MB");
#endif
      goto sd_ok;
    }
#if DEBUG_MODE
    Serial.print("[SD] attempt "); Serial.print(attempt);
    Serial.print(" failed  err=0x");
    Serial.print(sd.card()->errorCode(), HEX);
    Serial.print("/0x");
    Serial.println(sd.card()->errorData(), HEX);
#endif
    if (attempt < 3) delay(1000);
  }
#if DEBUG_MODE
  Serial.println("[SD] init failed");
#endif
  return false;

  sd_ok:
  // ログファイル: 存在しなければヘッダ行を作成
  if (ENABLE_LOGGING && !sd.exists(LOG_FILE)) {
    FsFile f = sd.open(LOG_FILE, O_WRITE | O_CREAT);
    if (f) {
      f.println("datetime,ch1,ch2,ch3,ch4,temp,batt");
      f.close();
#if DEBUG_MODE
      Serial.println("[SD] /log.csv created");
#endif
    }
  }
  return true;
}

// ============================================================
// HX711（ライブラリ 1 インスタンスを MUX 切替で共用）
// ============================================================

HX711 hx;

// チャンネルごとのタレオフセット（HX711 は1インスタンス共用のため切替のたび復元）
static long s_hx_tare_offset[4] = {0, 0, 0, 0};

// ── タレオフセット フラッシュ保存／復元（InternalFS / LittleFS） ──
static const char TARE_FILE[] = "/tare.bin";

static void saveTareOffsets() {
  if (!InternalFS.begin()) return;
  InternalFS.remove(TARE_FILE);
  // SdFat も `File`(=FsFile) を定義するため、InternalFS 側は完全修飾で曖昧さを回避
  Adafruit_LittleFS_Namespace::File f(InternalFS);
  if (f.open(TARE_FILE, FILE_O_WRITE)) {
    f.write((const uint8_t*)s_hx_tare_offset, sizeof(s_hx_tare_offset));
    f.close();
#if DEBUG_MODE
    Serial.println("[TARE] offsets saved to flash");
#endif
  }
}

static void loadTareOffsets() {
  if (!InternalFS.begin()) return;
  Adafruit_LittleFS_Namespace::File f(InternalFS);
  if (f.open(TARE_FILE, FILE_O_READ)) {
    if ((size_t)f.size() == sizeof(s_hx_tare_offset)) {
      f.read((uint8_t*)s_hx_tare_offset, sizeof(s_hx_tare_offset));
#if DEBUG_MODE
      Serial.print("[TARE] offsets loaded: ");
      for (int i = 0; i < 4; i++) { Serial.print(s_hx_tare_offset[i]); if (i < 3) Serial.print(", "); }
      Serial.println();
#endif
    }
    f.close();
  }
}

// HX711 全有効チャネルに tare を実行する（3V3_SW ON・I2C 初期化済みで呼ぶこと）
static void performTare() {
  if ((s_errors & ERR_TCA9534_I2C) != 0U) {
#if DEBUG_MODE
    Serial.println("[TARE] skipped (TCA9534 error)");
#endif
    return;
  }
  for (uint8_t i = 0; i < 4; i++) {
    if (CH_ASSIGN[i] != 1) continue;
    muxSelect((uint8_t)(i + 1));
    delay(10);
    hx.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
    unsigned long t = millis();
    while (!hx.is_ready()) { if (millis() - t > 1000) break; }
    if (hx.is_ready()) {
      hx.tare();
      s_hx_tare_offset[i] = hx.get_offset();
#if DEBUG_MODE
      Serial.print("[TARE] CH"); Serial.print(i + 1);
      Serial.print(" done (offset="); Serial.print(s_hx_tare_offset[i]); Serial.println(")");
#endif
    } else {
#if DEBUG_MODE
      Serial.print("[TARE] CH"); Serial.print(i + 1); Serial.println(" timeout");
#endif
    }
  }
  saveTareOffsets();
}

// 中央値（外れ値に強い簡易ロバスト化）
static float median(float *a, int n) {
  float t[5];
  memcpy(t, a, sizeof(float) * (size_t)n);
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - i - 1; j++)
      if (t[j] > t[j + 1]) { float x = t[j]; t[j] = t[j + 1]; t[j + 1] = x; }
  return t[n / 2];
}

// 指定チャネル（1〜4）の HX711 に MUX を合わせてからライブラリ begin
static void hxBegin(uint8_t ch) {
#if DEBUG_MODE
  Serial.print("[HX BEGIN CH"); Serial.print(ch); Serial.println("]");
#endif
  muxSelect(ch);
  delay(10);
  // SCK を LOW に確定してから begin（起動時に SCK が浮くと power-down になる）
  pinMode(HX711_SCK_PIN, OUTPUT);
  digitalWrite(HX711_SCK_PIN, LOW);
  delay(10);
  hx.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
  hx.set_offset(s_hx_tare_offset[ch - 1]);
}

// true=正常、false=タイムアウト（ERR_HX711_TIMEOUT をセット）
static bool hxRead(int *out) {
  unsigned long start = millis();
  while (!hx.is_ready()) {
    if (millis() - start > 1000) {
#if DEBUG_MODE
      Serial.println("[HX TIMEOUT]");
#endif
      s_errors |= ERR_HX711_TIMEOUT;
      statusErrorRed();
      *out = 0;
      return false;
    }
  }
  float b[DATA_NUM];
  for (int i = 0; i < DATA_NUM; i++) b[i] = hx.get_value();
  *out = (int)median(b, DATA_NUM);
  return true;
}

// ============================================================
// 1 サイクル分の計測結果（グローバル: 送信・SD記録で参照）
// ============================================================

int ch[4], tempV, battV;

// CH_ASSIGN に従い 4 スロット分を計測し、温度・電池を取得
static void measureAll() {
#if DEBUG_MODE
  Serial.println("----- MEASURE START -----");
#endif
  statusMeasureGreen();

  for (int i = 0; i < 4; i++) {
    if (CH_ASSIGN[i] == 1) {
      if ((s_errors & ERR_TCA9534_I2C) != 0U) {
        ch[i] = 0;
      } else {
        hxBegin((uint8_t)(i + 1));
        hxRead(&ch[i]);
        ch[i] = (int)((float)ch[i] / STRAIN_SCALE);  // 生値 → ひずみ値（με）
      }
    } else {
      // CH_ASSIGN 2(MPU)/3(DS18B20) は v3.04 では未対応（ビットバン再実装が必要）
      ch[i] = 0;
    }
#if DEBUG_MODE
    Serial.print("[CH"); Serial.print(i + 1); Serial.print("] "); Serial.println(ch[i]);
#endif
  }

  tempV = measureTemp();
  battV = measureBatt();

#if USE_DS3231_TIMESTAMP && DEBUG_MODE
  {
    Ds3231Time ts;
    if (ds3231GetTime(ts)) {
      char tsbuf[20];
      snprintf(tsbuf, sizeof(tsbuf), "%04u-%02u-%02u %02u:%02u:%02u",
               (unsigned)ts.year, (unsigned)ts.month, (unsigned)ts.day,
               (unsigned)ts.hour, (unsigned)ts.min, (unsigned)ts.sec);
      Serial.print("[DS3231] "); Serial.println(tsbuf);
    } else {
      Serial.println("[DS3231] time read error");
    }
  }
#endif

#if DEBUG_MODE
  Serial.print("[TEMP(DS3231)] ");
  Serial.print(tempV / 10); Serial.print("."); Serial.print(abs(tempV % 10));
  Serial.println(" degC");
  Serial.print("[BATT] "); Serial.println(battV);
#endif
}

// ============================================================
// SD カード CSV 記録・ダンプ・消去
// ============================================================

// DS3231 の現在時刻を "YYYY-MM-DD HH:MM:SS" で buf に書く（読み取り失敗時はゼロ日時）
static void getTimestamp(char *buf, size_t len) {
  Ds3231Time t;
  if (ds3231GetTime(t)) {
    snprintf(buf, len, "%04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
             (unsigned)t.hour, (unsigned)t.min, (unsigned)t.sec);
  } else {
    snprintf(buf, len, "0000-00-00 00:00:00");
  }
}

// 1 サイクル分（timestamp,ch1..4,temp,batt）を /log.csv に追記
static void logRecord() {
  if (!ENABLE_LOGGING || !s_sdReady) return;
  FsFile f = sd.open(LOG_FILE, O_WRITE | O_APPEND);
  if (!f) {
#if DEBUG_MODE
    Serial.println("[SD] open failed");
#endif
    return;
  }
  char ts[24];
  getTimestamp(ts, sizeof(ts));
  f.print(ts);
  for (int i = 0; i < 4; i++) { f.print(","); f.print(ch[i]); }
  f.print(","); f.print(tempV);
  f.print(","); f.println(battV);
  f.close();
#if DEBUG_MODE
  Serial.print("[SD] logged: "); Serial.println(ts);
#endif
}

#if DEBUG_MODE
// SD ログを全件シリアル出力（'d' コマンド）
static void dumpLog() {
  if (!s_sdReady) { Serial.println("[LOG] SD not ready"); return; }
  FsFile f = sd.open(LOG_FILE, O_READ);
  if (!f) { Serial.println("[LOG] no file"); return; }
  Serial.println("=== LOG DUMP ===");
  while (f.available()) Serial.write(f.read());
  f.close();
  Serial.println("=== END ===");
}

// SD ログを削除（'e' コマンド）
static void eraseLog() {
  if (!s_sdReady) { Serial.println("[LOG] SD not ready"); return; }
  if (sd.exists(LOG_FILE)) { sd.remove(LOG_FILE); Serial.println("[LOG] erased"); }
  else                     { Serial.println("[LOG] no file"); }
}

// シリアルコマンド（d=dump / e=erase）を1文字ポーリング
static void pollSerialCommands() {
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'd': dumpLog(); break;
      case 'e': eraseLog(); break;
      default: break;
    }
  }
}
#endif  // DEBUG_MODE

// ============================================================
// Sigfox ペイロード用: int を 4 桁十六進（LSB first）
// ============================================================

String hx4(int v) {
  uint16_t u = (uint16_t)(int16_t)v;
  char b[5];
  snprintf(b, 5, "%02X%02X", (uint8_t)(u & 0xFF), (uint8_t)(u >> 8));
  return String(b);
}

// ============================================================
// Sigfox UART（Serial1）
// ============================================================

static String sendAT(String cmd, int waitMs = 2000) {
#if DEBUG_MODE
  Serial.print(">> "); Serial.println(cmd);
#endif
  Serial1.print(cmd + "\r");
  long start = millis();
  String response = "";
  while (millis() - start < (unsigned long)waitMs) {
    statusSigfoxBlinkTick();
    while (Serial1.available()) response += (char)Serial1.read();
    yield();
  }
#if DEBUG_MODE
  Serial.println(response);
#endif
  return response;
}

// ch[0..3]・温度・電池を連結した 16 進ペイロードで AT$SF= を送信
static void sendSigfox() {
  if (s_errors != 0U) {
#if DEBUG_MODE
    Serial.println("[SIGFOX] skipped (errors)");
#endif
    return;
  }

#if DEBUG_MODE && DEBUG_NO_SIGFOX
  {
    String payload = "";
    for (int i = 0; i < 4; i++) payload += hx4(ch[i]);
    payload += hx4(tempV);
    payload += hx4(battV);
    Serial.print("[SIGFOX] TX SKIPPED (DEBUG_NO_SIGFOX): AT$SF=");
    Serial.println(payload);
  }
  return;
#endif

  // モジュール準備完了待ち（AT ping）。OK が返るまで最大 10 秒
  {
    bool ready = false;
    unsigned long t0 = millis();
    while (millis() - t0 < 10000UL) {
      if (sendAT("AT", 1000).indexOf("OK") >= 0) { ready = true; break; }
#if DEBUG_MODE
      Serial.println("[SIGFOX] waiting for module ready...");
#endif
    }
    if (!ready) {
#if DEBUG_MODE
      Serial.println("[SIGFOX] module not ready: TX skipped (will retry next cycle)");
#endif
      return;  // エラーフラグは立てない
    }
  }

  String payload = "";
  for (int i = 0; i < 4; i++) payload += hx4(ch[i]);
  payload += hx4(tempV);
  payload += hx4(battV);
  String cmd = "AT$SF=" + payload;

#if DEBUG_MODE
  Serial.print("[SIGFOX] "); Serial.println(cmd);
#endif

  statusSigfoxBlinkReset();
  String result = sendAT(cmd, 10000);

  if (result.indexOf("OK") >= 0) {
#if DEBUG_MODE
    Serial.println("[SIGFOX] 送信成功");
#endif
  } else {
    s_errors |= ERR_SIGFOX_AT;
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[SIGFOX] 送信失敗");
#endif
  }
}

// ============================================================
// BLE アドバタイズ（COMM_MODE_BLE 時のみ）
//   MSD 16 バイト: [0-1]CompanyID FFFF /[2]PktType 04 /[3]DeviceID
//                  /[4-11]CH1..4 int16 LE /[12-13]BATT u16 LE /[14]Hour /[15]Min
// ============================================================
#ifdef COMM_MODE_BLE
static void bleAdvertise(uint8_t hour, uint8_t min_val) {
  uint8_t buf[16];
  buf[0] = 0xFF; buf[1] = 0xFF;
  buf[2] = 0x04;            // Pkt type: Monita Flex v3.04
  buf[3] = DEVICE_ID;
  for (int i = 0; i < 4; i++) {
    int16_t v = (int16_t)ch[i];
    buf[4 + i * 2]     = (uint8_t)(v & 0xFF);
    buf[4 + i * 2 + 1] = (uint8_t)((v >> 8) & 0xFF);
  }
  uint16_t batt = (uint16_t)battV;
  buf[12] = (uint8_t)(batt & 0xFF);
  buf[13] = (uint8_t)((batt >> 8) & 0xFF);
  buf[14] = hour;
  buf[15] = min_val;

  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addManufacturerData(buf, sizeof(buf));
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.setInterval(1600, 1600);  // 1000ms 間隔
  Bluefruit.Advertising.setFastTimeout(0);
  Bluefruit.Advertising.start(0);

#if DEBUG_MODE
  Serial.print("[BLE] アドバタイズ開始 ID=0x"); Serial.print(DEVICE_ID, HEX);
  Serial.print(" CH=");
  for (int i = 0; i < 4; i++) { Serial.print(ch[i]); Serial.print(" "); }
  Serial.print("BATT="); Serial.print(battV);
  Serial.print(" TIME="); Serial.print(hour); Serial.print(":"); Serial.println(min_val);
#endif
}
#endif  // COMM_MODE_BLE

// ============================================================
// スリープ（RTC2 + __WFI）
// ============================================================

#define RTC2_PRESCALER 4095U
#define RTC2_TICKS_PER_SECOND 8U
#define RTC2_COUNTER_MASK 0x00FFFFFFU

static volatile bool s_rtc2Compare0Wake;

extern "C" void RTC2_IRQHandler(void) {
  if (NRF_RTC2->EVENTS_COMPARE[0]) {
    NRF_RTC2->EVENTS_COMPARE[0] = 0;
    (void)NRF_RTC2->EVENTS_COMPARE[0];
    NRF_RTC2->INTENCLR = RTC_INTENCLR_COMPARE0_Msk;
    NRF_RTC2->TASKS_STOP = 1;
    s_rtc2Compare0Wake = true;
  }
}

// 周辺（3V3_SW）をオフにし、RTC2 で minutes 分待ってから復帰する。
static void deepSleep(uint32_t minutes) {

#if DEBUG_MODE && DEBUG_NO_SLEEP
  Serial.println("[Sleep SKIPPED (DEBUG_NO_SLEEP)]");
  delay(3000);
  return;
#endif

#if DEBUG_MODE
  Serial.println("[Sleep before wait]");
  delay(5000);
#endif

  rgbOff();

  // スリープ中は周辺 IC 用レールをオフ（3V3_SW = TCA9534 P2）
  tca9534SetBit(2, 0);
#ifdef COMM_MODE_SIGFOX
  Serial1.end();
#endif

#if DEBUG_MODE
  Serial.print("[RTC2 sleep] "); Serial.print(minutes); Serial.println(" min");
  Serial.flush();
#endif

  if (minutes == 0U) minutes = 1U;

  uint64_t ticks64 = (uint64_t)minutes * 60ULL * (uint64_t)RTC2_TICKS_PER_SECOND;
  if (ticks64 > RTC2_COUNTER_MASK) ticks64 = RTC2_COUNTER_MASK;
  if (ticks64 < 1ULL) ticks64 = 1ULL;
  const uint32_t ticks = (uint32_t)ticks64;

  s_rtc2Compare0Wake = false;

  NRF_RTC2->TASKS_STOP = 1;
  NRF_RTC2->TASKS_CLEAR = 1;
  NRF_RTC2->PRESCALER = RTC2_PRESCALER;
  NRF_RTC2->EVTENCLR = 0xFFFFFFFFU;
  NRF_RTC2->EVENTS_COMPARE[0] = 0;
  (void)NRF_RTC2->EVENTS_COMPARE[0];
  NRF_RTC2->CC[0] = ticks;
  NRF_RTC2->INTENCLR = 0xFFFFFFFFU;
  NRF_RTC2->INTENSET = RTC_INTENSET_COMPARE0_Msk;

  NVIC_SetPriority(RTC2_IRQn, 7);
  NVIC_ClearPendingIRQ(RTC2_IRQn);
  NVIC_EnableIRQ(RTC2_IRQn);
  NRF_RTC2->TASKS_START = 1;

  s_btnFlag = false;

  while (!s_rtc2Compare0Wake) {
    __DSB();
    __WFI();

    if (s_btnFlag) {
      s_btnFlag = false;
      delay(20);
      if (digitalRead(USER_BUTTON_PIN) == LOW) {
        uint32_t startTick = NRF_RTC2->COUNTER;
        const uint32_t longPressTicks = (BUTTON_LONG_PRESS_MS / 1000UL) * RTC2_TICKS_PER_SECOND;
        bool longPress = false;
        while (digitalRead(USER_BUTTON_PIN) == LOW) {
          uint32_t elapsed = (NRF_RTC2->COUNTER - startTick) & RTC2_COUNTER_MASK;
          if (elapsed >= longPressTicks) { longPress = true; break; }
        }
        if (longPress) {
          s_pendingTare = true;
          NRF_RTC2->TASKS_STOP = 1;
          NRF_RTC2->INTENCLR = 0xFFFFFFFFU;
          NVIC_DisableIRQ(RTC2_IRQn);
#if DEBUG_MODE
          Serial.println("[BTN] long press -> tare pending");
          Serial.flush();
#endif
          return;
        } else {
#if DEBUG_MODE
          Serial.println("[BTN] short press -> reset");
          Serial.flush();
#endif
          NVIC_SystemReset();
        }
      } else {
#if DEBUG_MODE
        Serial.println("[BTN] short press (released) -> reset");
        Serial.flush();
#endif
        NVIC_SystemReset();
      }
    }
  }

  NVIC_DisableIRQ(RTC2_IRQn);
  NRF_RTC2->INTENCLR = RTC_INTENCLR_COMPARE0_Msk;
  NRF_RTC2->TASKS_STOP = 1;
}

// ============================================================
// 1 サイクル共通処理（計測 → SD記録 → 送信）
// ============================================================

static void runCycle() {
  measureAll();
  logRecord();  // 毎サイクル SD へ CSV 追記

#ifdef COMM_MODE_SIGFOX
  sendSigfox();
  deepSleep(SLEEP_MINUTES);
#endif

#ifdef COMM_MODE_BLE
  {
    Ds3231Time rtcT = {};
    bool rtcOk = ds3231GetTime(rtcT);
#if TEST_ADV_MODE
    bleAdvertise(rtcOk ? rtcT.hour : 0, rtcOk ? rtcT.min : 0);
    delay(10000);
#else
    if (rtcOk && rtcT.min <= ADV_TRIGGER_MIN) {
      bleAdvertise(rtcT.hour, rtcT.min);
      uint32_t advMs = (uint32_t)ADV_DURATION_MIN * 60000UL;
      uint32_t t0 = millis();
      while (millis() - t0 < advMs) { delay(1000); yield(); }
      Bluefruit.Advertising.stop();
    }
    deepSleep(MEASURE_INTERVAL_MIN);
#endif
  }
#endif
}

// ============================================================
// Arduino エントリ
// ============================================================

void setup() {

  rgbHwBegin();
  rgbOff();

  pinMode(USER_BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(USER_BUTTON_PIN), btnISR, FALLING);

  statusBootBlueStrong();
  delay((unsigned long)BOOT_BLUE_MS);
  statusIdleBlueDim();

#if DEBUG_MODE
  Serial.begin(115200);
  delay(2000);  // USB CDC 安定待ち
  Serial.println("=== Monita Flex v3.04 (計測+SD+送信) DEBUG START ===");
  Serial.println("    Commands: d=dump log  e=erase log");
  Serial.flush();
#endif

#ifdef COMM_MODE_SIGFOX
  Serial1.setPins(SIGFOX_RX_PIN, SIGFOX_TX_PIN);
  Serial1.begin(SIGFOX_BAUD);
  delay(3000);  // BRKLSM100 コールドスタート待ち
#endif

#ifdef COMM_MODE_BLE
  Bluefruit.begin();
  char bleName[16];
  snprintf(bleName, sizeof(bleName), "Monita-%02X", DEVICE_ID);
  Bluefruit.setName(bleName);
#if DEBUG_MODE
  Serial.println("[BLE] init OK");
#endif
#endif

  // ── I2C（ビットバン）→ TCA9534（3V3_SW ON / CS アサート）──
  i2cInit();
  analogReadResolution(12);

  s_errors = ERR_NONE;

#if DS3231_SET_TIME
  {
    uint8_t yr2 = (uint8_t)((DS3231_INIT_YEAR) % 100U);
    bool ok = ds3231SetTime(yr2, (uint8_t)(DS3231_INIT_MONTH), (uint8_t)(DS3231_INIT_DAY),
                            (uint8_t)(DS3231_INIT_HOUR), (uint8_t)(DS3231_INIT_MIN),
                            (uint8_t)(DS3231_INIT_SEC));
#if DEBUG_MODE
    Serial.println(ok ? "[DS3231] time set OK" : "[DS3231] time set FAILED");
#endif
    if (!ok) s_errors |= ERR_DS3231_I2C;
  }
#endif

  if (!tca9534Init()) {
    s_errors |= ERR_TCA9534_I2C;
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[TCA9534] init failed");
#endif
  }

  // ── SD カード初期化（3V3_SW ON 後 / SCK=LOW,MOSI=HIGH 確定後）──
  sdPrePinConfig();
  s_sdReady = initSd();

  loadTareOffsets();

  runCycle();
}

void loop() {

  statusIdleBlueDim();

#if DEBUG_MODE
  pollSerialCommands();  // d=dump / e=erase（スリープなしデバッグ時に使用）
#endif

  // 3V3_SW 再投入（TCA9534 P2）。cold boot 対策込みで tca9534Init を再実行。
  s_errors = ERR_NONE;
  if (!tca9534Init()) {
    s_errors |= ERR_TCA9534_I2C;
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[TCA9534] re-init failed");
#endif
  }

  // SD はスリープ中に電源断されるため毎サイクル再初期化
  sdPrePinConfig();
  s_sdReady = initSd();

#ifdef COMM_MODE_SIGFOX
  Serial1.setPins(SIGFOX_RX_PIN, SIGFOX_TX_PIN);
  Serial1.begin(SIGFOX_BAUD);
  delay(3000);  // BRKLSM100 再起動待ち
#endif

#if DEBUG_MODE
  Serial.println("[WAKE]");
#endif

  // ── ボタン処理 ──
  if (s_btnFlag) {
    s_btnFlag = false;
    delay(20);
    if (digitalRead(USER_BUTTON_PIN) == LOW) {
      unsigned long pressStart = millis();
      bool longPress = false;
      while (digitalRead(USER_BUTTON_PIN) == LOW) {
        if (millis() - pressStart >= BUTTON_LONG_PRESS_MS) { longPress = true; break; }
      }
      if (longPress) {
        s_pendingTare = true;
#if DEBUG_MODE
        Serial.println("[BTN] long press -> tare pending");
#endif
      } else {
#if DEBUG_MODE
        Serial.println("[BTN] short press -> reset");
#endif
        NVIC_SystemReset();
      }
    } else {
#if DEBUG_MODE
      Serial.println("[BTN] short press (released) -> reset");
#endif
      NVIC_SystemReset();
    }
  }

  if (s_pendingTare) {
    s_pendingTare = false;
    performTare();
  }

  runCycle();
}
