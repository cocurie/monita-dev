/**
 * Monita Flex v3.3 — 村本建設 Phase1 現場実証ファーム
 * （ベース: v3.03 Sigfox スケッチ + XIAO Sense 傾き・IC温度追加）
 *
 * 【対象ハード】
 *   Monita Flex v3.3 基板（= v3.03 回路）+ XIAO nRF52840 Sense + SMA100A Sigfox
 *
 * 【役割の概要】
 *   - XIAO 内蔵 LSM6DS3TR-C（0x6A）から傾き θ（N=100 平均）と IC温度を取得
 *   - DS18B20 × 4（CH1〜CH4 コネクタ、MUX 経由 D6）から温度を取得
 *   - Sigfox（UART D8/D9）で AT$SF= にペイロードを載せて送信
 *   - 送信後、3V3_SW を落とし、内蔵 RTC2 で 15 分スリープして繰り返し
 *
 * 【Sigfox ペイロード 12 バイト（little-endian int16 × 6）】
 *   [0-1]  θ    ×0.01°    XIAO 内蔵 LSM6DS3TR-C 傾き（N=100 平均、USB 左向き実装）
 *   [2-3]  T_IC ×0.1°C    XIAO 内蔵 LSM6DS3TR-C IC温度
 *   [4-5]  T1   ×0.1°C    DS18B20 CH1 筐体内底面
 *   [6-7]  T2   ×0.1°C    DS18B20 CH2 筐体内側面①
 *   [8-9]  T3   ×0.1°C    DS18B20 CH3 筐体内側面②
 *   [10-11] T4  ×0.1°C    DS18B20 CH4 土留め壁面
 *
 * 【ハードの正本】
 *   【7】Monita/開発/Flex基板/Monita_Flex_構成_v3.03.md
 *
 * 【スリープの方式】
 *   nRF52840 内蔵 RTC2 ＋ LFCLK（ボード定義の USE_LFRC または USE_LFXO）。
 *   Arduino コアの FreeRTOS が RTC1 を SysTick 相当に使うため、ここでは RTC2 のみ使用。
 *   プリスケーラ 4095 → LFCLK を 32768Hz 名義としたとき 8 tick/秒。
 *
 * 【ステータス LED】
 *   framework の Seeed_XIAO_nRF52840_Sense は LED_RED/GREEN/BLUE（離散 GPIO）。
 *   WS2812 の場合は USE_WS2812_STATUS_LED を有効にしピン・NeoPixel ライブラリを合わせる。
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
 * | ERR_TCA9534_I2C        | TCA9534（4052 A/B 用）の設定／出力レジスタ I²C 失敗 | 同上     | 同上                         |
 * | ERR_SIGFOX_AT          | AT$SF= の応答に OK が含まれない／タイムアウト | 赤・点灯 | 計測は済み→送信スキップ扱い後もそのサイクルは赤→スリープ |
 *
 * 共通ポリシー:
 *   - 本番・DEBUG_MODE とも LED ロジックは同一。
 *   - エラー発生後は **送信をスキップしてスリープ**（計測エラー時は sendSigfox を呼ばない）。
 *   - deepSleep 入場時は **常に青・最低輝度**（睡眠中表示を優先し赤は消す）。
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
// コア同梱。タレオフセットのフラッシュ保存（リセット後も保持）に使用
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
// DS18B20（1-Wire 温度センサ）。CH_ASSIGN[i]=3 のスロットで使用
#include <OneWire.h>
#include <DallasTemperature.h>
// XIAO nRF52840 Sense 内蔵 LSM6DS3TR-C（傾き・IC温度）
#include <LSM6DS3.h>
// USB CDC（Serial）。DEBUG_MODE 時のログ出力に使用
#include <Adafruit_TinyUSB.h>
// ============================================================
// 通信モード: Sigfox 固定（COMM_MODE_SIGFOX）
// ============================================================
#ifndef COMM_MODE_SIGFOX
  #error "platformio.ini の build_flags に -D COMM_MODE_SIGFOX を指定してください"
#endif

// ============================================================
// アプリ設定（ここを主に編集する）
// ============================================================

#define DEBUG_MODE           1        // 1: USB Serial デバッグログ有効。本番は 0
#define DEBUG_NO_SLEEP       0        // 1: deepSleep をスキップして即 loop() に戻る（DEBUG_MODE 1 時のみ有効）
#define DEBUG_NO_SIGFOX      1        // 1: AT$SF= を送らずログだけ出す（デューティサイクル節約）
#define SLEEP_MINUTES        15       // 1サイクル後のスリープ時間（分）
#define BOOT_BLUE_MS         500      // 電源 ON 後の青点灯時間（ms）
#define BUTTON_LONG_PRESS_MS 5000UL  // D0 長押し閾値（ms）: 以上で tare、未満でリセット

// ── DS3231 RTC 設定 ────────────────────────────────────────────
// DS3231_SET_TIME=1 にすると起動時（setup）に以下の時刻を DS3231 に書き込む。
// 書き込み後は必ず 0 に戻してビルドし直すこと（毎起動で時刻が上書きされるため）。
#define DS3231_SET_TIME      0        // 1: 起動時に DS3231 へ時刻を書き込む。書き込み後は 0 に戻す
#define DS3231_INIT_YEAR     2026     // 書き込む年（西暦 4 桁）
#define DS3231_INIT_MONTH    6        // 書き込む月（1〜12）
#define DS3231_INIT_DAY      16        // 書き込む日（1〜31）
#define DS3231_INIT_HOUR     13       // 書き込む時（0〜23、24h 形式）
#define DS3231_INIT_MIN      58        // 書き込む分（0〜59）
#define DS3231_INIT_SEC      0        // 書き込む秒（0〜59）

// USE_DS3231_TIMESTAMP=1 にすると各サイクルで DS3231 の現在時刻を読み出し、
// DEBUG_MODE=1 の場合はシリアルに出力する。
// ※ Sigfox ペイロードは現状 12B 上限に達しているため、タイムスタンプのペイロード
//    組み込みは送信フォーマット再設計後に対応（現バージョンではログ出力のみ）。
#define USE_DS3231_TIMESTAMP 1        // 1: 各サイクルで時刻を読み出す

// ── Sigfox 設定 ────────────────────────────────────────────
#define SIGFOX_TX_PIN 8    // D8: XIAO TX → BRKLSM100 RX
#define SIGFOX_RX_PIN 9    // D9: XIAO RX ← BRKLSM100 TX
#define SIGFOX_BAUD   9600

// ── I2C 加速度センサ設定 ───────────────────────────────────────
// 対応センサ: LSM6DS3 / LSM6DSO / LSM6DSL（SA0 ピンでアドレス切替）
//   SA0 = LOW（GND）  → 0x6A
//   SA0 = HIGH（3V3） → 0x6B ← スキャンで確認済み
// ※ MPU6050 を使う場合: AD0=LOW→0x68（DS3231と競合）, AD0=HIGH→0x69
#define MPU_ADDR 0x6B  // LSM6DS SA0=HIGH
#define XIAO_IMU_ADDR 0x6A  // XIAO内蔵 LSM6DS3TR-C（SA0=GND）

// ── ステータス LED ─────────────────────────────────────────────
// WS2812 を使う場合は 1 にし、NeoPixel のデータピン・個数と lib_deps を設定する。
#define USE_WS2812_STATUS_LED 0

#if USE_WS2812_STATUS_LED
#include <Adafruit_NeoPixel.h>
#define NEOPIXEL_PIN 16    // 実機・データシートで確認して変更
#define NEOPIXEL_COUNT 1
#endif

// 各スロット i（0〜3）が CH(i+1) に相当。
//   1 = HX711（ひずみ・荷重）
//   2 = TCA9546A 経由 I2C センサ（LSM6DS 等の加速度センサなど）
//       ※ DS3231 はオンボード U7（0x68）と競合するため CH 接続不可
//   3 = DS18B20（1-Wire 温度センサ）※ 外部プルアップ 4.7kΩ（3V3_SW → CH pin3）必要
const uint8_t CH_ASSIGN[4] = {3, 3, 3, 3};  // 村本建設: 全スロット DS18B20

// ── ひずみ補正係数（キャリブレーション） ──────────────────────────
//
// 【手順】
//   Step1: STRAIN_SCALE = 1.0f のまま生値をシリアルモニタで確認する
//   Step2: カンチレバー等の既知荷重をかけ、生値の変化量を記録する
//   Step3: STRAIN_SCALE = 生値変化量 / 既知ひずみ[με] で設定する
//
// 【変換式】
//   送信値（με）= HX711生値 / STRAIN_SCALE
//
// 【参考】
//   v3.02 実測値: STRAIN_SCALE = 1110.0f（環境・ゲージにより異なる）
//   ゲージファクター: 金属箔ひずみゲージは通常 2.0
//
// ★ まず生値確認 → キャリブレーション後にこの値を更新する ★
static const float STRAIN_SCALE = 1110.0f;  // 1.0f = 生値そのまま出力（未キャリブレーション）

// HX711 1ch あたりの生サンプル数（中央値をとる前の個数）
#define DATA_NUM 5
// 将来の「同一 ch 複数回平均」等用。現状コードでは未使用。
#define REPEAT_NUM 3

#ifndef PI
#define PI 3.14159265358979323846
#endif

// ============================================================
// ピン番号（Arduino ピン番号 = XIAO の Dx。正本 Monita_Flex_構成_v3.03.md）
// ============================================================

// HX711: PD_SCK=D6, DOUT=D7（SN74LV4052 MUX 経由で各 JP に接続）
#define HX711_SCK_PIN  6
#define HX711_DOUT_PIN 7

// アナログ: 電池分圧=A3、温度センサ=A2（解像度は setup で 12bit 設定）
#define BATT_ANALOG_PIN A3
#define TEMP_ANALOG_PIN A2

// TCA9534（U6）: SN74LV4052 の A/B を I²C で駆動。A0=A1=A2=GND → 0x20
#define TCA9534_ADDR 0x20

// DS3231（U7）: RTC + 温度センサ。I²C アドレスは固定（0x68）
#define DS3231_ADDR  0x68

// D0 = タクトスイッチ（GND ショート、内部プルアップ）/ D1 = 予備 GPIO
#define USER_BUTTON_PIN 0
#define SPARE_GPIO_PIN  1

// D10 = MOSFET_GATE → 3V3_SW ON/OFF（HIGH で周辺レール給電）
#define SW_POWER_PIN 10

// TCA9546A（U6）I²C アドレス（A0〜A2 全て GND → 0x70）
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
  ERR_TCA9534_I2C = 1u << 4,
  ERR_DS3231_I2C = 1u << 5,   // DS3231 I²C 通信失敗（温度・時刻読み出し／書き込み）
};

static uint32_t s_errors;

// D0 ボタン: ISR からメインへ「押下エッジあり」を伝えるフラグ
static volatile bool s_btnFlag = false;
// deepSleep() がタイマー満了前にボタン長押しで抜けたとき、次の loop() で tare を実行する
static bool s_pendingTare = false;

static void btnISR() {
  s_btnFlag = true;
}

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

// r,g,b は 0〜255。輝度は全体スケール（スリープ時の青は brightness 低めで指定）。
// XIAO nRF52840 の RGB LED はアクティブ LOW（LED_STATE_ON=0）のため値を反転する。
// analogWrite(pin, 0)=点灯 / analogWrite(pin, 255)=消灯
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
#define RGB_BRIGHT_SLEEP_BLUE 16 // スリープ時の青（できるだけ低輝度）

static void rgbOff() {
#if USE_WS2812_STATUS_LED
  rgbHwShow(0, 0, 0, 1);
#else
#ifdef LED_RED
  // アクティブ LOW: 255(HIGH) = 消灯
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
  rgbHwShow(0, 0, 255, RGB_BRIGHT_SLEEP_BLUE);
}

static void statusMeasureGreen() {
  rgbHwShow(0, 255, 0, RGB_BRIGHT_FULL);
}

static void statusErrorRed() {
  rgbHwShow(255, 0, 0, RGB_BRIGHT_FULL);
}

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
    if (s_sigfoxBlinkOn)
      rgbHwShow(0, 255, 0, RGB_BRIGHT_FULL);
    else
      rgbOff();
  }
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

#if DEBUG_MODE && DEBUG_NO_SLEEP
  Serial.println("[Sleep SKIPPED (DEBUG_NO_SLEEP)]");
  delay(3000);  // 次サイクルまでの最低待機
  return;
#endif

#if DEBUG_MODE
  // USB 接続でログを見る時間を確保（この間もスリープ表示の青デューム）
  Serial.println("[Sleep before wait]");
  delay(5000);
#endif

  // スリープ中は LED オフ（消費電流削減）
  rgbOff();

  // スリープ中は周辺 IC 用レールをオフ（消費電流削減。正本の 3V3_SW 節）
  digitalWrite(SW_POWER_PIN, LOW);
  // Sigfox モジュール電源オフ後は UART を閉じる（フロートやリーク対策）
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

  // 計測・送信フェーズ中に発生したボタン押下の残りフラグをここで破棄する。
  // スリープ中（WFI ループ内）の押下のみを有効とする。
  s_btnFlag = false;

  while (!s_rtc2Compare0Wake) {
    __DSB();
    __WFI();

    if (s_btnFlag) {
      s_btnFlag = false;
      delay(20); // チャタリング除去
      if (digitalRead(USER_BUTTON_PIN) == LOW) {
        // ボタンがまだ押されている → 長押し判定
        // millis() は FreeRTOS tickless idle の影響を受けるため、
        // すでに動作中の RTC2 カウンタで計時する（確実に 8tick/秒で進む）。
        uint32_t startTick = NRF_RTC2->COUNTER;
        const uint32_t longPressTicks =
            (BUTTON_LONG_PRESS_MS / 1000UL) * RTC2_TICKS_PER_SECOND;
        bool longPress = false;
        while (digitalRead(USER_BUTTON_PIN) == LOW) {
          uint32_t elapsed = (NRF_RTC2->COUNTER - startTick) & RTC2_COUNTER_MASK;
          if (elapsed >= longPressTicks) {
            longPress = true;
            break;
          }
        }
        if (longPress) {
          // 長押し: tare フラグを立てて早期リターン（loop() で実行）
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
          // 短押し: ソフトウェアリセット
#if DEBUG_MODE
          Serial.println("[BTN] short press -> reset");
          Serial.flush();
#endif
          NVIC_SystemReset();
        }
      } else {
        // ボタンはすでに離されている → 短押しとみなしてリセット
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
// TCA9534 — SN74LV4052（U5）の A/B を I²C から駆動
//
// レジスタは TI TCA9534 互換の一般的マップ（Input0 / Output1 / Polarity2 / Config3）。
// ch は 1〜4（物理スロット）。idx 0〜3 で v3.01 相当の A/B 2bit（A=LSB, B=bit1）を TCA の P1/P0 に写像。
// ============================================================

static bool tca9534WriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool tca9534ReadReg(uint8_t reg, uint8_t *out) {
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom(TCA9534_ADDR, (uint8_t)1) != 1)
    return false;
  *out = Wire.read();
  return true;
}

// `3V3_SW` 復帰後も呼べるよう冪等に。失敗時は ERR_TCA9534_I2C。
static bool tca9534Configure() {
  if (!tca9534WriteReg(0x02, 0x00))
    return false;
  // P0,P1 を出力、P2〜P7 を入力（未使用ピンは入力へ）
  if (!tca9534WriteReg(0x03, 0xFC))
    return false;
  uint8_t out = 0;
  if (!tca9534ReadReg(0x01, &out))
    return false;
  out = (uint8_t)((out & (uint8_t)~0x03U) | 0x00U); // A=0,B=0 で MUX を既知状態へ
  return tca9534WriteReg(0x01, out);
}

static void muxSelect(uint8_t ch) {
  uint8_t idx = (ch - 1) & 0x03;
  const uint8_t a = idx & 0x01;
  const uint8_t b = (idx >> 1) & 0x01;
  // P0=B, P1=A → OUT レジスタの bit0=B, bit1=A
  const uint8_t twoBits = (uint8_t)(b | (a << 1));

  uint8_t out = 0;
  if (!tca9534ReadReg(0x01, &out)) {
    s_errors |= ERR_TCA9534_I2C;
    statusErrorRed();
    return;
  }
  out = (uint8_t)((out & (uint8_t)~0x03U) | (twoBits & 0x03U));
  if (!tca9534WriteReg(0x01, out)) {
    s_errors |= ERR_TCA9534_I2C;
    statusErrorRed();
  }
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
// DS18B20（1-Wire 温度センサ）— MUX 経由でピンを共用
//
// 手順: muxSelect(ch) → 4052 が CHx ↔ D6（HX711_SCK_PIN）を接続
//       → D6 上で OneWire プロトコルを実行
// DATA ライン: JP コネクタ pin3（PD_SCK_CHx）= D6（ネットリスト確認済み）
// 変換時間: 12bit = 最大 750ms / 9bit = 最大 94ms（CONVERT_T コマンド発行後）
// ============================================================

static OneWire      s_ow(HX711_SCK_PIN);   // D6 = CH コネクタ pin3（PD_SCK_CHx）
static DallasTemperature s_ds(&s_ow);

// 指定チャネルの DS18B20 を読む。戻り値: 温度×10（例: 253 = 25.3℃）、失敗時 0
static int measureDS18B20(uint8_t ch) {
  muxSelect(ch);
  delay(10);        // MUX 切替安定待ち

  // D6 を意図的に LOW → HIGH → INPUT と遷移させることで DS18B20 に立ち上がりエッジを与える。
  // ※ HX711 操作後は D6 が OUTPUT LOW のままのため HIGH 遷移が機能するが、
  //    初回起動時（D6 がデフォルト INPUT）は LOW→HIGH の遷移が発生せず DS18B20 が応答しない。
  //    明示的に LOW を出してから HIGH にすることで初回でも確実に動作させる。
  pinMode(HX711_SCK_PIN, OUTPUT);
  digitalWrite(HX711_SCK_PIN, LOW);
  delay(10);        // LOW 保持（HX711 操作後と同じ状態を作る）
  digitalWrite(HX711_SCK_PIN, HIGH);
  delay(2);         // HIGH 保持（立ち上がりエッジ確認用）
  pinMode(HX711_SCK_PIN, INPUT);
  delay(200);       // バス安定待ち（外部 4.7kΩ で HIGH に戻るまで）
  s_ds.begin();     // MUX 切替後にバスを再スキャン

  // 念のためリトライ（最大 3 回）
  for (int retry = 0; retry < 3 && s_ds.getDeviceCount() == 0; retry++) {
    delay(500);
    s_ds.begin();
#if DEBUG_MODE
    Serial.print("[DS18B20 CH");
    Serial.print(ch);
    Serial.print("] retry ");
    Serial.println(retry + 1);
#endif
  }

  DeviceAddress addr;
  if (s_ds.getAddress(addr, 0)) {
    // 初回電源ON直後の変換は CRC エラーになる場合がある。
    // 9bit（94ms）でダミー変換を1回行い内部回路を安定させてから本変換を実施する。
    s_ds.setResolution(addr, 9);   // 9bit: 変換 94ms（ダミー用）
    s_ds.requestTemperatures();    // ダミー変換（結果は捨てる）
    s_ds.setResolution(addr, 12);  // 12bit: 変換 750ms（本計測用）
  }

  s_ds.requestTemperatures();  // 本変換

  float t = s_ds.getTempCByIndex(0);
  if (t == DEVICE_DISCONNECTED_C) {
#if DEBUG_MODE
    Serial.print("[DS18B20 CH");
    Serial.print(ch);
    Serial.println("] not found");
#endif
    return 0;
  }

#if DEBUG_MODE
  Serial.print("[DS18B20 CH");
  Serial.print(ch);
  Serial.print("] ");
  Serial.print(t, 1);
  Serial.println(" degC");
#endif

  return (int)(t * 10.0f);
}

// ============================================================
// HX711（ライブラリ 1 インスタンスを MUX 切替で共用）
// ============================================================

HX711 hx;

// チャンネルごとのタレオフセット（HX711 は1インスタンス共用なのでここに保存する）
// hx.tare() のオフセットは1つしか保持できないため、チャンネル切替のたびに set_offset() で復元する
static long s_hx_tare_offset[4] = {0, 0, 0, 0};

// ── タレオフセット フラッシュ保存／復元 ──────────────────────────────
// s_hx_tare_offset は SRAM 変数のため、スリープ中に MCU がリセットされると失われる。
// InternalFS（LittleFS）に書き出すことでリセット後も保持する。

static const char TARE_FILE[] = "/tare.bin";

// タレ実行後に呼ぶ。オフセット配列をフラッシュへ書き込む。
static void saveTareOffsets() {
  if (!InternalFS.begin()) return;
  InternalFS.remove(TARE_FILE);
  File f(InternalFS);
  if (f.open(TARE_FILE, FILE_O_WRITE)) {
    f.write((const uint8_t*)s_hx_tare_offset, sizeof(s_hx_tare_offset));
    f.close();
#if DEBUG_MODE
    Serial.println("[TARE] offsets saved to flash");
#endif
  }
}

// 起動時に呼ぶ。フラッシュからオフセット配列を復元する。
// ファイルが存在しない（初回起動・全消去後）場合は {0,0,0,0} のまま。
static void loadTareOffsets() {
  if (!InternalFS.begin()) return;
  File f(InternalFS);
  if (f.open(TARE_FILE, FILE_O_READ)) {
    if ((size_t)f.size() == sizeof(s_hx_tare_offset)) {
      f.read((uint8_t*)s_hx_tare_offset, sizeof(s_hx_tare_offset));
#if DEBUG_MODE
      Serial.print("[TARE] offsets loaded: ");
      for (int i = 0; i < 4; i++) {
        Serial.print(s_hx_tare_offset[i]);
        if (i < 3) Serial.print(", ");
      }
      Serial.println();
#endif
    }
    f.close();
  }
}

// HX711 全有効チャネルに tare を実行する（3V3_SW ON・Wire 初期化済みの状態で呼ぶこと）
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
    while (!hx.is_ready()) {
      if (millis() - t > 1000) break;
    }
    if (hx.is_ready()) {
      hx.tare();
      s_hx_tare_offset[i] = hx.get_offset();  // チャンネルごとに保存
#if DEBUG_MODE
      Serial.print("[TARE] CH");
      Serial.print(i + 1);
      Serial.print(" done (offset=");
      Serial.print(s_hx_tare_offset[i]);
      Serial.println(")");
#endif
    } else {
#if DEBUG_MODE
      Serial.print("[TARE] CH");
      Serial.print(i + 1);
      Serial.println(" timeout");
#endif
    }
  }
  // タレ完了後にオフセットをフラッシュへ保存（リセット後も保持するため）
  saveTareOffsets();
}

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
  // SCK を LOW に確定してから begin（起動時に SCK が浮くと power-down になる）
  pinMode(HX711_SCK_PIN, OUTPUT);
  digitalWrite(HX711_SCK_PIN, LOW);
  delay(10);
  hx.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
  // チャンネルごとに保存したタレオフセットを復元
  // （hx は1インスタンス共用のため、切替のたびに set_offset() で正しいオフセットに戻す）
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
  for (int i = 0; i < DATA_NUM; i++) {
    // get_value() = read() - tare_offset（タレ補正済み生値）
    // read() だとタレ値が反映されないため get_value() を使う
    b[i] = hx.get_value();
  }

  *out = (int)median(b, DATA_NUM);
  return true;
}

// ============================================================
// DS3231 — RTC ＋ 温度センサ（Wire 直叩き、外部ライブラリ不要）
//
// I²C アドレス: 0x68（固定）
// 温度レジスタ: 0x11(MSB, signed) / 0x12(LSB, bit7:6 = 0.25℃ 分解能)
// 時刻レジスタ: 0x00〜0x06（BCD 形式）
// ============================================================

// BCD ↔ 10進変換
static uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10U + (b & 0x0FU)); }
static uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10U) << 4) | (d % 10U)); }

// DS3231 時刻を保持する構造体
struct Ds3231Time {
  uint8_t  sec;    // 0〜59
  uint8_t  min;    // 0〜59
  uint8_t  hour;   // 0〜23
  uint8_t  day;    // 1〜31
  uint8_t  month;  // 1〜12
  uint16_t year;   // 西暦 4 桁（例: 2026）
};

// DS3231 に時刻を書き込む（DS3231_SET_TIME=1 のときのみ setup で呼ぶ）
// yr2: 西暦下 2 桁（2026 → 26）
// __attribute__((unused)): DS3231_SET_TIME=0 のとき呼ばれないため警告抑制
static bool __attribute__((unused)) ds3231SetTime(uint8_t yr2, uint8_t mo, uint8_t day,
                          uint8_t hr, uint8_t mn, uint8_t sc) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);            // レジスタ先頭（seconds）から書き始める
  Wire.write(dec2bcd(sc));     // 0x00: seconds
  Wire.write(dec2bcd(mn));     // 0x01: minutes
  Wire.write(dec2bcd(hr));     // 0x02: hours（24h 形式）
  Wire.write(0x01);            // 0x03: day of week（使用しないため固定値）
  Wire.write(dec2bcd(day));    // 0x04: date
  Wire.write(dec2bcd(mo));     // 0x05: month（century bit は 0）
  Wire.write(dec2bcd(yr2));    // 0x06: year（下 2 桁）
  return Wire.endTransmission() == 0;
}

// DS3231 から現在時刻を読み出す。成功時 true、I²C 失敗時 false
static bool ds3231GetTime(Ds3231Time &t) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(DS3231_ADDR, (uint8_t)7) < 7) return false;
  t.sec   = bcd2dec(Wire.read() & 0x7FU);   // bit7 = oscillator stop flag（無視）
  t.min   = bcd2dec(Wire.read() & 0x7FU);
  t.hour  = bcd2dec(Wire.read() & 0x3FU);   // bit6=12h/24h bit（24h 固定前提で除く）
  Wire.read();                                // 0x03: day of week（使用しない）
  t.day   = bcd2dec(Wire.read() & 0x3FU);
  t.month = bcd2dec(Wire.read() & 0x1FU);   // bit7 = century bit（除く）
  t.year  = 2000U + bcd2dec(Wire.read());
  return true;
}

// ============================================================
// 温度（DS3231 内蔵センサ。v3.02 の MCP9700 アナログから変更）
//
// 戻り値: 温度×10 を int（例: 253 = 25.3℃）。
// DS3231 温度精度: ±3℃（標準）。基板温度モニタ用途には十分。
// ============================================================

static int measureTemp() {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x11);  // temp MSB レジスタ
  if (Wire.endTransmission(false) != 0) {
    s_errors |= ERR_DS3231_I2C;
#if DEBUG_MODE
    Serial.println("[DS3231] temp read error");
#endif
    return 0;
  }
  if (Wire.requestFrom(DS3231_ADDR, (uint8_t)2) < 2) {
    s_errors |= ERR_DS3231_I2C;
    return 0;
  }
  int8_t  msb = (int8_t)Wire.read();   // 整数部（符号付き 8bit）
  uint8_t lsb = Wire.read();            // 分数部（bit7:6 = 0.25℃ 単位）
  // 温度 = MSB + (lsb >> 6) × 0.25  → ×10 で int 化
  float temp = (float)msb + (float)(lsb >> 6) * 0.25f;
  return (int)(temp * 10.0f);
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
// MPU6050（I2C MPU_ADDR）— 加速度から簡易ピッチ（度×10）
//
// アドレス: MPU_ADDR（デフォルト 0x69 / AD0=HIGH）
// レジスタ 0x3B から加速度 XYZ 各 2byte。姿勢は pitch のみ算出。
//
// 【初期化について】
//   MPU6050 は電源投入直後スリープ状態。レジスタ 0x6B（PWR_MGMT_1）に
//   0x00 を書いてスリープ解除が必要。tcaSelect() 後・measureMPU() 前に
//   mpuWakeup() を呼ぶか、measureMPU() 内で毎回実行する。
//   3V3_SW サイクルごとに電源が切れるため、毎サイクル初期化が必要。
// ============================================================

// LSM6DS 系（LSM6DS3 / LSM6DSO / LSM6DSL）初期化
// 電源投入後またはリセット後に加速度計を有効化する（デフォルト OFF）
// CTRL1_XL (0x10): ODR=104Hz, FS=±2g → 0x40
static bool mpuWakeup() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x10);   // CTRL1_XL: 加速度計設定レジスタ
  Wire.write(0x40);   // ODR_XL=104Hz, FS_XL=±2g, LPF1_BW=ODR/2
  return Wire.endTransmission() == 0;
}

static int measureMPU() {
  // 毎回初期化（3V3_SW サイクルで電源断されるため）
  if (!mpuWakeup()) {
    s_errors |= ERR_MPU_I2C;
    statusErrorRed();
#if DEBUG_MODE
    Serial.print("[LSM6] init failed (addr=0x");
    Serial.print(MPU_ADDR, HEX);
    Serial.println(")");
#endif
    return 0;
  }
  delay(20);  // ODR=104Hz → 初回変換完了まで約10ms、余裕を持って20ms

  // OUTX_L_A (0x28) から加速度 XYZ 各 2byte（リトルエンディアン）
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x28);
  if (Wire.endTransmission(false) != 0) {
    s_errors |= ERR_MPU_I2C;
    statusErrorRed();
    return 0;
  }
  uint8_t n = Wire.requestFrom(MPU_ADDR, (uint8_t)6);
  if (n < 6) {
    s_errors |= ERR_MPU_I2C;
    statusErrorRed();
    return 0;
  }

  // LSM6DS はリトルエンディアン（LSB first）
  int16_t ax = (int16_t)(Wire.read() | Wire.read() << 8);
  int16_t ay = (int16_t)(Wire.read() | Wire.read() << 8);
  int16_t az = (int16_t)(Wire.read() | Wire.read() << 8);

#if DEBUG_MODE
  Serial.print("[LSM6] ax="); Serial.print(ax);
  Serial.print(" ay="); Serial.print(ay);
  Serial.print(" az="); Serial.println(az);
#endif

  float pitch =
      atan2f((float)ax, sqrtf((float)ay * (float)ay + (float)az * (float)az)) * 180.0f / (float)PI;
  return (int)(pitch * 10);
}

// ============================================================
// 1 サイクル分の計測結果（グローバル: Sigfox 組み立てで参照）
// ============================================================

// ch[0..3]: 各スロットの生値（CH_ASSIGN に応じて HX711 または MPU）
int ch[4], tempV, battV;

// ── XIAO 内蔵 LSM6DS3TR-C（傾き θ + IC 温度）────────────────────────
static LSM6DS3 s_xiao_imu(I2C_MODE, XIAO_IMU_ADDR);
#define XIAO_IMU_N_AVG 100

// USB 左向き実装: 垂直壁面で AY≈-1g → atan2(-AY, AZ) = +90°
// tempV = θ×1000（0.001° 単位）、battV = T_IC×10（0.1℃ 単位）
static void measureXIAO() {
  if (s_xiao_imu.begin() != 0) {
    tempV = 0; battV = 0;
#if DEBUG_MODE
    Serial.println("[XIAO IMU] init failed");
#endif
    return;
  }
  delay(20);
  float ax_sum = 0.0f, ay_sum = 0.0f, az_sum = 0.0f;
  for (int i = 0; i < XIAO_IMU_N_AVG; i++) {
    ax_sum += s_xiao_imu.readFloatAccelX();
    ay_sum += s_xiao_imu.readFloatAccelY();
    az_sum += s_xiao_imu.readFloatAccelZ();
    delay(2);
  }
  float theta = atan2f(-ay_sum / (float)XIAO_IMU_N_AVG,
                        az_sum / (float)XIAO_IMU_N_AVG) * 180.0f / (float)PI;
  float t_ic  = s_xiao_imu.readTempC();
  tempV = (int)(theta * 100.0f);   // 0.01° 単位（int16 範囲: ±327.67°）
  battV = (int)(t_ic  * 10.0f);
#if DEBUG_MODE
  Serial.printf("[XIAO IMU] theta=%.4f deg  T_IC=%.1f C\n", theta, t_ic);
#endif
}

// CH_ASSIGN に従い 4 スロット分を順に計測し、温度・電池を末尾に追加
static void measureAll() {

#if DEBUG_MODE
  Serial.println("----- MEASURE START -----");
#endif

  // 計測フェーズ: 緑点灯（電源 ON の強青 500 ms は setup で済ませ、その後〜計測開始は setup がディム青）
  statusMeasureGreen();

  // ── パス1: DS18B20（CH_ASSIGN=3）を最初に計測 ────────────────────
  // HX711 が D6 を OUTPUT に設定する前に 1-Wire 通信を完了させる。
  // 本番モード（DEBUG_MODE=0）で HX711 → DS18B20 の順に処理すると
  // D6 の状態干渉により DS18B20 が応答しない問題を根本回避する。
  for (int i = 0; i < 4; i++) {
    if (CH_ASSIGN[i] == 3) {
      ch[i] = measureDS18B20((uint8_t)(i + 1));
#if DEBUG_MODE
      Serial.print("[CH");
      Serial.print(i + 1);
      Serial.print("] ");
      Serial.println(ch[i]);
#endif
    }
  }

  // ── パス2: HX711 / I2C を計測 ────────────────────────────────────
  for (int i = 0; i < 4; i++) {

    if (CH_ASSIGN[i] == 1) {
      if ((s_errors & ERR_TCA9534_I2C) != 0U) {
        ch[i] = 0;
      } else {
        // 物理 CH は 1 origin（MUX と正本 JP の対応）
        hxBegin((uint8_t)(i + 1));
        hxRead(&ch[i]);
        // 生値 → ひずみ値（με）変換
        ch[i] = (int)((float)ch[i] / STRAIN_SCALE);
      }
    } else if (CH_ASSIGN[i] == 2) {
      // TCA9546A のチャネル i（0 origin）を選択して MPU を読む
      if (tcaSelect((uint8_t)i) != 0) {
        s_errors |= ERR_TCA_I2C;
        statusErrorRed();
        ch[i] = 0;
      } else {
        ch[i] = measureMPU();
      }
      tcaDisable();
    } else {
      // CH_ASSIGN[i] == 3 はパス1で処理済み → スキップ
      continue;
    }

#if DEBUG_MODE
    Serial.print("[CH");
    Serial.print(i + 1);
    Serial.print("] ");
    Serial.println(ch[i]);
#endif
  }

  measureXIAO();  // θ→tempV, T_IC→battV（村本建設 Phase1）

#if USE_DS3231_TIMESTAMP
  {
    Ds3231Time ts;
    if (ds3231GetTime(ts)) {
#if DEBUG_MODE
      // タイムスタンプをシリアルに出力（YYYY-MM-DD HH:MM:SS 形式）
      char tsbuf[20];
      snprintf(tsbuf, sizeof(tsbuf), "%04u-%02u-%02u %02u:%02u:%02u",
               (unsigned)ts.year, (unsigned)ts.month, (unsigned)ts.day,
               (unsigned)ts.hour, (unsigned)ts.min, (unsigned)ts.sec);
      Serial.print("[DS3231] ");
      Serial.println(tsbuf);
#endif
    } else {
#if DEBUG_MODE
      Serial.println("[DS3231] time read error");
#endif
      s_errors |= ERR_DS3231_I2C;
    }
  }
#endif  // USE_DS3231_TIMESTAMP

#if DEBUG_MODE
  Serial.print("[TEMP(DS3231)] ");
  Serial.print(tempV / 10);
  Serial.print(".");
  Serial.print(abs(tempV % 10));
  Serial.println(" degC");
  Serial.print("[BATT] ");
  Serial.println(battV);
#endif
}

// ============================================================
// Sigfox ペイロード用: int を 4 桁十六進文字列（符号付き 16bit 範囲を uint16 キャスト）
// ============================================================

String hx4(int v) {
  uint16_t u = (uint16_t)(int16_t)v;
  char b[5];
  // リトルエンディアン（LSB first）でバックエンドパーサーに合わせる
  snprintf(b, 5, "%02X%02X", (uint8_t)(u & 0xFF), (uint8_t)(u >> 8));
  return String(b);
}

// ============================================================
// Sigfox UART（Serial1）
// ============================================================

// 改行付きで AT を送り、waitMs ミリ秒の間に届いた応答をすべて読んで返す（ポーリング）。
// Sigfox 送信中は緑点滅をこのループで更新する。
static String sendAT(String cmd, int waitMs = 2000) {

#if DEBUG_MODE
  Serial.print(">> ");
  Serial.println(cmd);
#endif

  Serial1.print(cmd + "\r");

  long start = millis();
  String response = "";

  while (millis() - start < (unsigned long)waitMs) {
    statusSigfoxBlinkTick();
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      response += c;
    }
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
  // デバッグ時のみ: ペイロードをログに出力して送信スキップ
  {
    // 村本建設 Phase1: θ(tempV) + T_IC(battV) + T1-T4(ch[0..3])
    String payload = "";
    payload += hx4(tempV);
    payload += hx4(battV);
    for (int i = 0; i < 4; i++) payload += hx4(ch[i]);
    Serial.print("[SIGFOX] TX SKIPPED (DEBUG_NO_SIGFOX): AT$SF=");
    Serial.println(payload);
  }
  return;
#endif

  // ── モジュール準備完了待ち（AT ping）────────────────────────
  // AT$SF= を投げる前にモジュールが応答できる状態か確認する。
  // OK が返るまで最大 10 秒リトライ。タイムアウト時は送信スキップ（エラーにしない）。
  {
    bool ready = false;
    unsigned long t0 = millis();
    while (millis() - t0 < 10000UL) {
      String r = sendAT("AT", 1000);
      if (r.indexOf("OK") >= 0) {
        ready = true;
        break;
      }
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

  // 村本建設 Phase1 ペイロード: θ(tempV) + T_IC(battV) + T1-T4(ch[0..3])
  String payload = "";
  payload += hx4(tempV);
  payload += hx4(battV);
  for (int i = 0; i < 4; i++) {
    payload += hx4(ch[i]);
  }

  String cmd = "AT$SF=" + payload;

#if DEBUG_MODE
  Serial.print("[SIGFOX] ");
  Serial.println(cmd);
#endif

  // Sigfox データ送信中: 緑点滅（sendAT 内で tick）
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
// Arduino エントリ
// ============================================================

void setup() {

  rgbHwBegin();
  rgbOff();

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);  // 3V3_SW ON

  pinMode(USER_BUTTON_PIN, INPUT_PULLUP);
  pinMode(SPARE_GPIO_PIN,  INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(USER_BUTTON_PIN), btnISR, FALLING);

  statusBootBlueStrong();
  delay((unsigned long)BOOT_BLUE_MS);
  statusIdleBlueDim();

#if DEBUG_MODE
  Serial.begin(115200);
  // monitor_dtr=0 では !Serial が解除されないため while(!Serial) は使わない。
  // USB 列挙後、1秒ごとに起動メッセージを送り続け、モニタが繋がれば確実に受信できるようにする。
  delay(2000);  // USB CDC 安定待ち（モニタが開くまでの猶予）
  Serial.println("=== DEBUG START ===");
  Serial.flush();
#endif

  // nRF UART ピン割当（setPins の引数順: RX, TX）
  Serial1.setPins(SIGFOX_RX_PIN, SIGFOX_TX_PIN);
  Serial1.begin(SIGFOX_BAUD);
  delay(3000);  // BRKLSM100 コールドスタート待ち

  Wire.begin();
  analogReadResolution(12);
  delay(200);

  s_errors = ERR_NONE;

#if DS3231_SET_TIME
  // ── DS3231 時刻書き込み ──────────────────────────────────────
  // DS3231_SET_TIME=1 のときのみ実行。書き込み後は必ず 0 に戻してビルドし直すこと。
  {
    uint8_t yr2 = (uint8_t)((DS3231_INIT_YEAR) % 100U);
    bool ok = ds3231SetTime(yr2,
                            (uint8_t)(DS3231_INIT_MONTH),
                            (uint8_t)(DS3231_INIT_DAY),
                            (uint8_t)(DS3231_INIT_HOUR),
                            (uint8_t)(DS3231_INIT_MIN),
                            (uint8_t)(DS3231_INIT_SEC));
#if DEBUG_MODE
    if (ok) {
      Serial.println("[DS3231] time set OK");
    } else {
      Serial.println("[DS3231] time set FAILED");
    }
#endif
    if (!ok) {
      s_errors |= ERR_DS3231_I2C;
    }
  }
#endif  // DS3231_SET_TIME

  if (!tca9534Configure()) {
    s_errors |= ERR_TCA9534_I2C;
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[TCA9534] init failed");
#endif
  }

  // フラッシュから前回のタレオフセットを復元（リセット後も継続して有効にするため）
  loadTareOffsets();

  measureAll();

  sendSigfox();
  deepSleep(SLEEP_MINUTES);
}

void loop() {

  statusIdleBlueDim();
  digitalWrite(SW_POWER_PIN, HIGH);  // 3V3_SW 再投入

  // スリープ中に Serial1.end() しているため再初期化
  Serial1.setPins(SIGFOX_RX_PIN, SIGFOX_TX_PIN);
  Serial1.begin(SIGFOX_BAUD);
  delay(3000);  // BRKLSM100 再起動待ち

  Wire.begin();

#if DEBUG_MODE
  Serial.println("[WAKE]");
#endif

  s_errors = ERR_NONE;
  if (!tca9534Configure()) {
    s_errors |= ERR_TCA9534_I2C;
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[TCA9534] re-init failed");
#endif
  }
  // ── ボタン処理（DEBUG_NO_SLEEP時 or スリープ中に押されてloop()到達時に処理）──
  if (s_btnFlag) {
    s_btnFlag = false;
    delay(20);  // チャタリング除去
    if (digitalRead(USER_BUTTON_PIN) == LOW) {
      // ボタンがまだ押されている → 長押し判定
      unsigned long pressStart = millis();
      bool longPress = false;
      while (digitalRead(USER_BUTTON_PIN) == LOW) {
        if (millis() - pressStart >= BUTTON_LONG_PRESS_MS) {
          longPress = true;
          break;
        }
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
      // ボタンはすでに離されている（計測中等に押されてloop()到達時に検知）→ 短押しとみなしてリセット
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
  measureAll();

  sendSigfox();
  deepSleep(SLEEP_MINUTES);
}
