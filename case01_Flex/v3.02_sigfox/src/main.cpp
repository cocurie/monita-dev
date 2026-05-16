/**
 * Monita Flex v3.02 — 計測＋Sigfox 送信スケッチ（PlatformIO / XIAO nRF52840）
 *
 * 【対象ハード】
 *   Monita Flex **v3.02 基板専用**（4052 の A/B は **TCA9534**、D0 はゼロ点／操作タクト用）。
 *   **v3.01 基板に本ビルドを書き込んでも HX711 経路は成立しない**（I²C に TCA9534 が無い）。
 *
 * 【役割の概要】
 *   - HX711 最大 4ch（4052 MUX 経由）または I2C 上 MPU（TCA9546A でバス切替）から荷重／姿勢を取得
 *   - アナログで温度・電池電圧を取得
 *   - Sigfox（UART）で AT$SF= にペイロードを載せて送信
 *   - 送信後、3V3_SW を落とし、内蔵 RTC2 で指定分スリープして繰り返し
 *
 * 【ハードの正本】
 *   【7】Monita/開発/Flex基板/Monita_Flex_構成_v3.02.md
 *   継承・ピン詳細の確定値は v3.01 正本 [Monita_Flex_構成_v3.01.md] も参照。
 *   回路図・実装と食い違う場合は回路図を正とする。
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

// D0 長押し判定閾値（ms）: これ以上 LOW が続いたら tare、未満で離したらソフトリセット
#define BUTTON_LONG_PRESS_MS 5000UL

// 無線モジュール選択: 0=Sigfox (BRKLSM100/U1)、1=LTE-M (SIM7080G/U7)
// U1 と U7 は UART 共有のため、基板実装モジュールに合わせてどちらか一方のみ選択すること（同時実装禁止）
#define MODULE_TYPE 0

// SIM7080G 設定（MODULE_TYPE == 1 のときのみ使用。各値は要変更）
// ※トークンはコードに平文で残さない運用を推奨（書き込み済みファームは除く）
#define LTE_APN         "iot.1nce.net"       // SIM カードの APN（1NCE の場合。要確認）
#define LTE_SERVER_HOST "your.server.com"    // HTTPS 送信先ホスト名（要変更）
#define LTE_POST_PATH   "/api/v2/write?org=YourOrg&bucket=YourBucket&precision=s"  // POST パス（要変更）
#define LTE_TOKEN       "REPLACE_WITH_YOUR_TOKEN"   // Bearer 認証トークン（要変更）
#define LTE_DEVICE_ID   "monita-flex-01"     // Line Protocol タグ用デバイス ID（要変更）
#define LTE_BAUD        115200               // SIM7080G デフォルトボーレート（Sigfox の 9600 と異なる）

// WS2812 を使う場合は 1 にし、NeoPixel のデータピン・個数と lib_deps を設定する。
// Seeed XIAO nRF52840 Sense の Adafruit variant は離散 RGB（LED_RED/GREEN/BLUE）が標準。
#define USE_WS2812_STATUS_LED 0

#if USE_WS2812_STATUS_LED
#include <Adafruit_NeoPixel.h>
#define NEOPIXEL_PIN 16    // 実機・データシートで確認して変更
#define NEOPIXEL_COUNT 1
#endif

// 各スロット i（0〜3）が CH(i+1) に相当。1=HX711、2=TCA 経由で MPU6050 等（I2C 0x68）
const uint8_t CH_ASSIGN[4] = {1, 1, 1, 1};

// HX711 1ch あたりの生サンプル数（中央値をとる前の個数）
#define DATA_NUM 5
// 将来の「同一 ch 複数回平均」等用。現状コードでは未使用。
#define REPEAT_NUM 3

#ifndef PI
#define PI 3.14159265358979323846
#endif

// ============================================================
// ピン番号（Arduino ピン番号 = XIAO の Dx。正本 Monita_Flex_構成_v3.02.md）
// ============================================================

// HX711: PD_SCK=D6, DOUT=D7（U5 SN74LV4052 経由で各 JP の HX711 に接続）
#define HX711_SCK_PIN 6
#define HX711_DOUT_PIN 7

// アナログ: 電池分圧=D3→A3、温度センサ U4=D2→A2（解像度は setup で 12bit）
#define BATT_ANALOG_PIN A3
#define TEMP_ANALOG_PIN A2

// 4052 の A/B は **TCA9534 の GPIO**（主バス D4/D5 の Wire 上、U6 と並列）。回路図の A0〜A2 で 7bit アドレスを決める。
#define TCA9534_ADDR 0x20  // A0=A1=A2=GND（ネットリスト U6 確認済み）
// 4052 側の対応（リビジョン案）: TCA9534 **P0 → B**、**P1 → A**（`muxSelect` の 2bit 写像）。回路図で変更した場合は `muxSelect` を合わせる。

// D0 = ゼロ点／リセット判読用タクト（GND ショート、内部プルアップ）。長押し・短押しのポリシーは正本 §6（本スケッチでは計測ループ前のスタブのみ）。
#define USER_BUTTON_PIN 0
// D1 = 予備 GPIO（未使用時は入力＋プルアップ）
#define SPARE_GPIO_PIN 1

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
  ERR_TCA9534_I2C = 1u << 4,
  ERR_LTE_AT = 1u << 5,
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
static void rgbHwShow(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness8) {
#ifdef LED_RED
  uint16_t rr = (uint16_t)r * brightness8 / 255;
  uint16_t gg = (uint16_t)g * brightness8 / 255;
  uint16_t bb = (uint16_t)b * brightness8 / 255;
  analogWrite(LED_RED, (int)rr);
  analogWrite(LED_GREEN, (int)gg);
  analogWrite(LED_BLUE, (int)bb);
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
  analogWrite(LED_RED, 0);
  analogWrite(LED_GREEN, 0);
  analogWrite(LED_BLUE, 0);
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

static void statusSleepBlueDim() {
  rgbHwShow(0, 0, 255, RGB_BRIGHT_SLEEP_BLUE);
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

#if DEBUG_MODE
  // USB 接続でログを見る時間を確保（この間もスリープ表示の青デューム）
  Serial.println("[Sleep before wait]");
  delay(5000);
#endif

  // スリープ中表示: 青・最低輝度（エラー赤より優先）
  statusSleepBlueDim();

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

    if (s_btnFlag) {
      s_btnFlag = false;
      delay(20); // チャタリング除去
      if (digitalRead(USER_BUTTON_PIN) == LOW) {
        unsigned long pressStart = millis();
        bool longPress = false;
        while (digitalRead(USER_BUTTON_PIN) == LOW) {
          if (millis() - pressStart >= BUTTON_LONG_PRESS_MS) {
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
// HX711（ライブラリ 1 インスタンスを MUX 切替で共用）
// ============================================================

HX711 hx;

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
#if DEBUG_MODE
      Serial.print("[TARE] CH");
      Serial.print(i + 1);
      Serial.println(" done");
#endif
    } else {
#if DEBUG_MODE
      Serial.print("[TARE] CH");
      Serial.print(i + 1);
      Serial.println(" timeout");
#endif
    }
  }
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
  float v = raw * (3.3 / 4095.0);
  // MCP9700 近似: 10mV/℃, 0℃で 500mV
  float t = (v - 0.5) / 0.01;
  return (int)(t * 10);
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
  Wire.beginTransmission(0x68);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) {
    s_errors |= ERR_MPU_I2C;
    statusErrorRed();
    return 0;
  }
  uint8_t n = Wire.requestFrom(0x68, (uint8_t)6);
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
// 1 サイクル分の計測結果（グローバル: Sigfox 組み立てで参照）
// ============================================================

// ch[0..3]: 各スロットの生値（CH_ASSIGN に応じて HX711 または MPU）
int ch[4], tempV, battV;

// CH_ASSIGN に従い 4 スロット分を順に計測し、温度・電池を末尾に追加
static void measureAll() {

#if DEBUG_MODE
  Serial.println("----- MEASURE START -----");
#endif

  // 計測フェーズ: 緑点灯（電源 ON の強青 500 ms は setup で済ませ、その後〜計測開始は setup がディム青）
  statusMeasureGreen();

  for (int i = 0; i < 4; i++) {

    if (CH_ASSIGN[i] == 1) {
      if ((s_errors & ERR_TCA9534_I2C) != 0U) {
        ch[i] = 0;
        continue;
      }
      // 物理 CH は 1 origin（MUX と正本 JP の対応）
      hxBegin((uint8_t)(i + 1));
      hxRead(&ch[i]);
    } else if (CH_ASSIGN[i] == 2) {
      // MPU は TCA のチャネル i（0 origin）に合わせてバスを開く
      if (tcaSelect((uint8_t)i) != 0) {
        s_errors |= ERR_TCA_I2C;
        statusErrorRed();
        ch[i] = 0;
      } else {
        ch[i] = measureMPU();
      }
      tcaDisable();
    }

#if DEBUG_MODE
    Serial.print("[CH");
    Serial.print(i + 1);
    Serial.print("] ");
    Serial.println(ch[i]);
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
// Sigfox ペイロード用: int を 4 桁十六進文字列（符号付き 16bit 範囲を uint16 キャスト）
// ============================================================

String hx4(int v) {
  char b[5];
  snprintf(b, 5, "%04X", (uint16_t)(int16_t)v);
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
// SIM7080G LTE-M 送信（MODULE_TYPE == 1 のときのみコンパイル）
//
// 実証済み構成（2026-05-02）:
//   MCU: XIAO nRF52840 / モジュール: SIM7080G / SIM: 1NCE IoT SIM
//   UART: 115200bps, \r\n 終端（Sigfox の \r とは異なる）
//   通信: HTTPS POST → InfluxDB Line Protocol（AT+SH系コマンド）
//
// 設定値（上部 #define）を NEXCO 案件の送信先に合わせること。
// ============================================================

#if (MODULE_TYPE == 1)

// SIM7080G 専用 AT 送信（\r\n 終端。Sigfox の sendAT とは別関数）
static String sendATLTE(const String &cmd, int waitMs = 5000) {
#if DEBUG_MODE
  Serial.print(">> ");
  Serial.println(cmd);
#endif
  Serial1.print(cmd + "\r\n");
  long start = millis();
  String response = "";
  while (millis() - start < waitMs) {
    statusSigfoxBlinkTick();
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      response += c;
    }
  }
#if DEBUG_MODE
  Serial.println(response);
#endif
  return response;
}

// ネットワーク初期化（LTE-M モード設定→登録確認→PDP 有効化）
static bool lteInitNetwork() {
  sendATLTE("AT+CNMP=38", 2000);  // LTE-M のみに絞る（NB-IoT 等を除外）
  delay(500);
  sendATLTE("AT+CMNB=1", 2000);   // Cat-M1 選択
  delay(500);
  sendATLTE("AT+CGDCONT=1,\"IP\",\"" LTE_APN "\"", 2000);
  delay(500);

  // ネットワーク登録待ち（最大 60 秒 / 5 秒間隔 × 12 回）
  // +CREG: 0,1=接続 / 0,5=ローミング接続（日本では 5 が正常なことがある）
  bool registered = false;
  for (int i = 0; i < 12; i++) {
    String r = sendATLTE("AT+CREG?", 3000);
    if (r.indexOf("0,1") >= 0 || r.indexOf("0,5") >= 0) {
      registered = true;
      break;
    }
    delay(5000);
  }
  if (!registered) {
#if DEBUG_MODE
    Serial.println("[LTE] network registration timeout");
#endif
    return false;
  }

  // データ接続（GPRS Attach）確認
  if (sendATLTE("AT+CGATT?", 3000).indexOf("+CGATT: 1") < 0) {
#if DEBUG_MODE
    Serial.println("[LTE] CGATT failed");
#endif
    return false;
  }
  delay(3000);

  // PDP コンテキスト有効化
  // すでにアクティブな場合は ERROR が返るが正常。AT+CNACT? で IP 確認が確実。
  sendATLTE("AT+CNACT=0,1", 15000);
  delay(3000);

  if (sendATLTE("AT+CNACT?", 3000).indexOf("0,1") < 0) {
#if DEBUG_MODE
    Serial.println("[LTE] IP address not obtained");
#endif
    return false;
  }

  return true;
}

// InfluxDB Line Protocol 形式で HTTPS POST 送信
// body 例: "monita,device=monita-flex-01 ch1=1234,ch2=5678,..."
static void sendSIM7080G() {

  if (s_errors != 0U) {
#if DEBUG_MODE
    Serial.println("[LTE] skipped (errors)");
#endif
    return;
  }

  // Line Protocol ボディ組み立て
  String body = "monita,device=" LTE_DEVICE_ID " ";
  body += "ch1=" + String(ch[0]);
  body += ",ch2=" + String(ch[1]);
  body += ",ch3=" + String(ch[2]);
  body += ",ch4=" + String(ch[3]);
  body += ",temp=" + String(tempV);
  body += ",batt=" + String(battV);
  int bodyLen = (int)body.length();

#if DEBUG_MODE
  Serial.print("[LTE] body: ");
  Serial.println(body);
#endif

  statusSigfoxBlinkReset();

  // モジュール疎通確認
  if (sendATLTE("AT", 2000).indexOf("OK") < 0) {
    s_errors |= ERR_LTE_AT;
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[LTE] AT failed");
#endif
    return;
  }
  sendATLTE("AT+CPIN?", 2000);

  // ネットワーク初期化
  if (!lteInitNetwork()) {
    s_errors |= ERR_LTE_AT;
    statusErrorRed();
    return;
  }

  // 前回セッション切断（残骸があっても続行）
  sendATLTE("AT+SHDISC", 3000);
  delay(500);

  // SSL 設定
  sendATLTE("AT+CSSLCFG=\"ignorertctime\",1,1", 2000); delay(300);
  sendATLTE("AT+CSSLCFG=\"sslversion\",1,3", 2000);    delay(300);
  sendATLTE("AT+CSSLCFG=\"sni\",1,\"" LTE_SERVER_HOST "\"", 2000); delay(300);
  sendATLTE("AT+SHSSL=1,\"\"", 2000); delay(300);

  // HTTP パラメータ設定
  sendATLTE("AT+SHCONF=\"BODYLEN\",1024", 2000);  delay(300);
  sendATLTE("AT+SHCONF=\"HEADERLEN\",350", 2000); delay(300);
  sendATLTE("AT+SHCONF=\"URL\",\"https://" LTE_SERVER_HOST "\"", 2000); delay(300);

  // HTTPS 接続
  String conn = sendATLTE("AT+SHCONN", 15000);
  if (conn.indexOf("OK") < 0) {
    s_errors |= ERR_LTE_AT;
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[LTE] SHCONN failed");
#endif
    sendATLTE("AT+CNACT=0,0", 5000);
    return;
  }

  // HTTP ヘッダ設定
  sendATLTE("AT+SHCHEAD", 2000); delay(300);
  sendATLTE("AT+SHAHEAD=\"Authorization\",\"Token " LTE_TOKEN "\"", 2000); delay(300);
  sendATLTE("AT+SHAHEAD=\"Content-Type\",\"text/plain; charset=utf-8\"", 2000); delay(300);

  // ボディ送信: AT+SHBOD 後は sendATLTE 経由ではなく Serial1 直書き（実証済み）
  Serial1.print("AT+SHBOD=" + String(bodyLen) + ",5000\r\n");
  delay(2000);        // ">" プロンプト待ち
  Serial1.print(body);  // \r\n なしで raw 送信
  delay(1000);

  // POST 実行（3=POST）。204 No Content = InfluxDB 書き込み成功、200 OK も受け入れ
  String result = sendATLTE("AT+SHREQ=\"" LTE_POST_PATH "\",3", 15000);

  sendATLTE("AT+SHDISC", 3000);
  sendATLTE("AT+CNACT=0,0", 5000);

  if (result.indexOf(",204,") >= 0 || result.indexOf(",200,") >= 0) {
#if DEBUG_MODE
    Serial.println("[LTE] POST 成功");
#endif
  } else {
    s_errors |= ERR_LTE_AT;
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[LTE] POST 失敗: " + result);
#endif
  }
}

#endif  // MODULE_TYPE == 1

// ============================================================
// sendData() — MODULE_TYPE に応じて Sigfox または LTE-M を呼び分ける
// ============================================================

static void sendData() {
#if (MODULE_TYPE == 0)
  sendSigfox();
#elif (MODULE_TYPE == 1)
  sendSIM7080G();
#endif
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

  pinMode(USER_BUTTON_PIN, INPUT_PULLUP);
  pinMode(SPARE_GPIO_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(USER_BUTTON_PIN), btnISR, FALLING);

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
#if (MODULE_TYPE == 0)
  Serial1.begin(SIGFOX_BAUD);
  delay(3000);  // BRKLSM100 コールドスタート待ち
#else
  Serial1.begin(LTE_BAUD);
  delay(5000);  // SIM7080G 電源投入後の起動待ち
#endif

  // ハード I2C（D4/D5）— 正本の SDA/SCL
  Wire.begin();
  analogReadResolution(12);

  // 3V3_SW 投入直後は各 IC の起動・デカップ充電に余裕を持たせる
  delay(200);

  s_errors = ERR_NONE;

  if (!tca9534Configure()) {
    s_errors |= ERR_TCA9534_I2C;
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[TCA9534] init failed (check ADDR / wiring / v3.02 board)");
#endif
  }

  measureAll();
  sendData();

  deepSleep(SLEEP_MINUTES);
}

void loop() {

  // 起床直後〜計測前: アイドルはディム青
  statusIdleBlueDim();

  // スリープから戻ったあと、再度周辺レールを有効化
  digitalWrite(SW_POWER_PIN, HIGH);

  // スリープ中は Serial1 を end しているため、ここで UART を再度有効化
  Serial1.setPins(SIGFOX_RX_PIN, SIGFOX_TX_PIN);
#if (MODULE_TYPE == 0)
  Serial1.begin(SIGFOX_BAUD);
  delay(3000);  // BRKLSM100 再起動待ち
#else
  Serial1.begin(LTE_BAUD);
  delay(5000);  // SIM7080G 再起動待ち
#endif

  // スリープ前に Wire を止めていないが、周辺電源復帰後は再初期化しておく方が安全
  Wire.begin();

#if DEBUG_MODE
  Serial.println("[WAKE]");
#endif

  s_errors = ERR_NONE;

  if (!tca9534Configure()) {
    s_errors |= ERR_TCA9534_I2C;
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[TCA9534] re-init after wake failed");
#endif
  }

  if (s_pendingTare) {
    s_pendingTare = false;
    performTare();
  }

  measureAll();
  sendData();

  deepSleep(SLEEP_MINUTES);
}
