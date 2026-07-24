/**
 * Monita Flex v3.10 — 計測＋Sigfox/BLE/LoRa 送信スケッチ（PlatformIO / XIAO nRF52840）
 *
 * 【対象ハード】
 *   Monita Flex **v3.10 基板**（v3.03 をベースに LoRa(E220-900T22S(JP)-EV2) を追加）。
 *   Sigfox・BLE・LoRa はロットごとにビルド時選択（同時実装しない）。
 *   ピン割当は v3.03 と同一。LoRaのM0/M1はTCA9534(U6)のP2拡張GPIO経由で制御する
 *   （v3.10のみの新規配線。基板上でM0とM1を短絡しP2 1本で共通駆動。ver3.10.schネットリストで確認済み）。
 *   v3.02 基板にも書き込み可（ピン割当・I²C 構成は同一。ただしLoRaはP2未配線のため使用不可）。
 *   **v3.01 基板に本ビルドを書き込んでも HX711 経路は成立しない**（I²C に TCA9534 が無い）。
 *
 * 【役割の概要】
 *   - HX711 最大 4ch（4052 MUX 経由）または I2C 上 MPU（TCA9546A でバス切替）から荷重／姿勢を取得
 *   - DS3231 の温度レジスタから基板温度を取得（MCP9700 アナログは廃止）
 *   - 電池電圧をアナログで取得
 *   - Sigfox（UART）で AT$SF= にペイロードを載せて送信、または
 *     BLE アドバタイズで Gateway へ送信、または
 *     LoRa（E220 透過モード、UART は Sigfox と共用ネット）で Gateway へ送信
 *   - 送信後、3V3_SW を落とし、内蔵 RTC2 で指定分スリープして繰り返し
 *
 * 【ハードの正本】
 *   【7】Monita/開発/Flex基板/Monita_Flex_構成_v3.10.md
 *   回路図・実装と食い違う場合は回路図を正とする。
 *
 * 【LoRa(E220-900T22S(JP)) について】
 *   - UART は D8/D9（Sigfoxと共用ネット。ビルド時排他のため物理的に一方しか実装しない）
 *   - M0/M1 は TCA9534(U6) の P2（基板上でM0/M1短絡・共通駆動）。AUXは未接続（固定ディレイで代替）
 *   - 起動毎にチャンネル・アドレス等の設定値を確認し、想定値と異なれば書き込む
 *     （選択肢A方式。詳細は Monita_Flex_構成_v3.10.md §6 参照）
 *   - E220のレジスタビット配置・タイミングの一部は実機データシート未確認の暫定値。
 *     "要確認" とコメントした箇所は実機到着後に見直すこと。
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
 * | ERR_HX711_TIMEOUT      | HX711 が is_ready で 1000 ms 超待ち       | 赤・点灯 | 送信は行わない→スリープ    |
 * | ERR_MPU_I2C            | MPU 読みで requestFrom / バイト数不足      | 同上     | 同上                         |
 * | ERR_TCA_I2C            | TCA9546 の endTransmission が非ゼロ（MPU時）| 同上     | 同上                         |
 * | ERR_TCA9534_I2C        | TCA9534（4052 A/B・LoRa M0/M1 用）の設定／出力レジスタ I²C 失敗 | 同上     | 同上                         |
 * | ERR_SIGFOX_AT          | AT$SF= の応答に OK が含まれない／タイムアウト | 赤・点灯 | 計測は済み→送信スキップ扱い後もそのサイクルは赤→スリープ |
 * | ERR_LORA_CFG           | LoRa設定モードでのREAD/WRITE応答異常       | 赤・点灯 | 送信スキップ→スリープ      |
 *
 * 共通ポリシー:
 *   - 本番・DEBUG_MODE とも LED ロジックは同一。
 *   - エラー発生後は **送信をスキップしてスリープ**（計測エラー時は送信関数を呼ばない）。
 *   - deepSleep 入場時は **常に青・最低輝度**（睡眠中表示を優先し赤は消す）。
 * =============================================================================
 */

#include <Arduino.h>
// nRF レジスタ直接操作（RTC2 割り込み・スリープ待ち用）
#include <nrf.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <Wire.h>
// 重量センサ用 ADC ブリッジ
#include <HX711.h>
// コア同梱。タレオフセットのフラッシュ保存（リセット後も保持）に使用
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
// DS18B20（1-Wire 温度センサ）。CH_ASSIGN[i]=3 のスロットで使用
#include <OneWire.h>
#include <DallasTemperature.h>
// USB CDC（Serial）。DEBUG_MODE 時のログ出力に使用
#include <Adafruit_TinyUSB.h>
// BLE モード時のみ使用
#ifdef COMM_MODE_BLE
#include <bluefruit.h>
#endif

// ============================================================
// 通信モード選択
// platformio.ini の build_flags で指定する:
//   -D COMM_MODE_SIGFOX  … Sigfox 送信モード
//   -D COMM_MODE_BLE     … BLE アドバタイズモード（Gateway 経由でクラウド送信）
//   -D COMM_MODE_LORA    … LoRa 透過送信モード（Gateway 経由でクラウド送信）
// ============================================================
#if !defined(COMM_MODE_SIGFOX) && !defined(COMM_MODE_BLE) && !defined(COMM_MODE_LORA)
  #error "platformio.ini の build_flags に -D COMM_MODE_SIGFOX / -D COMM_MODE_BLE / -D COMM_MODE_LORA のいずれかを指定してください"
#endif

// ============================================================
// BLE モード設定（COMM_MODE_BLE 時のみ有効）
// ============================================================
#ifdef COMM_MODE_BLE
static const uint8_t  DEVICE_ID           = 0x01;  // 子機 ID（複数台時は変える: 0x01〜0xFF）
static const uint8_t  FW_VERSION          = 1;     // 子機ファームのバージョン。コミットのたびに+1すること
static const uint32_t MEASURE_INTERVAL_MIN = 20;   // 計測間隔（分）
static const uint32_t ADV_DURATION_MIN     = 10;   // アドバタイズ継続時間（分）
static const uint8_t  ADV_TRIGGER_MIN      = 2;    // 毎時 :00〜:02 のときアドバタイズ
// 1: 時刻条件・スリープをスキップして常時アドバタイズ。
//    Gatewayとの結線テストのほか、AC電源が確保できる現場での本番運用にも使用する
//    （電池駆動の現場は 0 にしてスリープ運用する）。
#define ADV_CONTINUOUS_MODE  1
#endif

// ============================================================
// LoRa モード設定（COMM_MODE_LORA 時のみ有効）
//
// LoRaはSigfoxと同じ「起床→計測→送信→スリープ」の1回送信サイクルとする
// （BLEのような継続アドバタイズ／時間窓判定は行わない）。
// ============================================================
#ifdef COMM_MODE_LORA
static const uint8_t  DEVICE_ID  = 0x01;  // 子機 ID（複数台時は変える: 0x01〜0xFF）
static const uint8_t  FW_VERSION = 5;     // 子機ファームのバージョン。コミットのたびに+1すること
#endif

// ============================================================
// アプリ設定（ここを主に編集する）
// ============================================================

#define DEBUG_MODE           1        // 1: USB Serial デバッグログ有効。本番は 0 ★ボタン誤作動調査のため一時的に1にしている
#define DEBUG_NO_SLEEP       0        // 1: deepSleep をスキップして即 loop() に戻る（DEBUG_MODE 1 時のみ有効）
#define DEBUG_NO_SIGFOX      0        // 1: AT$SF= を送らずログだけ出す（デューティサイクル節約）
#define DEBUG_NO_LORA        0        // 1: LoRa送信を行わずログだけ出す
#define SLEEP_MINUTES        15       // 1サイクル後のスリープ時間（分）
#define BOOT_BLUE_MS         500      // 電源 ON 後の青点灯時間（ms）
#define BUTTON_LONG_PRESS_MS 5000UL  // D0 長押し閾値（ms）: 以上で tare、未満でリセット

// ── DS3231 RTC 設定 ────────────────────────────────────────────
// 起動時に DS3231 の OSF（発振停止フラグ）を確認し、時刻が無効な場合
// （バックアップ電池切れ・初回電源投入等）のみコンパイル時刻（__DATE__/__TIME__）
// を自動書き込みする。時刻が有効な場合は上書きしない
// （WDT リセット等で頻繁に再起動しても時刻が巻き戻らないようにするため）。
// 手動で時刻を指定したい場合は DS3231_FORCE_SET_TIME を 1 にする（使用後は 0 に戻すこと）。
#define DS3231_FORCE_SET_TIME 0

// USE_DS3231_TIMESTAMP=1 にすると各サイクルで DS3231 の現在時刻を読み出し、
// DEBUG_MODE=1 の場合はシリアルに出力する（BLE/LoRaモードではペイロードにも使用）。
#define USE_DS3231_TIMESTAMP 1        // 1: 各サイクルで時刻を読み出す

// ── ボタンイベント履歴の手動クリア ──────────────────────────────
// EVENTLOG_FORCE_CLEAR を 1 にして書き込むと、起動時にフラッシュ上の
// ボタンイベント履歴（tare.bin とは別ファイル）を空にする。
// クリア後は必ず 0 に戻してビルドし直すこと（毎起動で消去されてしまうため）。
#define EVENTLOG_FORCE_CLEAR 0

// ── Sigfox / LoRa 共用 UART 設定 ────────────────────────────────
// D8/D9 は Sigfox・LoRa で共用ネット（ビルド時排他のため物理的に一方しか実装しない）
#define SIGFOX_TX_PIN 8    // D8: XIAO TX → BRKLSM100 RX（またはE220 RXD）
#define SIGFOX_RX_PIN 9    // D9: XIAO RX ← BRKLSM100 TX（またはE220 TXD）
#define SIGFOX_BAUD   9600
#ifdef COMM_MODE_LORA
#define LORA_TX_PIN   SIGFOX_TX_PIN
#define LORA_RX_PIN   SIGFOX_RX_PIN
#define LORA_UART_BAUD 9600   // E220-900T22S(JP) デフォルト（要データシート確認）
#endif

// ── I2C 加速度センサ設定 ───────────────────────────────────────
// 対応センサ: LSM6DS3 / LSM6DSO / LSM6DSL（SA0 ピンでアドレス切替）
//   SA0 = LOW（GND）  → 0x6A
//   SA0 = HIGH（3V3） → 0x6B ← スキャンで確認済み
// ※ MPU6050 を使う場合: AD0=LOW→0x68（DS3231と競合）, AD0=HIGH→0x69
#define MPU_ADDR 0x6B  // LSM6DS SA0=HIGH

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
const uint8_t CH_ASSIGN[4] = {1, 1, 1, 1};

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
// ピン番号（Arduino ピン番号 = XIAO の Dx。正本 Monita_Flex_構成_v3.10.md）
// ============================================================

// HX711: PD_SCK=D6, DOUT=D7（SN74LV4052 MUX 経由で各 JP に接続）
#define HX711_SCK_PIN  6
#define HX711_DOUT_PIN 7

// アナログ: 電池分圧=A3、温度センサ=A2（解像度は setup で 12bit 設定）
#define BATT_ANALOG_PIN A3
#define TEMP_ANALOG_PIN A2

// TCA9534（U6）: SN74LV4052 の A/B を I²C で駆動。A0=A1=A2=GND → 0x20
// v3.10ではP2をLoRa(E220) M0/M1共通駆動にも使用（基板上でM0/M1短絡・新規配線）
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
  ERR_LORA_CFG = 1u << 6,     // LoRa設定モードでのREAD/WRITE応答異常
};

static uint32_t s_errors;

// D0 ボタン: ISR からメインへ「押下エッジあり」を伝えるフラグ
static volatile bool s_btnFlag = false;
// deepSleep() がタイマー満了前にボタン長押しで抜けたとき、次の loop() で tare を実行する
static bool s_pendingTare = false;

static void btnISR() {
  s_btnFlag = true;
}

// ボタンイベント（タレ成功・短押しリセット等）をフラッシュへ記録する。定義は後方（DS3231関連の後）。
static void logEvent(const char *type, int32_t extra);

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

// タレ（ゼロ点補正）成功時: シアン（緑+青）を約4秒間点滅させる。
// 他のどの状態表示（測定=緑・エラー=赤・スリープ=青ディム・送信=緑点滅）とも
// 色の組み合わせが重ならないよう選定。現場でボタン操作の結果が目視で分かるようにする。
// 4秒間かけて点滅させることで、ボタンを離す際のチャタリングが収まるまでの
// 猶予時間としても機能する（呼び出し側で s_btnFlag を再クリアする前提）。
static void statusTareSuccessBlink() {
  const unsigned long totalMs     = 4000UL;
  const unsigned long halfPeriodMs = 200UL;
  unsigned long elapsed = 0;
  bool on = false;
  while (elapsed < totalMs) {
    on = !on;
    if (on) {
      rgbHwShow(0, 255, 255, RGB_BRIGHT_FULL);
    } else {
      rgbOff();
    }
    delay(halfPeriodMs);
    elapsed += halfPeriodMs;
  }
  rgbOff();
}

// Sigfox/LoRa 送信中の緑点滅（ノンブロッキング）
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

// ============================================================
// ウォッチドッグタイマー（nRF52840 内蔵 WDT）
// 無人運用中にファーム（HX711/I2C 読み取り等）がハングした場合、
// 自動リセットで復旧するための安全網。一度 START すると停止不可。
//
// スリープ中も WDT は動き続けるため、給餌は 2 箇所で行う:
//   (1) loop() 先頭（起床直後）  → スリープ区間をカバー
//   (2) deepSleep() 開始時       → 計測・アドバタイズ等の活動区間をカバー
// これによりタイムアウトは「スリープ時間」「活動時間」の長い方だけを
// 超える値であればよい（合計値をカバーする必要はない）。
// ============================================================
static uint32_t const WDT_TIMEOUT_MS = 25UL * 60UL * 1000UL;  // 25分（MEASURE_INTERVAL_MIN=20分, ADV_DURATION_MIN=10分に余裕を持たせた値）

static void wdtInit(uint32_t timeoutMs) {
  NRF_WDT->CONFIG  = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);  // スリープ中も継続動作
  NRF_WDT->CRV     = (uint32_t)((uint64_t)timeoutMs * 32768ULL / 1000ULL);
  NRF_WDT->RREN    = WDT_RREN_RR0_Msk;
  NRF_WDT->TASKS_START = 1;
}

static inline void wdtFeed() {
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;
}

// 周辺（Sigfox/LoRa・HX711 電源レール 3V3_SW）をオフにし、RTC2 で minutes 分待ってから復帰する。
// 待機中は __WFI で CPU を止める（他割り込みで一時起床し得るが、フラグが立つまでループ継続）。
static void deepSleep(uint32_t minutes) {
  wdtFeed();  // これから始まる長時間スリープ区間の給餌

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
#if defined(COMM_MODE_SIGFOX) || defined(COMM_MODE_LORA)
  // Sigfox/LoRaモジュール電源オフ後は UART を閉じる（フロートやリーク対策）
  Serial1.end();
#endif

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
          logEvent("RESET_SHORT", 0);
#if DEBUG_MODE
          Serial.println("[BTN] short press -> reset");
          Serial.flush();
          delay(150);  // USB送信完了待ち（flush()だけではリセットで送信が打ち切られることがある）
#endif
          NVIC_SystemReset();
        }
      } else {
        // ボタンはすでに離されている → 短押しとみなしてリセット
        logEvent("RESET_SHORT", 0);
#if DEBUG_MODE
        Serial.println("[BTN] short press (released) -> reset");
        Serial.flush();
        delay(150);  // USB送信完了待ち
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
// TCA9534 — SN74LV4052（U5）の A/B、および（v3.10）LoRa M0/M1 を I²C から駆動
//
// レジスタは TI TCA9534 互換の一般的マップ（Input0 / Output1 / Polarity2 / Config3）。
// ch は 1〜4（物理スロット）。idx 0〜3 で v3.01 相当の A/B 2bit（A=LSB, B=bit1）を TCA の P1/P0 に写像。
// v3.10 では P2=LoRa M0/M1 共通駆動を追加（COMM_MODE_LORA 時のみ出力設定）。
// ★ver3.10 最終ネットリスト（ver3.10.sch）確認: LORA_MODE ネットが U6 P2 と JP11 の
//   M0・M1 の両方に接続されている（基板上でM0/M1が短絡され、P2 1本で共通駆動する設計）。
//   E220の使用範囲ではMode0(両LOW)とMode3(両HIGH)しか使わないため、この共通駆動で足りる。
//   ※当初ファームはP2=M0/P3=M1の2本独立を想定していたが、実基板に合わせてP2共通に修正した。
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
#ifdef COMM_MODE_LORA
  // P0〜P2 を出力（P0/P1=MUX A/B、P2=LoRa M0/M1共通）、P3〜P7 を入力（未使用）
  if (!tca9534WriteReg(0x03, 0xF8))
    return false;
#else
  // P0,P1 を出力、P2〜P7 を入力（未使用ピンは入力へ）
  if (!tca9534WriteReg(0x03, 0xFC))
    return false;
#endif
  uint8_t out = 0;
  if (!tca9534ReadReg(0x01, &out))
    return false;
#ifdef COMM_MODE_LORA
  // A=0,B=0（MUX既知状態）、P2=0（E220 Mode0=通常送受信）で初期化
  out = (uint8_t)((out & (uint8_t)~0x07U) | 0x00U);
#else
  out = (uint8_t)((out & (uint8_t)~0x03U) | 0x00U); // A=0,B=0 で MUX を既知状態へ
#endif
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
  uint8_t successCount = 0;
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
      successCount++;
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

  // 現場でボタン操作の結果が目視で分かるよう、成功時はLEDをシアンで約4秒点滅
  if (successCount > 0) {
    statusTareSuccessBlink();
  }

  // 長押し中〜点滅中にボタンを離した際のチャタリングでFALLING割り込みが
  // 再発生し、s_btnFlag が立ってしまうことがある（離す動作は制御できないため）。
  // これを次サイクルまで持ち越すと「タレ成功直後に勝手にリセットがかかる」
  // 誤動作になるため、ここで確実に破棄する。
  s_btnFlag = false;

  logEvent("TARE_OK", (int32_t)successCount);
}

// 小さな配列のバブルソートで中央値（外れ値に強い簡易ロバスト化）を求める。
// outRange が非NULLの場合、ソート済み配列の最大-最小（そのサイクル内のブレ幅）も返す。
// ※ レンジは BLE/LoRaモードのみペイロードに使用する（Sigfoxモードでは未使用）。
static float medianWithRange(float *a, int n, float *outRange) {
  float t[5];
  memcpy(t, a, sizeof(float) * (size_t)n);
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - i - 1; j++)
      if (t[j] > t[j + 1]) {
        float x = t[j];
        t[j] = t[j + 1];
        t[j + 1] = x;
      }
  if (outRange != nullptr) {
    *outRange = t[n - 1] - t[0];
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
// outRange が非NULLの場合、DATA_NUM回サンプリング中の最大-最小（生値ベース）を返す。
static bool hxRead(int *out, float *outRange = nullptr) {

  unsigned long start = millis();

  while (!hx.is_ready()) {
    if (millis() - start > 1000) {
#if DEBUG_MODE
      Serial.println("[HX TIMEOUT]");
#endif
      s_errors |= ERR_HX711_TIMEOUT;
      statusErrorRed();
      *out = 0;
      if (outRange != nullptr) {
        *outRange = 0;
      }
      return false;
    }
  }

  float b[DATA_NUM];
  for (int i = 0; i < DATA_NUM; i++) {
    // get_value() = read() - tare_offset（タレ補正済み生値）
    // read() だとタレ値が反映されないため get_value() を使う
    b[i] = hx.get_value();
  }

  float range = 0;
  *out = (int)medianWithRange(b, DATA_NUM, &range);
  if (outRange != nullptr) {
    *outRange = range;
  }
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

// DS3231 に時刻を書き込む
// yr2: 西暦下 2 桁（2026 → 26）
static bool ds3231SetTime(uint8_t yr2, uint8_t mo, uint8_t day,
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

// ステータスレジスタ 0x0F の OSF（発振停止フラグ, bit7）を読む。
// 1 = 電源喪失等で時刻が無効（要再設定）、0 = 時刻は有効（連続動作中）
static bool ds3231OscillatorStopped() {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x0F);
  if (Wire.endTransmission(false) != 0) return true;  // 読み取れない場合は安全側（無効扱い）
  if (Wire.requestFrom(DS3231_ADDR, (uint8_t)1) < 1) return true;
  uint8_t status = Wire.read();
  return (status & 0x80) != 0;
}

// OSF をクリアする（時刻書き込み後に呼ぶ。0x0F の bit7 を 0 にする以外は保持）
static void ds3231ClearOscillatorStopFlag() {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x0F);
  if (Wire.endTransmission(false) != 0) return;
  if (Wire.requestFrom(DS3231_ADDR, (uint8_t)1) < 1) return;
  uint8_t status = Wire.read();
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x0F);
  Wire.write(status & (uint8_t)~0x80U);
  Wire.endTransmission();
}

// __DATE__ ("Mmm dd yyyy") と __TIME__ ("hh:mm:ss") を解析してビルド時刻を得る。
// コンパイル時に埋め込まれる定数文字列を実行時にパースするだけなので毎ビルドで自動更新される。
static void getCompileTime(uint8_t &yr2, uint8_t &mo, uint8_t &day,
                            uint8_t &hr, uint8_t &mn, uint8_t &sc) {
  static const char monthNames[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char monStr[4] = {0};
  int  d, y, h, mi, s;
  sscanf(__DATE__, "%3s %d %d", monStr, &d, &y);
  sscanf(__TIME__, "%d:%d:%d", &h, &mi, &s);
  int monthIdx = (strstr(monthNames, monStr) - monthNames) / 3;  // 0-11
  yr2 = (uint8_t)(y % 100);
  mo  = (uint8_t)(monthIdx + 1);
  day = (uint8_t)d;
  hr  = (uint8_t)h;
  mn  = (uint8_t)mi;
  sc  = (uint8_t)s;
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
// ボタンイベント履歴（フラッシュ保存）
//
// タレ実行・短押しリセット等の発生時刻をフラッシュに記録する。
// 現場でUSB接続できない状況でも、次回接続時に履歴を確認できるようにするため。
// 循環バッファ形式（古いものから上書き）。
// ============================================================

#define EVENT_LOG_CAPACITY 20

struct EventLogEntry {
  char    timestamp[20];  // "YYYY-MM-DD HH:MM:SS"
  char    type[16];       // "TARE_OK" / "RESET_SHORT" 等
  int32_t extra;          // TARE_OK: 成功CH数 / RESET_SHORT: 実測押下ms
};

static const char EVENT_LOG_FILE[] = "/event_log.bin";
static EventLogEntry s_eventLog[EVENT_LOG_CAPACITY];
static uint16_t s_eventLogCount = 0;  // 有効エントリ数（最大 EVENT_LOG_CAPACITY）
static uint16_t s_eventLogNext  = 0;  // 次に書き込むスロット（循環）

// 起動時に呼ぶ。フラッシュから過去のイベントログを読み出す。
static void loadEventLog() {
  if (!InternalFS.begin()) return;
  File f(InternalFS);
  if (f.open(EVENT_LOG_FILE, FILE_O_READ)) {
    uint16_t hdr[2];
    if (f.read((uint8_t *)hdr, sizeof(hdr)) == (int)sizeof(hdr)) {
      s_eventLogCount = hdr[0];
      s_eventLogNext  = hdr[1];
      if (s_eventLogCount > EVENT_LOG_CAPACITY) s_eventLogCount = EVENT_LOG_CAPACITY;
      if (s_eventLogNext  >= EVENT_LOG_CAPACITY) s_eventLogNext = 0;
      f.read((uint8_t *)s_eventLog, sizeof(s_eventLog));
    }
    f.close();
  }
}

// 現在の s_eventLog をフラッシュへ書き込む
static void saveEventLog() {
  if (!InternalFS.begin()) return;
  InternalFS.remove(EVENT_LOG_FILE);
  File f(InternalFS);
  if (f.open(EVENT_LOG_FILE, FILE_O_WRITE)) {
    uint16_t hdr[2] = {s_eventLogCount, s_eventLogNext};
    f.write((const uint8_t *)hdr, sizeof(hdr));
    f.write((const uint8_t *)s_eventLog, sizeof(s_eventLog));
    f.close();
  }
}

// イベントを1件追加してフラッシュに保存する（DEBUG_MODEの有無に関わらず常時記録）。
// type は15文字以内。DS3231の時刻取得に失敗した場合は "????-..." で記録する。
static void logEvent(const char *type, int32_t extra) {
  EventLogEntry &e = s_eventLog[s_eventLogNext];

  Ds3231Time t = {};
  if (ds3231GetTime(t)) {
    snprintf(e.timestamp, sizeof(e.timestamp), "%04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
             (unsigned)t.hour, (unsigned)t.min, (unsigned)t.sec);
  } else {
    snprintf(e.timestamp, sizeof(e.timestamp), "????-??-?? ??:??:??");
  }
  strncpy(e.type, type, sizeof(e.type) - 1);
  e.type[sizeof(e.type) - 1] = '\0';
  e.extra = extra;

  s_eventLogNext = (uint16_t)((s_eventLogNext + 1) % EVENT_LOG_CAPACITY);
  if (s_eventLogCount < EVENT_LOG_CAPACITY) s_eventLogCount++;

  saveEventLog();

#if DEBUG_MODE
  Serial.print("[EVENTLOG] 記録: ");
  Serial.print(e.timestamp);
  Serial.print(" ");
  Serial.print(e.type);
  Serial.print(" extra=");
  Serial.println(e.extra);
#endif
}

// 起動時（DEBUG_MODE時のみ）に過去のイベント履歴をシリアルへ出力する
static void printEventLog() {
#if DEBUG_MODE
  if (s_eventLogCount == 0) {
    Serial.println("[EVENTLOG] 記録なし");
    return;
  }
  Serial.println("[EVENTLOG] ボタンイベント履歴（古い順）:");
  uint16_t start = (s_eventLogCount < EVENT_LOG_CAPACITY) ? 0 : s_eventLogNext;
  for (uint16_t i = 0; i < s_eventLogCount; i++) {
    uint16_t idx = (uint16_t)((start + i) % EVENT_LOG_CAPACITY);
    EventLogEntry &e = s_eventLog[idx];
    Serial.print("  ");
    Serial.print(e.timestamp);
    Serial.print("  ");
    Serial.print(e.type);
    Serial.print("  extra=");
    Serial.println(e.extra);
  }
#endif
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
// 1 サイクル分の計測結果（グローバル: Sigfox/LoRa 組み立てで参照）
// ============================================================

// ch[0..3]: 各スロットの生値（CH_ASSIGN に応じて HX711 または MPU）
int ch[4], tempV, battV;

#if defined(COMM_MODE_BLE) || defined(COMM_MODE_LORA)
// chRange[0..3]: HX711チャネルのDATA_NUM回サンプリング中の最大-最小（ブレ幅）。
// BLE/LoRaのペイロードにのみ使用（Sigfoxモードでは送信しない）。
int chRange[4];
#endif

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
#if defined(COMM_MODE_BLE) || defined(COMM_MODE_LORA)
        chRange[i] = 0;
#endif
      } else {
        // 物理 CH は 1 origin（MUX と正本 JP の対応）
        hxBegin((uint8_t)(i + 1));
#if defined(COMM_MODE_BLE) || defined(COMM_MODE_LORA)
        {
          float range = 0;
          hxRead(&ch[i], &range);
          // レンジも生値→ひずみ値（με）変換で単位を揃える
          chRange[i] = (int)(range / STRAIN_SCALE);
        }
#else
        hxRead(&ch[i]);
#endif
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

  tempV = measureTemp();
  battV = measureBatt();

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
    wdtFeed();  // 長時間の AT 応答待ちでもハング扱いされないよう給餌
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
    String payload = "";
    for (int i = 0; i < 4; i++) payload += hx4(ch[i]);
    payload += hx4(tempV);
    payload += hx4(battV);
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
// LoRa（E220-900T22S(JP)、COMM_MODE_LORA 時のみ）
//
// 【モード制御】M0・M1=TCA9534 P2 共通駆動（基板上でM0/M1短絡、v3.10新規配線）。
//   P2 LOW=Mode0（通常送受信・透過モード）/ P2 HIGH=Mode3（設定・スリープ）。
//   AUXは未接続のため固定ディレイで代替する（LORA_MODE_SWITCH_DELAY_MS）。
//
// 【設定コマンド】0xC0=書込(不揮発保存) / 0xC1=読出 / 0xC2=書込(揮発)
//   レジスタ配置は E220-900T22S(JP) 公式データシート（CLEALINK TECHNOLOGY、
//   Rev.2.1.1）表8〜11で確認済み（2026-07-17、test_sketches 18〜21 で実機検証済み）:
//     0x00 ADDH        : デバイスアドレス上位バイト
//     0x01 ADDL        : デバイスアドレス下位バイト
//     0x02 REG0        : bit7:5=UART速度(011=9600bps) / bit4:0=エア速度(SF/BW)
//     0x03 REG1        : bit7:6=ペイロード長 / bit5=RSSI環境ノイズ有効化 /
//                        bit4:2=Reserved(0固定) / bit1:0=送信出力
//                        (00=Not available / 01=13dBm(default) / 10=7dBm / 11=0dBm)
//     0x04 REG2        : 周波数チャンネル
//     0x05 REG3        : bit7=RSSIバイト有効化 / bit6=送信方式(0=透過(default)/1=固定) /
//                        bit5:4=Reserved(0固定) / bit3=低電圧動作 / bit2:0=WORサイクル
//   ★実機で工場出荷状態を読んだところ REG1=0x00（送信出力Not available）・
//     REG3=0x40（固定送信モード）という異常値だった（テスト機体固有か本当の出荷
//     状態かは不明）。透過送信・有効な送信出力になるよう明示的に書き込む。
//   ★REG3のRSSIバイト有効化(bit7)はFlex（送信専用）自体は使わないが、Gateway側
//     （受信側）と設定を統一するため同じ値を書き込む（全台共通の設計方針）。
//
// 【設定書き込みロジック】選択肢A方式（Monita_Flex_構成_v3.10.md §6参照）:
//   起動（起床）毎に現在値をREADし、想定値と異なればWRITEする。
//   これによりファーム書き換えだけで全台の設定を変更でき、設定化けからも自動復旧する。
//
// 【フレーム形式（透過モードにはフレーム境界が無いため独自に付与）】
//   [0]SYNC=0xAA [1]LEN [2..LEN+1]MSDペイロード(BLEのMSDからCompany ID(2B)を除いた部分と同一形式)
//   [LEN+2]チェックサム = (SYNC+LEN+Σpayload) & 0xFF（単純加算。LoRa無線層自体のCRCと二重）
// ============================================================
#ifdef COMM_MODE_LORA

#define LORA_MODE_SWITCH_DELAY_MS 100U  // M0/M1切替後の安定待ち（暫定値、要実測）
#define LORA_CFG_REG_START 0x00
#define LORA_CFG_REG_LEN   6            // ADDH..OPTION (0x00-0x05)

// 想定設定値（全台共通。2026-07-17、実機2台(test_sketches 18/19)で送受信疎通確認済み）
static const uint8_t LORA_CFG_ADDH = 0x00;
static const uint8_t LORA_CFG_ADDL = 0x00;
static const uint8_t LORA_CFG_REG0 = 0x68;  // UART9600bps + エア速度(SF7/BW125kHz、実機確認値)
static const uint8_t LORA_CFG_REG1 = 0x01;  // ペイロード長200B(default)/RSSIノイズ無効/送信出力13dBm
static const uint8_t LORA_CFG_REG2 = 0x00;  // チャンネル0
static const uint8_t LORA_CFG_REG3 = 0x80;  // RSSIバイト有効化ON(Gatewayとの設定統一用)/透過送信モード

// TCA9534 P2（=E220 M0・M1 共通駆動）を high/low に駆動する。
// ネットリスト上M0とM1は短絡されP2の1本で制御する（両LOW=Mode0通常送受信／両HIGH=Mode3設定）。
// 読み出し失敗時は false（呼び出し側でERR_TCA9534_I2C相当を扱う）
static bool loraSetMode(bool high) {
  uint8_t out = 0;
  if (!tca9534ReadReg(0x01, &out)) return false;
  out = (uint8_t)((out & (uint8_t)~0x04U) | (high ? 0x04U : 0U));  // P2 = bit2
  if (!tca9534WriteReg(0x01, out)) return false;
  delay(LORA_MODE_SWITCH_DELAY_MS);
  return true;
}
static inline bool loraModeNormal() { return loraSetMode(false); }
static inline bool loraModeConfig() { return loraSetMode(true); }

// 設定モード中に READ(0xC1) でレジスタ6バイトを読み出す
static bool loraReadConfig(uint8_t *out6) {
  // ★2026-07-23: Gateway側で「E220が継続的にバイトを送り続ける状態（Configモード未切替時の
  // ノイズ垂れ流し等）だと掃除ループが無限ループしフリーズする」バグが見つかったため、
  // 同じ掃除ループに時間制限を追加（Flex側の未然防止）。
  {
    unsigned long drainStart = millis();
    while (Serial1.available()) {
      Serial1.read();
      if (millis() - drainStart > 300UL) break;
    }
  }
  Serial1.write((uint8_t)0xC1);
  Serial1.write((uint8_t)LORA_CFG_REG_START);
  Serial1.write((uint8_t)LORA_CFG_REG_LEN);

  const int respLen = 3 + LORA_CFG_REG_LEN;  // ヘッダ(0xC1,addr,len) + データ
  uint8_t resp[3 + LORA_CFG_REG_LEN];
  int idx = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 500UL && idx < respLen) {
    if (Serial1.available()) {
      resp[idx++] = (uint8_t)Serial1.read();
    }
  }
  if (idx < respLen) return false;      // 応答不足（タイムアウト）
  if (resp[0] != 0xC1) return false;    // ヘッダ不一致
  memcpy(out6, &resp[3], LORA_CFG_REG_LEN);
  return true;
}

// 設定モード中に WRITE(0xC0, 不揮発保存) でレジスタ6バイトを書き込む
static void loraWriteConfig() {
  Serial1.write((uint8_t)0xC0);
  Serial1.write((uint8_t)LORA_CFG_REG_START);
  Serial1.write((uint8_t)LORA_CFG_REG_LEN);
  Serial1.write(LORA_CFG_ADDH);
  Serial1.write(LORA_CFG_ADDL);
  Serial1.write(LORA_CFG_REG0);
  Serial1.write(LORA_CFG_REG1);
  Serial1.write(LORA_CFG_REG2);
  Serial1.write(LORA_CFG_REG3);
  delay(200);  // モジュール内部の不揮発メモリ書き込み待ち（暫定値、要実測）
  // 応答（エコーバック）は読み捨てる。書込確認は次サイクルのREADで行う
  unsigned long t0 = millis();
  while (millis() - t0 < 300UL) { while (Serial1.available()) Serial1.read(); }
}

#if DEBUG_MODE
// デバッグ用: 6バイトをHEXで出力（期待値と実測値の突き合わせに使う。Gateway側の実装を移植）
static void loraPrintRegs(const char* label, const uint8_t regs[LORA_CFG_REG_LEN]) {
  Serial.print("[LORA] "); Serial.print(label); Serial.print(": ");
  for (int i = 0; i < LORA_CFG_REG_LEN; i++) {
    if (regs[i] < 0x10) Serial.print('0');
    Serial.print(regs[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
}
#endif

// 起動（起床）毎に呼ぶ。現在の設定値を確認し、想定値と異なれば書き込む（選択肢A方式）。
// 成功時 true。READ自体が失敗した場合は ERR_LORA_CFG をセットして false を返す。
static bool loraCheckAndConfigure() {
  if (!loraModeConfig()) {
    s_errors |= ERR_LORA_CFG;
    return false;
  }

  uint8_t cur[LORA_CFG_REG_LEN] = {0};
  bool readOk = loraReadConfig(cur);

  bool matches = readOk &&
      cur[0] == LORA_CFG_ADDH && cur[1] == LORA_CFG_ADDL &&
      cur[2] == LORA_CFG_REG0 && cur[3] == LORA_CFG_REG1 &&
      cur[4] == LORA_CFG_REG2 && cur[5] == LORA_CFG_REG3;

#if DEBUG_MODE
  Serial.print("[LORA] config read ");
  Serial.println(!readOk ? "失敗" : (matches ? "一致" : "不一致→書込"));
  if (readOk) {
    loraPrintRegs("実測値(読込)", cur);
    uint8_t expected[LORA_CFG_REG_LEN] = {LORA_CFG_ADDH, LORA_CFG_ADDL, LORA_CFG_REG0,
                                           LORA_CFG_REG1, LORA_CFG_REG2, LORA_CFG_REG3};
    loraPrintRegs("期待値      ", expected);
  }
#endif

  if (!readOk) {
    s_errors |= ERR_LORA_CFG;
    loraModeNormal();
    return false;
  }

  if (!matches) {
    // ★2026-07-23: 書込直後の確認読み込みがタイミング次第で失敗することがある
    // （E220内部のレジスタ書込処理完了前に読み返してしまう等）とGateway側の実機
    // デバッグで確認したため、Flex側にも同じ「書込→確認」のリトライを移植する。
    // 従来は書込むだけで確認せず、中途半端な設定のまま送信してしまう恐れがあった
    // （2026-07-19実機ログで「不一致→書込」の直後に送信し、Gatewayで受信できない
    // 事象が発生。この未確認書込が原因の可能性が高い）。
    bool verifyOk = false;
    for (int attempt = 1; attempt <= 2 && !verifyOk; attempt++) {
      loraWriteConfig();
      uint8_t verify[LORA_CFG_REG_LEN] = {0};
      bool verifyReadOk = loraReadConfig(verify);
      verifyOk = verifyReadOk &&
          verify[0] == LORA_CFG_ADDH && verify[1] == LORA_CFG_ADDL &&
          verify[2] == LORA_CFG_REG0 && verify[3] == LORA_CFG_REG1 &&
          verify[4] == LORA_CFG_REG2 && verify[5] == LORA_CFG_REG3;
#if DEBUG_MODE
      Serial.print("[LORA] config write 確認(");
      Serial.print(attempt); Serial.print("/2): ");
      Serial.println(verifyOk ? "OK" : "NG");
      if (verifyReadOk) loraPrintRegs("書込後の実測値", verify);
      else              Serial.println("[LORA] 書込後の読込自体に失敗（応答なし）");
#endif
    }
    if (!verifyOk) {
      // 2回とも確認NG → 設定が不確実なまま送信すると受信側で復号できない可能性が
      // 高いため、エラーとしてこのサイクルの送信をスキップする（sendLoRa()側で判定）
      s_errors |= ERR_LORA_CFG;
      loraModeNormal();
      return false;
    }
  }

  return loraModeNormal();
}

// 透過モードでフレームを送信する（[SYNC][LEN][payload...][checksum]）
static void loraSendFrame(const uint8_t *msd, uint8_t msdLen) {
  uint8_t sum = (uint8_t)(0xAAU + msdLen);
  Serial1.write((uint8_t)0xAA);
  Serial1.write(msdLen);
  for (uint8_t i = 0; i < msdLen; i++) {
    Serial1.write(msd[i]);
    sum = (uint8_t)(sum + msd[i]);
  }
  Serial1.write(sum);
}

// ── 送信完了待ち・冗長送信 ──────────────────────────────────────────
// AUX未接続のため実際の送信完了（UART転送＋LoRaエアタイム）を検知できない。
// loraSendFrame()はSerial1.write()がUARTバッファに積んだ時点で返るため、
// 直後にdeepSleep()で3V3_SW（E220電源）を落とすと、実際の無線送信が
// 完了する前に電源が切れてパケットが消える（2026-07-17発見・修正）。
// フレーム長22バイト@9600bpsのUART転送(~23ms)＋LoRaエアタイム(SF7/BW125kHzで
// 数十ms)に十分な余裕を見て固定ディレイで待つ（要実測、暫定値）。
#define LORA_TX_COMPLETE_DELAY_MS 300U
// LoRaは単発送信・ACK無しのため、瞬間的な電波障害でそのサイクルのデータが
// 丸ごと失われる。同一フレームを複数回送ることで冗長性を持たせる。
#define LORA_TX_REPEAT 2
#define LORA_TX_REPEAT_GAP_MS 100U

// LoRa MSD ペイロード（BLE MSDからCompany ID(2B)を除いた19バイトと同一形式。Gateway側で
// 同じパース処理を再利用する前提）:
//   [0] Pkt type   : 0x04（Monita Flex v3.10 LoRa）
//   [1] Device ID  : DEVICE_ID
//   [2] FW Version : FW_VERSION
//   [3-4]   CH1 : int16_t LE
//   [5-6]   CH2 : int16_t LE
//   [7-8]   CH3 : int16_t LE
//   [9-10]  CH4 : int16_t LE
//   [11-12] BATT: uint16_t LE（mV）
//   [13]    Hour
//   [14]    Minute
//   [15-18] CH1〜CH4 Range（uint8_t、0〜255にクランプ）
static void sendLoRa() {
  if (s_errors != 0U) {
#if DEBUG_MODE
    Serial.println("[LORA] skipped (errors)");
#endif
    return;
  }

  if (!loraCheckAndConfigure()) {
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[LORA] config check failed, TX skipped");
#endif
    return;
  }

  Ds3231Time rtcT = {};
  bool rtcOk = ds3231GetTime(rtcT);

  uint8_t msd[19];
  msd[0] = 0x04;  // Pkt type: Monita Flex v3.10 LoRa
  msd[1] = DEVICE_ID;
  msd[2] = FW_VERSION;
  for (int i = 0; i < 4; i++) {
    int16_t v = (int16_t)ch[i];
    msd[3 + i * 2]     = (uint8_t)(v & 0xFF);
    msd[3 + i * 2 + 1] = (uint8_t)((v >> 8) & 0xFF);
  }
  uint16_t batt = (uint16_t)battV;
  msd[11] = (uint8_t)(batt & 0xFF);
  msd[12] = (uint8_t)((batt >> 8) & 0xFF);
  msd[13] = rtcOk ? rtcT.hour : 0;
  msd[14] = rtcOk ? rtcT.min  : 0;
  for (int i = 0; i < 4; i++) {
    int r = chRange[i];
    if (r < 0)   r = 0;
    if (r > 255) r = 255;
    msd[15 + i] = (uint8_t)r;
  }

#if DEBUG_MODE && DEBUG_NO_LORA
  Serial.print("[LORA] TX SKIPPED (DEBUG_NO_LORA): FW=");
  Serial.print(FW_VERSION);
  Serial.print(" CH="); for (int i=0;i<4;i++){Serial.print(ch[i]); Serial.print(" ");}
  Serial.print("BATT="); Serial.println(battV);
  return;
#endif

  statusSigfoxBlinkReset();
  for (uint8_t rep = 0; rep < LORA_TX_REPEAT; rep++) {
    loraSendFrame(msd, sizeof(msd));
    delay(LORA_TX_COMPLETE_DELAY_MS);  // 送信完了待ち（電源断・次送信前に確保する）
    if (rep + 1 < LORA_TX_REPEAT) delay(LORA_TX_REPEAT_GAP_MS);
  }

#if DEBUG_MODE
  Serial.print("[LORA] TX FW="); Serial.print(FW_VERSION);
  Serial.print(" CH="); for (int i=0;i<4;i++){Serial.print(ch[i]); Serial.print(" ");}
  Serial.print("RANGE="); for (int i=0;i<4;i++){Serial.print(chRange[i]); Serial.print(" ");}
  Serial.print("BATT="); Serial.print(battV);
  Serial.print(" TIME="); Serial.print(msd[13]); Serial.print(":"); Serial.println(msd[14]);
#endif
}
#endif  // COMM_MODE_LORA

// ============================================================
// BLE アドバタイズ（COMM_MODE_BLE 時のみ）
//
// MSD フォーマット（21 バイト）:
//   [0-1]  Company ID : 0xFF 0xFF
//   [2]    Pkt type   : 0x03（Monita Flex v3.03）
//   [3]    Device ID  : DEVICE_ID
//   [4]    FW Version : 子機ファームのバージョン（コミットごとに+1。git logと突き合わせて特定する）
//   [5-6]  CH1        : int16_t LE
//   [7-8]  CH2        : int16_t LE
//   [9-10] CH3        : int16_t LE
//   [11-12] CH4       : int16_t LE
//   [13-14] BATT      : uint16_t LE（mV）
//   [15]   Hour       : uint8_t
//   [16]   Minute     : uint8_t
//   [17]   CH1 Range  : uint8_t（DATA_NUM回サンプリング中の最大-最小。0〜255にクランプ）
//   [18]   CH2 Range  : uint8_t
//   [19]   CH3 Range  : uint8_t
//   [20]   CH4 Range  : uint8_t
// ============================================================
#ifdef COMM_MODE_BLE
static void bleAdvertise(uint8_t hour, uint8_t min_val) {
  uint8_t buf[21];
  buf[0]  = 0xFF;
  buf[1]  = 0xFF;
  buf[2]  = 0x03;          // Pkt type: Monita Flex v3.03
  buf[3]  = DEVICE_ID;
  buf[4]  = FW_VERSION;
  for (int i = 0; i < 4; i++) {
    int16_t v = (int16_t)ch[i];
    buf[5 + i * 2]     = (uint8_t)(v & 0xFF);
    buf[5 + i * 2 + 1] = (uint8_t)((v >> 8) & 0xFF);
  }
  uint16_t batt = (uint16_t)battV;
  buf[13] = (uint8_t)(batt & 0xFF);
  buf[14] = (uint8_t)((batt >> 8) & 0xFF);
  buf[15] = hour;
  buf[16] = min_val;
  for (int i = 0; i < 4; i++) {
    int r = chRange[i];
    if (r < 0)   r = 0;
    if (r > 255) r = 255;
    buf[17 + i] = (uint8_t)r;
  }

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
  Serial.print("[BLE] アドバタイズ開始 ID=0x");
  Serial.print(DEVICE_ID, HEX);
  Serial.print(" FW=");
  Serial.print(FW_VERSION);
  Serial.print(" CH=");
  for (int i = 0; i < 4; i++) { Serial.print(ch[i]); Serial.print(" "); }
  Serial.print("RANGE=");
  for (int i = 0; i < 4; i++) { Serial.print(chRange[i]); Serial.print(" "); }
  Serial.print("BATT="); Serial.print(battV);
  Serial.print(" TIME="); Serial.print(hour); Serial.print(":"); Serial.println(min_val);
#endif
}
#endif  // COMM_MODE_BLE

// ============================================================
// Arduino エントリ
// ============================================================

void setup() {

#if DEBUG_MODE
  // ── デバッグ用: 起動理由の確認（RESETREAS） ─────────────────────
  // ボタン誤作動・ウォッチドッグ・ブラウンアウト等、リセットの原因切り分けのため
  // 他の初期化より先に読み取る（他コードがRESETREASに触れる前に読むこと）。
  // USB再列挙・モニタ再接続のタイミングによっては起動直後の出力を1回で
  // 取りこぼすことがあるため、値は先に読み切ってから複数回リピート出力する。
  {
    uint32_t reason = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = 0xFFFFFFFFUL;  // 読み取り後すぐクリア（次回リセット時に前回分と混ざらないように）

    Serial.begin(115200);
    // monitor_dtr=0 では !Serial が解除されないため while(!Serial) は使わない。
    for (int rep = 0; rep < 5; rep++) {
      delay(1000);  // USB CDC 安定待ち・モニタ再接続待ち（1秒間隔で5回リピート）
      Serial.println("=== DEBUG START ===");
#if defined(COMM_MODE_BLE) || defined(COMM_MODE_LORA)
      Serial.print("[FW] Version=");
      Serial.println(FW_VERSION);
#endif
      Serial.print("[RESETREAS] 0x");
      Serial.println(reason, HEX);
      if (reason == 0) {
        Serial.println("  - (フラグなし。パワーオン起動、または前回リセット時に未クリア)");
      }
      if (reason & POWER_RESETREAS_RESETPIN_Msk) Serial.println("  - RESETPIN: 外部リセットピン");
      if (reason & POWER_RESETREAS_DOG_Msk)      Serial.println("  - DOG: ウォッチドッグタイマー満了");
      if (reason & POWER_RESETREAS_SREQ_Msk)     Serial.println("  - SREQ: ソフトウェアリセット（NVIC_SystemReset。ボタン短押し等）");
      if (reason & POWER_RESETREAS_LOCKUP_Msk)   Serial.println("  - LOCKUP: CPUロックアップ");
      if (reason & POWER_RESETREAS_OFF_Msk)      Serial.println("  - OFF: GPIOによるOFF状態からの復帰");
      if (reason & POWER_RESETREAS_LPCOMP_Msk)   Serial.println("  - LPCOMP");
      if (reason & POWER_RESETREAS_DIF_Msk)      Serial.println("  - DIF: デバッグ割り込み");
      if (reason & POWER_RESETREAS_NFC_Msk)      Serial.println("  - NFC");
      if (reason & POWER_RESETREAS_VBUS_Msk)     Serial.println("  - VBUS: USB接続によるリセット");
      Serial.flush();
    }
  }
#endif

  wdtInit(WDT_TIMEOUT_MS);  // 無人運用の安全網。以降 25 分キックが無ければ自動リセット

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

#ifdef COMM_MODE_SIGFOX
  // nRF UART ピン割当（setPins の引数順: RX, TX）
  Serial1.setPins(SIGFOX_RX_PIN, SIGFOX_TX_PIN);
  Serial1.begin(SIGFOX_BAUD);
  delay(3000);  // BRKLSM100 コールドスタート待ち
#endif

#ifdef COMM_MODE_LORA
  // nRF UART ピン割当（setPins の引数順: RX, TX）。SigfoxとネットDピン共用のためレジスタ上は同一
  Serial1.setPins(LORA_RX_PIN, LORA_TX_PIN);
  Serial1.begin(LORA_UART_BAUD);
  delay(500);  // E220 起動待ち（暫定値、要データシート確認・実測）
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

  Wire.begin();
  analogReadResolution(12);
  delay(200);

  s_errors = ERR_NONE;

  // ── DS3231 時刻自動設定 ──────────────────────────────────────
  // OSF（発振停止フラグ）が立っている場合（バックアップ電池切れ・初回電源投入等で
  // 時刻が無効な場合）のみ、コンパイル時刻を自動書き込みする。
  // 時刻が有効な場合は上書きしない（WDT リセット等で頻繁に再起動しても
  // 時刻が巻き戻らないようにするため）。
  {
    bool needsSet = DS3231_FORCE_SET_TIME || ds3231OscillatorStopped();
    if (needsSet) {
      uint8_t yr2, mo, day, hr, mn, sc;
      getCompileTime(yr2, mo, day, hr, mn, sc);
      bool ok = ds3231SetTime(yr2, mo, day, hr, mn, sc);
      if (ok) ds3231ClearOscillatorStopFlag();
#if DEBUG_MODE
      Serial.print("[DS3231] 時刻が無効だったためビルド時刻を書き込み: ");
      Serial.println(ok ? "OK" : "FAILED");
#endif
      if (!ok) s_errors |= ERR_DS3231_I2C;
    }
  }

  if (!tca9534Configure()) {
    s_errors |= ERR_TCA9534_I2C;
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[TCA9534] init failed");
#endif
  }

  // フラッシュから前回のタレオフセットを復元（リセット後も継続して有効にするため）
  loadTareOffsets();

  // フラッシュからボタンイベント履歴を復元し、USB接続時に確認できるよう表示
#if EVENTLOG_FORCE_CLEAR
  s_eventLogCount = 0;
  s_eventLogNext  = 0;
  saveEventLog();
#if DEBUG_MODE
  Serial.println("[EVENTLOG] EVENTLOG_FORCE_CLEAR=1 のため履歴を消去しました");
#endif
#else
  loadEventLog();
#endif
  printEventLog();

  measureAll();

#ifdef COMM_MODE_SIGFOX
  sendSigfox();
  deepSleep(SLEEP_MINUTES);
#endif

#ifdef COMM_MODE_LORA
  sendLoRa();
  deepSleep(SLEEP_MINUTES);
#endif

#ifdef COMM_MODE_BLE
  {
    Ds3231Time rtcT = {};
    bool rtcOk = ds3231GetTime(rtcT);
#if ADV_CONTINUOUS_MODE
    bleAdvertise(rtcOk ? rtcT.hour : 0, rtcOk ? rtcT.min : 0);
    delay(10000);
#else
    if (rtcOk && rtcT.min <= ADV_TRIGGER_MIN) {
      bleAdvertise(rtcT.hour, rtcT.min);
      uint32_t advMs = (uint32_t)ADV_DURATION_MIN * 60000UL;
      uint32_t t0 = millis();
      while (millis() - t0 < advMs) { delay(1000); yield(); }
      Bluefruit.Advertising.stop();
    } else {
#if DEBUG_MODE
      // アドバタイズ時間窓（毎時 :00〜:02）外のサイクル。
      // 計測ログはこの上ですでに出ているが、このサイクルではBLE送信をスキップしたことを明示する。
      Serial.print("[BLE] アドバタイズ時間窓外のためスキップ TIME=");
      Serial.print(rtcOk ? rtcT.hour : 0);
      Serial.print(":");
      Serial.print(rtcOk ? rtcT.min : 0);
      Serial.print(rtcOk ? "" : "（DS3231読み取り失敗）");
      Serial.println();
#endif
    }
    deepSleep(MEASURE_INTERVAL_MIN);
#endif
  }
#endif
}

void loop() {
  wdtFeed();  // 起床直後の給餌（スリープ区間ぶんをここでキック）

  statusIdleBlueDim();
  digitalWrite(SW_POWER_PIN, HIGH);  // 3V3_SW 再投入

#ifdef COMM_MODE_SIGFOX
  // スリープ中に Serial1.end() しているため再初期化
  Serial1.setPins(SIGFOX_RX_PIN, SIGFOX_TX_PIN);
  Serial1.begin(SIGFOX_BAUD);
  delay(3000);  // BRKLSM100 再起動待ち
#elif defined(COMM_MODE_LORA)
  // スリープ中に Serial1.end() しているため再初期化
  Serial1.setPins(LORA_RX_PIN, LORA_TX_PIN);
  Serial1.begin(LORA_UART_BAUD);
  delay(500);  // E220 起動待ち（暫定値、要データシート確認・実測）
#else
  // BLE モードには Sigfox/LoRa 起動待ちが無いため、電源投入直後の電圧安定化待ちが無かった。
  // 4ch HX711 同時投入時の突入電流によるレール電圧降下が I2C(TCA9534) エラーの
  // 原因になり得るため、Wire.begin() 前に安定化待ちを入れる。
  delay(100);
#endif

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
#if DEBUG_MODE
    Serial.print("[BTN] ISRフラグ検出 t=");
    Serial.print(millis());
    Serial.println(" ms");
#endif
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
      unsigned long heldMs = millis() - pressStart;
      if (longPress) {
        s_pendingTare = true;
#if DEBUG_MODE
        Serial.print("[BTN] long press -> tare pending (held=");
        Serial.print(heldMs);
        Serial.println(" ms)");
#endif
      } else {
        logEvent("RESET_SHORT", (int32_t)heldMs);
#if DEBUG_MODE
        Serial.print("[BTN] short press -> reset (held=");
        Serial.print(heldMs);
        Serial.println(" ms, ボタンが早期に離された可能性)");
        Serial.flush();
        delay(150);  // USB送信完了待ち（flush()だけではリセットで送信が打ち切られることがある）
#endif
        NVIC_SystemReset();
      }
    } else {
      // ボタンはすでに離されている（計測中等に押されてloop()到達時に検知）→ 短押しとみなしてリセット
      logEvent("RESET_SHORT", 0);
#if DEBUG_MODE
      Serial.println("[BTN] short press (released) -> reset（チャタリング除去20ms待機時点で既にHIGH）");
      Serial.flush();
      delay(150);  // USB送信完了待ち
#endif
      NVIC_SystemReset();
    }
  }

  if (s_pendingTare) {
    s_pendingTare = false;
    performTare();
  }
  measureAll();

#ifdef COMM_MODE_SIGFOX
  sendSigfox();
  deepSleep(SLEEP_MINUTES);
#endif

#ifdef COMM_MODE_LORA
  sendLoRa();
  deepSleep(SLEEP_MINUTES);
#endif

#ifdef COMM_MODE_BLE
  {
    Ds3231Time rtcT = {};
    bool rtcOk = ds3231GetTime(rtcT);
#if ADV_CONTINUOUS_MODE
    bleAdvertise(rtcOk ? rtcT.hour : 0, rtcOk ? rtcT.min : 0);
    delay(10000);
#else
    if (rtcOk && rtcT.min <= ADV_TRIGGER_MIN) {
      bleAdvertise(rtcT.hour, rtcT.min);
      uint32_t advMs = (uint32_t)ADV_DURATION_MIN * 60000UL;
      uint32_t t0 = millis();
      while (millis() - t0 < advMs) { delay(1000); yield(); }
      Bluefruit.Advertising.stop();
    } else {
#if DEBUG_MODE
      // アドバタイズ時間窓（毎時 :00〜:02）外のサイクル。
      // 計測ログはこの上ですでに出ているが、このサイクルではBLE送信をスキップしたことを明示する。
      Serial.print("[BLE] アドバタイズ時間窓外のためスキップ TIME=");
      Serial.print(rtcOk ? rtcT.hour : 0);
      Serial.print(":");
      Serial.print(rtcOk ? rtcT.min : 0);
      Serial.print(rtcOk ? "" : "（DS3231読み取り失敗）");
      Serial.println();
#endif
    }
    deepSleep(MEASURE_INTERVAL_MIN);
#endif
  }
#endif
}
