/**
 * Monita Flex v3.01 — 計測＋Sigfox 送信スケッチ（PlatformIO / XIAO nRF52840）
 *
 * 【役割の概要】
 *   - HX711 最大 4ch（4052 MUX 経由）または I2C 上 MPU（TCA9546A でバス切替）から荷重／姿勢を取得
 *   - アナログで温度・電池電圧を取得
 *   - Sigfox（UART）で AT$SF= にペイロードを載せて送信
 *   - 送信後、3V3_SW を落とし、内蔵 RTC2 で指定分スリープして繰り返し
 *
 * 【ハードの正本】
 *   【7】Monita/開発/Flex基板/Monita_Flex_構成_v3.01.md
 *   回路図・実装と食い違う場合は回路図を正とする。
 *
 * 【スリープの方式】
 *   nRF52840 内蔵 RTC2 ＋ LFCLK（ボード定義の USE_LFRC または USE_LFXO）。
 *   Arduino コアの FreeRTOS が RTC1 を SysTick 相当に使うため、ここでは RTC2 のみ使用。
 *   プリスケーラ 4095 → LFCLK を 32768Hz 名義としたとき 8 tick/秒。
 *
 * 【ステータス LED】
 *   framework の Seeed_XIAO_nRF52840_Sense は LED_RED/GREEN/BLUE（離散 GPIO・アクティブ LOW）。
 *   WS2812 の場合は USE_WS2812_STATUS_LED を有効にしピン・NeoPixel ライブラリを合わせる。
 *
 * 【LED ステータス一覧】
 *   起動      : 青（強）
 *   待機・起床 : 青（弱）
 *   計測・送信 : 緑（点灯）
 *   エラー    : 赤
 *   スリープ  : 消灯
 */

/*
 * =============================================================================
 * エラー処理一覧（フラグ s_errors／対応 LED／サイクル動作）
 * =============================================================================
 *
 * | ビット／名前           | 発生条件                                   | LED      | その後の動作（本仕様）      |
 * |------------------------|--------------------------------------------|----------|-----------------------------|
 * | ERR_HX711_TIMEOUT      | HX711 が is_ready で 1000 ms 超待ち       | 赤・点灯 | Sigfox は送信しない→スリープ |
 * | ERR_MPU_I2C            | MPU 読みで requestFrom / バイト数不足      | 同上     | 同上                         |
 * | ERR_TCA_I2C            | TCA9546 の endTransmission が非ゼロ（MPU時）| 同上     | 同上                         |
 * | ERR_SIGFOX_AT          | AT$SF= の応答に OK が含まれない／タイムアウト | 赤・点灯 | 計測は済み→送信スキップ扱い後もそのサイクルは赤→スリープ |
 *
 * 共通ポリシー:
 *   - 本番・DEBUG_MODE とも LED ロジックは同一。
 *   - エラー発生後は **送信をスキップしてスリープ**（計測エラー時は sendSigfox を呼ばない）。
 *   - deepSleep 入場時は **LED 消灯**（スリープ中は省電力のため消灯）。
 *
 * （将来追加しやすい例: 電池下限閾値 ERR_BATT_LOW、Wire.begin 後の TCA 応答確認 等）
 * =============================================================================
 */

#include <Arduino.h>
// nRF レジスタ直接操作（RTC2 割り込み・スリープ待ち用）
#include <nrf.h>
#include <math.h>
#include <string.h>
#include <Wire.h>
// 重量センサ用 ADC ブリッジ
#include <HX711.h>
// コア同梱。将来 LittleFS 等で使う場合に備えインクルードのみ（本スケッチでは未使用でも可）
#include <InternalFileSystem.h>
// USB CDC（Serial）。DEBUG_MODE 時のログ出力に使用
#include <Adafruit_TinyUSB.h>

// ============================================================
// アプリ設定（ここを主に編集する）
// ============================================================

// 1: USB Serial でデバッグログを出す。本番では 0 にするとログ待ち等がなくなる。
#define DEBUG_MODE 1

// 計測＋Sigfox の 1 サイクル終了後、「スリープ状態」に入る時間（分）。
// 実際の周期は この分 ＋ 起きている間の計測・送信時間。
#define SLEEP_MINUTES 1

// 電源 ON 後の「強い青」の表示時間（ms）
#define BOOT_BLUE_MS 500

// WS2812 を使う場合は 1 にし、NeoPixel のデータピン・個数と lib_deps を設定する。
// Seeed XIAO nRF52840 Sense の Adafruit variant は離散 RGB（LED_RED/GREEN/BLUE）が標準。
#define USE_WS2812_STATUS_LED 0

#if USE_WS2812_STATUS_LED
#include <Adafruit_NeoPixel.h>
#define NEOPIXEL_PIN 16    // 実機・データシートで確認して変更
#define NEOPIXEL_COUNT 1
#endif

// 各スロット i（0〜3）が CH(i+1) に相当。
//   0 = 未使用（ch[i]=0 固定、エラーなし）
//   1 = HX711（ロードセル）
//   2 = TCA 経由 MPU6050（I2C 0x68）→ ピッチ角×10
//   3 = TCA 経由 DS3231（I2C 0x68）→ 温度×10
const uint8_t CH_ASSIGN[4] = {0, 3, 3, 3};

// MCP9700T の温度オフセット補正（℃）。実測との差を加算する。正値で高め補正。
#define TEMP_OFFSET_C 4.5f

// HX711 1ch あたりの生サンプル数（中央値をとる前の個数）
#define DATA_NUM 3
// 将来の「同一 ch 複数回平均」等用。現状コードでは未使用。
#define REPEAT_NUM 3

#ifndef PI
#define PI 3.14159265358979323846
#endif

// ============================================================
// ピン番号（Arduino ピン番号 = XIAO の Dx。正本 Monita_Flex_構成_v3.01.md と対応）
// ============================================================

// HX711: PD_SCK=D6, DOUT=D7（U5 SN74LV4052 経由で各 JP の HX711 に接続）
#define HX711_SCK_PIN 6
#define HX711_DOUT_PIN 7

// アナログ: 電池分圧=D3→A3、温度センサ U4=D2→A2（解像度は setup で 12bit）
#define BATT_ANALOG_PIN A3
#define TEMP_ANALOG_PIN A2

// 4052 の選択線: D1=A, D0=B（正本の「D0=B / D1=A」表記と一致）
#define MUX_A_PIN 1
#define MUX_B_PIN 0

// D10 = MOSFET_GATE → Q2 → Q1 で 3V3_SW の ON/OFF（HIGH で周辺レール給電）
#define SW_POWER_PIN 10

// Sigfox BRKLSM100（U2）: UART。D8=TX→モジュール RX、D9=RX←モジュール TX（正本どおり交差）
#define SIGFOX_TX_PIN 8
#define SIGFOX_RX_PIN 9
#define SIGFOX_BAUD 9600

// TCA9546A（U6）の I2C アドレス（A0〜A2 全て GND のとき 0x70 が一般的）
#define TCA_ADDR 0x70

// ============================================================
// エラーフラグ（ファイル頭コメント「エラー処理一覧」と対応）
// ============================================================

enum : uint32_t {
  ERR_NONE = 0,
  ERR_HX711_TIMEOUT = 1u << 0,
  ERR_MPU_I2C = 1u << 1,
  ERR_TCA_I2C = 1u << 2,
  ERR_SIGFOX_AT = 1u << 3,
  ERR_DS3231_I2C = 1u << 4,
};

static uint32_t s_errors;

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

// r,g,b は 0〜255。輝度は全体スケール。
// XIAO nRF52840 の RGB LED はアクティブ LOW のため値を反転して渡す。
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
#define RGB_BRIGHT_DIM   16

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

static void statusBootBlueStrong() {
  rgbHwShow(0, 0, 255, RGB_BRIGHT_FULL);
}

static void statusIdleBlueDim() {
  rgbHwShow(0, 0, 255, RGB_BRIGHT_DIM);
}

static void statusMeasureGreen() {
  rgbHwShow(0, 255, 0, RGB_BRIGHT_FULL);
}

static void statusErrorRed() {
  rgbHwShow(255, 0, 0, RGB_BRIGHT_FULL);
}

// ============================================================
// スリープ（RTC2 + __WFI）
//
// RTC1 は FreeRTOS カーネル用のためアプリでは触らない。
// 起床は RTC2 の COMPARE0 のみ使用。
// ============================================================

// LFCLK を 32768Hz 名義としたとき: 32768 / (4095+1) = 8 → 1秒あたり 8 ティック
#define RTC2_PRESCALER 4095U
#define RTC2_TICKS_PER_SECOND 8U
// nRF RTC カウンタは実質 24bit（下位のみ有効）
#define RTC2_COUNTER_MASK 0x00FFFFFFU

// COMPARE0 ISR からメインループ側へ「起床した」ことを伝えるフラグ
static volatile bool s_rtc2Compare0Wake;

// RTC2 割り込み: 指定ティック経過でここが走り、待機ループを抜ける
extern "C" void RTC2_IRQHandler(void) {
  if (NRF_RTC2->EVENTS_COMPARE[0]) {
    // nRF ではイベントレジスタは 0 書き込みでクリア。読み戻しは誤割り込み対策として推奨パターン
    NRF_RTC2->EVENTS_COMPARE[0] = 0;
    (void)NRF_RTC2->EVENTS_COMPARE[0];
    NRF_RTC2->INTENCLR = RTC_INTENCLR_COMPARE0_Msk;
    NRF_RTC2->TASKS_STOP = 1;
    s_rtc2Compare0Wake = true;
  }
}

// 周辺（Sigfox・HX711 電源レール 3V3_SW）をオフにし、RTC2 で minutes 分待ってから復帰する。
// 待機中は __WFI で CPU を止める（他割り込みで一時起床し得るが、フラグが立つまでループ継続）。
static void deepSleep(uint32_t minutes) {

#if DEBUG_MODE
  Serial.println("[Sleep before wait]");
  delay(500);
#endif

  // スリープ中は消灯（省電力）
  rgbOff();

  // スリープ中は周辺 IC 用レールをオフ（消費電流削減。正本の 3V3_SW 節）
  digitalWrite(SW_POWER_PIN, LOW);
  // モジュール電源オフ後は UART を閉じる（フロートやリーク対策の一環）
  Serial1.end();

#if DEBUG_MODE
  Serial.print("[RTC2 sleep] ");
  Serial.print(minutes);
  Serial.println(" min");
  Serial.flush();
#endif

  // 0 分指定は誤設定扱いで最低 1 分（CC=0 即時一致を避ける意味もある）
  if (minutes == 0U) {
    minutes = 1U;
  }

  // スリープ時間を RTC ティック数に変換（分 → 秒 → 8tick/秒）
  uint64_t ticks64 =
      (uint64_t)minutes * 60ULL * (uint64_t)RTC2_TICKS_PER_SECOND;
  // 24bit カウンタを超える長さは切り詰め（最大約 24 日相当）
  if (ticks64 > RTC2_COUNTER_MASK) {
    ticks64 = RTC2_COUNTER_MASK;
  }
  if (ticks64 < 1ULL) {
    ticks64 = 1ULL;
  }
  const uint32_t ticks = (uint32_t)ticks64;

  s_rtc2Compare0Wake = false;

  // RTC2 を一度止めてクリアし、今回のスリープ長だけ CC[0] に設定して再スタート
  NRF_RTC2->TASKS_STOP = 1;
  NRF_RTC2->TASKS_CLEAR = 1;
  NRF_RTC2->PRESCALER = RTC2_PRESCALER;
  // 他イベントが有効でも今回は使わないので一括クリア（比較のみ使用）
  NRF_RTC2->EVTENCLR = 0xFFFFFFFFU;

  NRF_RTC2->EVENTS_COMPARE[0] = 0;
  (void)NRF_RTC2->EVENTS_COMPARE[0];

  // COUNTER が 0 から ticks に達したら COMPARE0 イベント
  NRF_RTC2->CC[0] = ticks;
  NRF_RTC2->INTENCLR = 0xFFFFFFFFU;
  NRF_RTC2->INTENSET = RTC_INTENSET_COMPARE0_Msk;

  // 優先度は FreeRTOS API を ISR から呼ばない前提でアプリ側に寄せる（数値はコアの他 IRQ とバランス要確認）
  NVIC_SetPriority(RTC2_IRQn, 7);
  NVIC_ClearPendingIRQ(RTC2_IRQn);
  NVIC_EnableIRQ(RTC2_IRQn);

  NRF_RTC2->TASKS_START = 1;

  while (!s_rtc2Compare0Wake) {
    __DSB();
    __WFI();
  }

  NVIC_DisableIRQ(RTC2_IRQn);
  NRF_RTC2->INTENCLR = RTC_INTENCLR_COMPARE0_Msk;
  NRF_RTC2->TASKS_STOP = 1;
}

// ============================================================
// SN74LV4052（U5）チャネル選択
//
// ch は 1〜4（物理スロット）。idx 0〜3 で A/B の 2bit を駆動。
// ============================================================

static void muxSelect(uint8_t ch) {
  uint8_t idx = (ch - 1) & 0x03;
  // 実機PCBでは 4052 Y1→JP3 / Y2→JP2 のため bit1→A, bit0→B で補正
  digitalWrite(MUX_A_PIN, (idx >> 1) & 0x01);
  digitalWrite(MUX_B_PIN,  idx        & 0x01);
}

// ============================================================
// TCA9546A（U6）— I2C バスを枝 JP 側へ切り替え
//
// ch: 0〜3 が TCA のチャネル番号（本スケッチでは MPU 用ループ index と一致させている）
// ============================================================

static uint8_t tcaSelect(uint8_t ch) {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(1 << ch);
  return Wire.endTransmission();
}

// 全チャネル切断（バスをメイン側だけに戻すイメージ。TCA の仕様上 0x00）
static void tcaDisable() {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(0x00);
  Wire.endTransmission();
}

// ============================================================
// HX711（ライブラリ 1 インスタンスを MUX 切替で共用）
// ============================================================

HX711 hx;

// 小さな配列のバブルソートで中央値（外れ値に強い簡易ロバスト化）
static float median(float *a, int n) {
  float t[5];
  memcpy(t, a, sizeof(float) * (size_t)n);
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - i - 1; j++)
      if (t[j] > t[j + 1]) {
        float x = t[j];
        t[j] = t[j + 1];
        t[j + 1] = x;
      }
  return t[n / 2];
}

// 指定チャネル（1〜4）の HX711 に MUX を合わせてからライブラリ begin
static void hxBegin(uint8_t ch) {

#if DEBUG_MODE
  Serial.print("[HX BEGIN CH");
  Serial.print(ch);
  Serial.println("]");
#endif

  muxSelect(ch);
  // MUX 切替直後の信号安定待ち（正本でも数十 ms 待機の例あり）
  delay(10);
  hx.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
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
  for (int i = 0; i < DATA_NUM; i++) {
    b[i] = hx.read();
  }

  *out = (int)median(b, DATA_NUM);
  return true;
}

// ============================================================
// 温度（U4。典型的には MCP9700 系の V/T 特性を想定した換算）
//
// 戻り値: 温度×10 を int（例: 253 = 25.3℃）。センサ実物に合わせて式は要確認。
// ============================================================

static int measureTemp() {
  int raw = analogRead(TEMP_ANALOG_PIN);
  float v = raw * (3.3f / 4095.0f);
  // MCP9700T: 10mV/℃, 0℃で 500mV
  float t = (v - 0.5f) / 0.01f + TEMP_OFFSET_C;
  return (int)(t * 10.0f);
}

// ============================================================
// 電池電圧（分圧後の ADC を実効電圧に戻す）
//
// 戻り値: mV 相当 int（表示・ペイロード用）。1.8696 は分圧比に応じた係数（回路図 R 値と要整合）。
// ============================================================

static int measureBatt() {
  int raw = analogRead(BATT_ANALOG_PIN);
  float v = raw * (3.3 / 4095.0);
  v *= 1.8696;
  return (int)(v * 1000);
}

// ============================================================
// MPU6050 系（I2C 0x68）— 加速度から簡易ピッチ（度×10）
//
// レジスタ 0x3B から加速度 XYZ 各 2byte。姿勢は pitch のみ算出。
// ============================================================

static int measureMPU() {
#if DEBUG_MODE
  Serial.println("[MPU] beginTransmission 0x68 ...");
  Serial.flush();
#endif
  Wire.beginTransmission(0x68);
  Wire.write(0x3B);
  uint8_t mpuErr = Wire.endTransmission(false);
#if DEBUG_MODE
  Serial.print("[MPU] endTransmission(false)=");
  Serial.println(mpuErr);
  Serial.flush();
#endif
  if (mpuErr != 0) {
    s_errors |= ERR_MPU_I2C;
    statusErrorRed();
    return 0;
  }
#if DEBUG_MODE
  Serial.println("[MPU] requestFrom 0x68 ...");
  Serial.flush();
#endif
  uint8_t n = Wire.requestFrom(0x68, (uint8_t)6);
#if DEBUG_MODE
  Serial.print("[MPU] requestFrom n=");
  Serial.println(n);
  Serial.flush();
#endif
  if (n < 6) {
    s_errors |= ERR_MPU_I2C;
    statusErrorRed();
    return 0;
  }

  int16_t ax = Wire.read() << 8 | Wire.read();
  int16_t ay = Wire.read() << 8 | Wire.read();
  int16_t az = Wire.read() << 8 | Wire.read();

  float pitch =
      atan2f((float)ax, sqrtf((float)ay * (float)ay + (float)az * (float)az)) * 180.0f / (float)PI;
  return (int)(pitch * 10);
}

// ============================================================
// DS3231（I2C 0x68）— 内蔵温度センサから温度×10（度×10）
//
// reg 0x11: 符号付き 8bit 整数部℃
// reg 0x12: bit7-6 が 0.25℃ 単位の小数部（00=0, 01=0.25, 10=0.50, 11=0.75）
// 戻り値: 温度×10 の int（例: 253 = 25.3℃）。MPU の pitch 値と同じ単位。
// ============================================================

// ============================================================
// DS3231（I2C 0x68）— TWIM 直接操作でタイムアウト付き温度読み出し
//
// 問題: Wire.endTransmission() は nRF52840 TWIM が SCL を LOW に
//       残したまま返るため、次の通信がハングする。
// 対策: Wire.end() 前に PSEL レジスタ（NRF ピン番号）を保存し、
//       Wire.end() 後に TWIM ハードを直接再起動してポーリング。
//       100 ms 以内に完了しなければ TASKS_STOP で強制終了する。
// ============================================================

static int measureDS3231() {
#if DEBUG_MODE
  Serial.println("[DS3231] reading temperature ...");
  Serial.flush();
#endif

  // Wire が使っている TWIM インスタンスを PSEL で判別して保存
  NRF_TWIM_Type *twim = nullptr;
  if (NRF_TWIM0->PSEL.SCL != 0xFFFFFFFFu) {
    twim = NRF_TWIM0;
  } else if (NRF_TWIM1->PSEL.SCL != 0xFFFFFFFFu) {
    twim = NRF_TWIM1;
  }

  if (!twim) {
#if DEBUG_MODE
    Serial.println("[DS3231] TWIM not found"); Serial.flush();
#endif
    s_errors |= ERR_DS3231_I2C; statusErrorRed();
    return 0;
  }

  const uint32_t psel_scl = twim->PSEL.SCL;
  const uint32_t psel_sda = twim->PSEL.SDA;

  Wire.end();  // nrfx 経由で TWIM 無効化（ハングなし確認済み）

  // TWIM をハードウェアレベルで直接再有効化（Wire の IRQ ハンドラを経由しない）
  twim->PSEL.SCL   = psel_scl;
  twim->PSEL.SDA   = psel_sda;
  twim->FREQUENCY  = 0x01980000UL;  // 100 kHz
  twim->ADDRESS    = 0x68u;
  twim->ENABLE     = 6u;            // TWIM_ENABLE_ENABLE_Enabled

  // TX: レジスタアドレス 0x11 → RX: 2 バイト（EasyDMA バッファは static で RAM 固定）
  static uint8_t txBuf[1] = {0x11};
  static uint8_t rxBuf[2];
  rxBuf[0] = rxBuf[1] = 0;

  twim->TXD.PTR    = (uint32_t)txBuf;
  twim->TXD.MAXCNT = 1u;
  twim->RXD.PTR    = (uint32_t)rxBuf;
  twim->RXD.MAXCNT = 2u;
  // TX 完了後 RX 開始、RX 完了後 STOP を自動発行
  twim->SHORTS = TWIM_SHORTS_LASTTX_STARTRX_Msk | TWIM_SHORTS_LASTRX_STOP_Msk;
  twim->EVENTS_STOPPED = 0; (void)twim->EVENTS_STOPPED;
  twim->EVENTS_ERROR   = 0; (void)twim->EVENTS_ERROR;

  twim->TASKS_STARTTX = 1;

  // 100 ms タイムアウト付きポーリング
  unsigned long t = millis();
  while (!twim->EVENTS_STOPPED && !twim->EVENTS_ERROR) {
    if (millis() - t > 100u) {
      twim->TASKS_STOP = 1;
      delay(5);
      twim->SHORTS = 0;
      twim->ENABLE = 0;
      Wire.begin(); Wire.setClock(100000);
#if DEBUG_MODE
      Serial.println("[DS3231] TWIM timeout"); Serial.flush();
#endif
      s_errors |= ERR_DS3231_I2C; statusErrorRed();
      return 0;
    }
  }

  twim->SHORTS = 0;
  const bool ok = (!twim->EVENTS_ERROR) && (twim->RXD.AMOUNT == 2u);
  twim->ENABLE = 0;

  // Wire を復元（TCA 操作などに必要）
  Wire.begin();
  Wire.setClock(100000);
  delay(5);

#if DEBUG_MODE
  Serial.print("[DS3231] ok="); Serial.print(ok);
  if (ok) {
    Serial.print("  raw=0x"); Serial.print(rxBuf[0], HEX);
    Serial.print(" 0x");      Serial.println(rxBuf[1], HEX);
  } else { Serial.println(); }
  Serial.flush();
#endif

  if (!ok) { s_errors |= ERR_DS3231_I2C; statusErrorRed(); return 0; }

  int8_t  msb = (int8_t)rxBuf[0];
  uint8_t lsb = rxBuf[1];
  float frac  = ((lsb >> 6) & 0x03u) * 0.25f;
  float tempC = (float)msb + (msb >= 0 ? frac : -frac);
  return (int)(tempC * 10.0f);
}

// ============================================================
// 1 サイクル分の計測結果（グローバル: Sigfox 組み立てで参照）
// ============================================================

// ch[0..3]: 各スロットの生値（CH_ASSIGN に応じて HX711 または MPU）
int ch[4], tempV, battV;

// CH_ASSIGN に従い 4 スロット分を順に計測し、温度・電池を末尾に追加
static void measureAll() {

#if DEBUG_MODE
  Serial.println("----- MEASURE START -----");
  Serial.flush();
#endif

  statusMeasureGreen();

  for (int i = 0; i < 4; i++) {

    if (CH_ASSIGN[i] == 0) {
      ch[i] = 0; // 未使用スロット
    } else if (CH_ASSIGN[i] == 1) {
      // 物理 CH は 1 origin（MUX と正本 JP の対応）
      hxBegin((uint8_t)(i + 1));
      hxRead(&ch[i]);
    } else if (CH_ASSIGN[i] == 2) {
      // MPU は TCA のチャネル i（0 origin）に合わせてバスを開く
#if DEBUG_MODE
      Serial.print("[TCA] select ch");
      Serial.print(i);
      Serial.println(" ...");
      Serial.flush();
#endif
      uint8_t tcaErr = tcaSelect((uint8_t)i);
#if DEBUG_MODE
      Serial.print("[TCA] endTransmission=");
      Serial.println(tcaErr);
      Serial.flush();
#endif
      if (tcaErr != 0) {
        s_errors |= ERR_TCA_I2C;
        statusErrorRed();
        ch[i] = 0;
      } else {
        ch[i] = measureMPU();
      }
      tcaDisable();
    } else if (CH_ASSIGN[i] == 3) {
      // DS3231 温度センサ: TCA のチャネル i（0 origin）に切替
#if DEBUG_MODE
      Serial.print("[TCA] select ch");
      Serial.print(i);
      Serial.println(" for DS3231 ...");
      Serial.flush();
#endif
      uint8_t tcaErr = tcaSelect((uint8_t)i);
#if DEBUG_MODE
      Serial.print("[TCA] endTransmission=");
      Serial.println(tcaErr);
      Serial.flush();
#endif
      if (tcaErr != 0) {
        s_errors |= ERR_TCA_I2C;
        statusErrorRed();
        ch[i] = 0;
      } else {
        ch[i] = measureDS3231();
      }
      tcaDisable();
    }

#if DEBUG_MODE
    Serial.print("[CH");
    Serial.print(i + 1);
    Serial.print("] ");
    Serial.println(ch[i]);
    Serial.flush();
#endif
  }

  tempV = measureTemp();
  battV = measureBatt();

#if DEBUG_MODE
  Serial.print("[TEMP] ");
  Serial.println(tempV);
  Serial.print("[BATT] ");
  Serial.println(battV);
#endif
}

// ============================================================
// Sigfox ペイロード用: int を 4 桁十六進文字列（リトルエンディアン・符号付き 16bit）
// ============================================================

String hx4(int v) {
  uint16_t u = (uint16_t)(int16_t)v;
  char b[5];
  snprintf(b, 5, "%02X%02X", u & 0xFF, (u >> 8) & 0xFF);
  return String(b);
}

// ============================================================
// Sigfox UART（Serial1）
// ============================================================

// 改行付きで AT を送り、waitMs ミリ秒の間に届いた応答をすべて読んで返す（ポーリング）。
// logResponse=false にするとレスポンス行の自動出力を抑制できる（呼び元で独自ログを出す場合）。
static String sendAT(String cmd, int waitMs = 2000, bool logResponse = true) {

#if DEBUG_MODE
  Serial.print(">> ");
  Serial.println(cmd);
#endif

  Serial1.print(cmd + "\r");

  long start = millis();
  String response = "";

  while (millis() - start < (unsigned long)waitMs) {
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      response += c;
    }
    yield();
  }

#if DEBUG_MODE
  if (logResponse) Serial.println(response);
#endif

  return response;
}

// モジュールが AT コマンドに応答するまでポーリング待機。
// 応答が来た時点で即座に次へ進む（固定 delay より速い）。
// 実測: 電源投入から約 1007 ms で安定応答（5サイクル計測）。
// maxMs = 4000 ms（実測最大値の約4倍・安全マージン）。
// 戻り値: true=準備完了、false=maxMs 超過（ERR_SIGFOX_AT をセット）
static bool waitSigfoxReady(unsigned long maxMs = 4000) {
  unsigned long start = millis();

#if DEBUG_MODE
  Serial.print("[SIGFOX] waiting for module");
#endif

  while (millis() - start < maxMs) {
    // 受信バッファをクリアしてから AT を送る
    while (Serial1.available()) Serial1.read();
    Serial1.print("AT\r");

    // 最大 1 秒応答を待つ
    unsigned long t = millis();
    String r = "";
    while (millis() - t < 1000) {
      while (Serial1.available()) r += (char)Serial1.read();
      if (r.indexOf("OK") >= 0) {
#if DEBUG_MODE
        Serial.print(" OK (");
        Serial.print(millis() - start);
        Serial.println(" ms)");
#endif
        return true;
      }
      yield();
    }

#if DEBUG_MODE
    Serial.print(".");
#endif
  }

#if DEBUG_MODE
  Serial.println(" timeout");
#endif
  s_errors |= ERR_SIGFOX_AT;
  statusErrorRed();
  return false;
}

// ch[0..3]・温度・電池を連結した 16 進ペイロードで AT$SF= を送信
static void sendSigfox() {

  if (s_errors != 0U) {
#if DEBUG_MODE
    Serial.println("[SIGFOX] skipped (errors)");
#endif
    return;
  }

  // 無線スタック初期化。応答は AT_ERROR（RC設定済み）が正常なので独自メッセージで表示。
  sendAT("AT$RC", 200, false);
#if DEBUG_MODE
  Serial.println("[AT$RC] radio stack ready");
#endif

  String payload = "";
  for (int i = 0; i < 4; i++) {
    payload += hx4(ch[i]);
  }
  payload += hx4(tempV);
  payload += hx4(battV);

  String cmd = "AT$SF=" + payload;

#if DEBUG_MODE
  Serial.print("[SIGFOX] ");
  Serial.println(cmd);
#endif

  // 計測と同じ緑を維持したまま送信（送信中も緑点灯）
  String result = sendAT(cmd, 25000);

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
// Arduino エントリ
// ============================================================

void setup() {

  rgbHwBegin();
  rgbOff();

  // 先に周辺電源を入れる（HX711・Sigfox・温度・TCA 等が 3V3_SW 側）
  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);

  pinMode(MUX_A_PIN, OUTPUT);
  pinMode(MUX_B_PIN, OUTPUT);

  // 電源 ON: 青・強（500 ms）
  statusBootBlueStrong();
  delay((unsigned long)BOOT_BLUE_MS);

  // Sigfox 起動待ちなど計測前のアイドルは青・最低輝度
  statusIdleBlueDim();

#if DEBUG_MODE
  Serial.begin(115200);
  // USB ホストが CDC を開くまで最大 5 秒待つ（開かなくてもその後は進む）
  unsigned long start = millis();
  while (!Serial && millis() - start < 5000) {
    delay(10);
  }

  Serial.println("=== DEBUG START ===");
#endif

  // nRF の UART ピン割当（コアの setPins: RX, TX の順に注意）
  Serial1.setPins(SIGFOX_RX_PIN, SIGFOX_TX_PIN);
  Serial1.begin(SIGFOX_BAUD);

  // ハード I2C（D4/D5）— 正本の SDA/SCL
  Wire.begin();
  analogReadResolution(12);

  // BRKLSM100 が AT コマンドに応答するまでポーリング（最大 5s）
  if (!waitSigfoxReady()) {
    deepSleep(SLEEP_MINUTES);
    return;
  }

  delay(50);

  s_errors = ERR_NONE;

  measureAll();

  sendSigfox();

  deepSleep(SLEEP_MINUTES);
}

void loop() {

  // 起床直後〜計測前: アイドルはディム青
  statusIdleBlueDim();

  // スリープから戻ったあと、再度周辺レールを有効化
  digitalWrite(SW_POWER_PIN, HIGH);

  // スリープ中は Serial1 を end しているため、ここで UART を再度有効化
  // ポーリングより前に初期化が必要なのでここに置く
  Serial1.setPins(SIGFOX_RX_PIN, SIGFOX_TX_PIN);
  Serial1.begin(SIGFOX_BAUD);

  // スリープ前に Wire を止めていないが、周辺電源復帰後は再初期化しておく方が安全
  Wire.begin();

#if DEBUG_MODE
  Serial.println("[WAKE]");
#endif

  // BRKLSM100 が AT コマンドに応答するまでポーリング（最大 5s）
  if (!waitSigfoxReady()) {
    deepSleep(SLEEP_MINUTES);
    return;
  }

  delay(50);

  s_errors = ERR_NONE;

  measureAll();
  sendSigfox();

  deepSleep(SLEEP_MINUTES);
}
