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
 *   framework の Seeed_XIAO_nRF52840_Sense の LED_RED/GREEN/BLUE（離散 GPIO）を使用。
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
// USB CDC（Serial）。ログ出力に使用（USB未接続時は書き込みが即座に破棄される）
#include <Adafruit_TinyUSB.h>
// VL53L4CD（ToF距離センサ、I2C）。CH_ASSIGN[i]=4 のスロットで使用
// stm32duino/STM32duino VL53L4CD（STマイクロエレクトロニクス公式ライブラリ）
#include <vl53l4cd_class.h>
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
static const uint8_t  DEVICE_ID  = 0x0E;  // 子機 ID（iPEC実機テスト用。Gateway側 01_http_post の TARGET_DEVICE_ID と一致させること）
static const uint8_t  FW_VERSION = 8;     // 子機ファームのバージョン。コミットのたびに+1すること
#endif

// ============================================================
// アプリ設定（ここを主に編集する）
// ============================================================

// USB Serial デバッグログは常時有効（USB未接続時は write() が即座に破棄されるため
// 消費電力への影響はない）。
#define SLEEP_MINUTES        1       // 1サイクル後のスリープ時間（分）
#define BOOT_BLUE_MS         500      // 電源 ON 後の青点灯時間（ms）
// ── タレ（ゼロ点補正）操作 ─────────────────────────────────────
// 【操作方法】D0 のタクトスイッチを押しながら電源スイッチ(S1)を ON にし、
//   LED（青）が点灯したらボタンを離す → タレ実行（成功時シアン点滅）。
//
// 【なぜ「離したら実行」なのか】
//   ボタンが物理的に固着・浸水した場合、「押されていたら即実行」だと
//   WDT リセットや電源瞬断のたびに勝手に再タレされてしまう。
//   「離す」ことを条件にすれば、固着状態では永遠に実行されない。
//
// 【なぜスリープ中の長押しをやめたのか】
//   nRF52 の attachInterrupt() は GPIOTE の IN event モードを使い、
//   スリープ電流が約14µA増える（PPK2 実測: 17.5µA → 31.6µA）。
//   起動時判定なら割り込み不要で digitalRead() だけで済む。
#define BOOT_TARE_RELEASE_TIMEOUT_MS 15000UL  // ボタンが離されるのを待つ上限（ms）

// ── DS3231 RTC 設定 ────────────────────────────────────────────
// 起動時に DS3231 の OSF（発振停止フラグ）を確認し、時刻が無効な場合
// （バックアップ電池切れ・初回電源投入等）のみコンパイル時刻（__DATE__/__TIME__）
// を自動書き込みする。時刻が有効な場合は上書きしない
// （WDT リセット等で頻繁に再起動しても時刻が巻き戻らないようにするため）。
// これによりアップロード直後（初回電源投入）は自動的にビルド時刻が書き込まれる。

// USE_DS3231_TIMESTAMP=1 にすると各サイクルで DS3231 の現在時刻を読み出し、
// シリアルに出力する（BLE/LoRaモードではペイロードにも使用）。
#define USE_DS3231_TIMESTAMP 1        // 1: 各サイクルで時刻を読み出す

// ボタンイベント履歴（フラッシュ保存）は起動毎に消去する。
// 履歴確認は Gateway 側の受信ログ CSV で行う。

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

// 各スロット i（0〜3）が CH(i+1) に相当。
//   1 = HX711（ひずみ・荷重）
//   2 = TCA9546A 経由 I2C センサ（LSM6DS 等の加速度センサなど）
//       ※ DS3231 はオンボード U7（0x68）と競合するため CH 接続不可
//   3 = DS18B20（1-Wire 温度センサ）※ 外部プルアップ 4.7kΩ（3V3_SW → CH pin3）必要
//   4 = VL53L4CD（ToF距離センサ、I2C）※ I2Cモジュール（MPU6050等と同一配線）を使用。
//       TCA9546A 経由でチャンネル分離するためアドレス競合なし
const uint8_t CH_ASSIGN[4] = {3, 3, 1, 1};

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

// HX711 1回の平均を求めるための生サンプル数（有野川子基板と同じ2段方式）
#define SAMPLES_PER_AVG 5
// 平均値を何回取得してメジアンを求めるか（最大・最小はペイロードに含めない）
#define MEASURE_COUNT   5

// ── VL53L4CD（ToF距離センサ）測定パラメータ ─────────────────────────
// タイミングバジェット（積分時間）[ms]。VL53L4CD_SetRangeTiming() の第1引数。
// 長くするほど精度が上がるがサンプル取得に時間がかかる（10〜200ms程度が目安）。
#define VL53_TIMING_BUDGET_MS   100
// 1回のメジアンを求めるための生サンプル数
#define VL53_SAMPLES_PER_MEDIAN 50
// 上記メジアンを何回取得し、最終メジアンを求めるか
#define VL53_MEASURE_COUNT      50

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

// ── スリープ時の I2C 完全切り離し用（消費電流対策）────────────────
// Wire（TwoWire）が使うのは NRF_TWIM0（ベースアドレス 0x40003000）。
// framework-arduinoadafruitnrf52/libraries/Wire/Wire_nRF52.cpp:406 で確認済み。
#define TWIM0_BASE_ADDR 0x40003000UL

// nRF52 ペリフェラルの強制パワーサイクル（Nordic 案内のワークアラウンド）。
// ENABLE=0 だけでは内部が完全停止しないため、ベースアドレス+0xFFC に
// 0 → 読み戻し → 1 を書いてリセットする。実行後は再初期化が必要。
static inline void nrfPeripheralPowerCycle(uint32_t baseAddr) {
  *(volatile uint32_t *)(baseAddr + 0xFFC) = 0;
  (void)*(volatile uint32_t *)(baseAddr + 0xFFC);
  *(volatile uint32_t *)(baseAddr + 0xFFC) = 1;
}

// 指定した Arduino ピンを「入力バッファ切断・プルなし」の最小消費状態にする。
//
// 【なぜ必要か】Wire.begin() は SDA/SCL を
//   DIR=Input / INPUT=Connect / PULL=**Pullup** に設定するが（Wire_nRF52.cpp:55-64）、
//   Wire.end() は ENABLE=0 を書くだけでこれを元に戻さない。
// Flex基板の I2C 外部プルアップ（R9/R10, 4.7kΩ）は 3V3_SW に接続されており、
// スリープ中は 3V3_SW = 0V になる。このとき nRF52 の内部プルアップ（約13kΩ）から
// 外部4.7kΩ を通って GND へ電流が流れ続ける:
//   3.3V ÷ (13kΩ + 4.7kΩ) ≒ 186µA/本 × 2本 ≒ 372µA
// PPK2 実測でも Wire.begin() の有無でスリープ電流が 18µA ↔ 305µA と変化し、
// このピン切り離しを行うことで 17µA まで復帰することを確認済み
// （test_sketches/24_sleep_current_baseline による二分探索）。
static inline void nrfPinDisconnect(uint8_t arduinoPin) {
  uint32_t p = g_ADigitalPinMap[arduinoPin];
  NRF_GPIO_Type *port = (p < 32) ? NRF_P0 : NRF_P1;
  port->PIN_CNF[p & 0x1F] =
      ((uint32_t)GPIO_PIN_CNF_DIR_Input        << GPIO_PIN_CNF_DIR_Pos)
    | ((uint32_t)GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos)
    | ((uint32_t)GPIO_PIN_CNF_PULL_Disabled    << GPIO_PIN_CNF_PULL_Pos)
    | ((uint32_t)GPIO_PIN_CNF_DRIVE_S0S1       << GPIO_PIN_CNF_DRIVE_Pos)
    | ((uint32_t)GPIO_PIN_CNF_SENSE_Disabled   << GPIO_PIN_CNF_SENSE_Pos);
}

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
  ERR_VL53L4CD_I2C = 1u << 7, // VL53L4CD 初期化／測距失敗
};

static uint32_t s_errors;

// 電源投入時に D0 ボタンが押されていたか（起動直後に1回だけ読み取る）。
// true の場合、初期化完了後に「ボタンが離されるのを待ってからタレ実行」する。
// ※ ボタン割り込み（attachInterrupt/GPIOTE）は使わない。スリープ電流が
//    約14µA増えるため（PPK2実測: 17.5µA → 31.6µA）。
static bool s_bootButtonHeld = false;

// ボタンイベント（タレ実行等）をフラッシュへ記録する。定義は後方（DS3231関連の後）。
static void logEvent(const char *type, int32_t extra);

// ============================================================
// ステータス LED（離散 RGB）
// ============================================================

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

#define RGB_BRIGHT_FULL 255
#define RGB_BRIGHT_SLEEP_BLUE 16 // スリープ時の青（できるだけ低輝度）

static void rgbOff() {
#ifdef LED_RED
  // アクティブ LOW: 255(HIGH) = 消灯
  analogWrite(LED_RED,   255);
  analogWrite(LED_GREEN, 255);
  analogWrite(LED_BLUE,  255);
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
// スリープ（FreeRTOS vTaskDelay ベース）
//
// 【経緯】以前は独自にRTC2レジスタを操作し while(!flag){__WFI();} で待機していたが、
// この方式ではFreeRTOSのスケジューラから見てタスクが「実行中」のままになるため
// Tickless Idle が発動せず、システムティック割り込みにより頻繁にCPUが起こされ
// 続けていた。PPK2実測でスリープ電流が期待値に対し数十倍（約8〜10mA）高くなって
// いることが判明したため、FreeRTOSのブロッキング待機に置き換えた
// （Tickless Idle が正しく発動し、CPUが低消費電力状態に入る）。
//
// ボタンによる早期起床は廃止したため、タスク通知（ulTaskNotifyTake）ではなく
// 単純な vTaskDelay() を使う。タレはボタンを押しながら電源ONする方式に変更した。
// ============================================================

#ifdef COMM_MODE_LORA
// ★2026-07-24追加: 複数台のLoRa子機を同時展開する際、E220の透過モードには
// CSMA等の衝突回避機構が無いため、全台が同じSLEEP_MINUTES間隔で起床すると
// 送信タイミングが揃い続け電波衝突が起きやすい。nRF52840内蔵ハードウェア乱数
// 発生器(RNG)で毎回本物の乱数を取り、スリープ時間にジッターを加えて各台の
// 送信タイミングをばらけさせる（デバイスIDベースの疑似乱数だと固定シードで
// 同期しかねないため、電源投入毎に変わるHW RNGを使う）。
static uint8_t nrfTrueRandomByte() {
  NRF_RNG->TASKS_START = 1;
  NRF_RNG->EVENTS_VALRDY = 0;
  while (NRF_RNG->EVENTS_VALRDY == 0) { /* HW RNGの1バイト生成を待つ（数十us程度） */ }
  uint8_t v = (uint8_t)NRF_RNG->VALUE;
  NRF_RNG->EVENTS_VALRDY = 0;
  NRF_RNG->TASKS_STOP = 1;
  return v;
}

// -jitterMaxSec 〜 +jitterMaxSec の範囲でランダムな符号付きオフセット(秒)を返す
static int32_t loraSleepJitterSeconds(uint16_t jitterMaxSec) {
  uint16_t raw = ((uint16_t)nrfTrueRandomByte() << 8) | nrfTrueRandomByte();
  uint32_t range = (uint32_t)jitterMaxSec * 2U + 1U;
  return (int32_t)(raw % range) - (int32_t)jitterMaxSec;
}
#endif

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
static uint32_t const WDT_TIMEOUT_MS = 65UL * 60UL * 1000UL;  // 65分（VL53L4CD 2段メジアン化で計測時間が最大約10分に伸びたため余裕を持たせた値）

static void wdtInit(uint32_t timeoutMs) {
  NRF_WDT->CONFIG  = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);  // スリープ中も継続動作
  NRF_WDT->CRV     = (uint32_t)((uint64_t)timeoutMs * 32768ULL / 1000ULL);
  NRF_WDT->RREN    = WDT_RREN_RR0_Msk;
  NRF_WDT->TASKS_START = 1;
}

static inline void wdtFeed() {
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;
}

// 周辺（Sigfox/LoRa・HX711 電源レール 3V3_SW）をオフにし、minutes 分だけ
// vTaskDelay() でブロッキング待機してから復帰する（Tickless Idle 発動のため）。
// ボタンによる早期起床は行わない（タレは起動時ボタン押下で実行する方式）。
static void deepSleep(uint32_t minutes) {
  wdtFeed();  // これから始まる長時間スリープ区間の給餌

  Serial.println("[Sleep before wait]");

  // スリープ中は LED オフ（消費電流削減）
  rgbOff();

  // rgbOff() は analogWrite(pin, 255) でLEDを消灯しているが、これはデューティ比を
  // 255（消灯相当）に書き込むだけで、内部で使われるPWMペリフェラル自体は停止しない。
  // PWMペリフェラルが動作し続けると、その動作クロックにより周期的にCPUが起こされ、
  // スリープ電流が数百µA〜mAオーダーまで上昇してしまう（実測で確認済み）。
  // スリープ前に明示的に停止する（次回analogWrite()呼び出し時に自動的に再初期化される）。
  NRF_PWM0->TASKS_STOP = 1;
  NRF_PWM0->ENABLE = 0;
  NRF_PWM1->TASKS_STOP = 1;
  NRF_PWM1->ENABLE = 0;
  NRF_PWM2->TASKS_STOP = 1;
  NRF_PWM2->ENABLE = 0;

  // PWMペリフェラルを無効化するとピンの出力制御はGPIOに戻るが、GPIO出力レジスタは
  // analogWrite()経由でしか触れておらず digitalWrite() で明示的に書いたことがないため、
  // リセット時のデフォルト値（LOW）のままになる。アクティブLOWのLEDのため、これだと
  // 点灯してしまう（R/G/B同時点灯で白っぽく見える）。ここで明示的にHIGH（消灯）に固定する。
#ifdef LED_RED
  pinMode(LED_RED,   OUTPUT); digitalWrite(LED_RED,   HIGH);
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, HIGH);
  pinMode(LED_BLUE,  OUTPUT); digitalWrite(LED_BLUE,  HIGH);
#endif

  // スリープ中は周辺 IC 用レールをオフ（消費電流削減。正本の 3V3_SW 節）
  digitalWrite(SW_POWER_PIN, LOW);
#if defined(COMM_MODE_SIGFOX) || defined(COMM_MODE_LORA)
  // Sigfox/LoRaモジュール電源オフ後は UART を閉じる（フロートやリーク対策）
  Serial1.end();
#endif

  // ── I2C（TWIM）をスリープ前に完全に切り離す ──────────────────
  // Wire.end() は ENABLE=0 を書くだけで以下が残るため、個別に対処する:
  //   (1) TWIM ペリフェラルが完全停止しない → 強制パワーサイクル
  //   (2) IRQ が有効なまま                  → NVIC_DisableIRQ
  //   (3) SDA/SCL の内部プルアップが残る    → ピン切断（これが電流の主因）
  // 3V3_SW を LOW にした「後」に実行すること（外部プルアップ先が 0V になった
  // 状態で内部プルアップが残ると電流が流れ続けるため）。
  // 復帰後は loop() 先頭の Wire.begin() で再初期化される。
  Wire.end();
  NVIC_DisableIRQ(SPIM0_SPIS0_TWIM0_TWIS0_SPI0_TWI0_IRQn);
  nrfPeripheralPowerCycle(TWIM0_BASE_ADDR);
  nrfPinDisconnect(PIN_WIRE_SDA);
  nrfPinDisconnect(PIN_WIRE_SCL);

  // 0 分指定は誤設定扱いで最低 1 分
  if (minutes == 0U) {
    minutes = 1U;
  }

  int64_t sleepSeconds = (int64_t)minutes * 60LL;

#ifdef COMM_MODE_LORA
  // ★2026-07-24: 複数台展開時の送信タイミング衝突を避けるため±LORA_SLEEP_JITTER_MAX_SEC秒の
  // ランダムジッターを加える。周期をほぼ崩さずに毎回ずらす目的で±10秒とした。
  static uint16_t const LORA_SLEEP_JITTER_MAX_SEC = 10;  // ±10秒
  int32_t jitterSec = loraSleepJitterSeconds(LORA_SLEEP_JITTER_MAX_SEC);
  sleepSeconds += jitterSec;
  if (sleepSeconds < 1) sleepSeconds = 1;  // ジッターで0秒以下にならないよう下限を確保
  Serial.print("[LORA] sleep jitter: ");
  Serial.print(jitterSec);
  Serial.println(" sec");
#endif

  Serial.print("[Sleep] ");
  Serial.print((long)sleepSeconds);
  Serial.println(" sec");
  Serial.flush();

  uint32_t sleepMs = (uint32_t)(sleepSeconds * 1000LL);

  // sleepMs だけブロッキング待機する。この間、FreeRTOS の Tickless Idle により
  // CPU が低消費電力状態へ落ちる（独自 WFI busy-loop では発動しなかった問題の対策）。
  // ボタンによる早期起床は行わない（GPIOTE を使うとスリープ電流が約14µA増えるため。
  // タレはボタンを押しながら電源ONする方式に変更した）。
  vTaskDelay(pdMS_TO_TICKS(sleepMs));
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

// 現在Wireが向いているI2Cバスをスキャンし、応答したアドレスをシリアルに出力する。
// tcaSelect() で目的のチャネルへ切り替えた直後に呼べば、そのチャネル配下だけを見られる。
static void scanI2CBus(const char *label) {
  Serial.print("[I2C SCAN] ");
  Serial.print(label);
  Serial.println(" 開始");
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("[I2C SCAN]   検出: 0x");
      if (addr < 16) Serial.print("0");
      Serial.println(addr, HEX);
      found++;
    }
  }
  Serial.print("[I2C SCAN] 検出数: ");
  Serial.println(found);
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
    Serial.print("[DS18B20 CH");
    Serial.print(ch);
    Serial.print("] retry ");
    Serial.println(retry + 1);
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
    Serial.print("[DS18B20 CH");
    Serial.print(ch);
    Serial.println("] not found");
    return 0;
  }

  Serial.print("[DS18B20 CH");
  Serial.print(ch);
  Serial.print("] ");
  Serial.print(t, 1);
  Serial.println(" degC");

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
    Serial.println("[TARE] offsets saved to flash");
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
      Serial.print("[TARE] offsets loaded: ");
      for (int i = 0; i < 4; i++) {
        Serial.print(s_hx_tare_offset[i]);
        if (i < 3) Serial.print(", ");
      }
      Serial.println();
    }
    f.close();
  }
}

// HX711 全有効チャネルに tare を実行する（3V3_SW ON・Wire 初期化済みの状態で呼ぶこと）
static void performTare() {
  if ((s_errors & ERR_TCA9534_I2C) != 0U) {
    Serial.println("[TARE] skipped (TCA9534 error)");
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
      Serial.print("[TARE] CH");
      Serial.print(i + 1);
      Serial.print(" done (offset=");
      Serial.print(s_hx_tare_offset[i]);
      Serial.println(")");
    } else {
      Serial.print("[TARE] CH");
      Serial.print(i + 1);
      Serial.println(" timeout");
    }
  }
  // タレ完了後にオフセットをフラッシュへ保存（リセット後も保持するため）
  saveTareOffsets();

  // 現場でボタン操作の結果が目視で分かるよう、成功時はLEDをシアンで約4秒点滅
  if (successCount > 0) {
    statusTareSuccessBlink();
  }

  logEvent("TARE_OK", (int32_t)successCount);
}

// ============================================================
// 起動時タレ（ゼロ点補正）
//
// 【操作方法】D0 のタクトスイッチを押しながら電源スイッチ(S1)を ON にし、
//   LED（青）が点灯したらボタンを離す → タレ実行（成功時シアン約4秒点滅）。
//
// 【「離したら実行」にしている理由】
//   ボタンが浸水・振動などで物理的に固着した場合、「押されていたら即実行」
//   だと WDT リセットや電源瞬断のたびに勝手に再タレされてしまう。
//   「離す」ことを条件にすれば、固着状態では永久に実行されない。
//   タイムアウトした場合は赤点灯で異常を知らせ、タレは行わない。
//
// 呼び出しは 3V3_SW ON・Wire 初期化済み・tca9534Configure() 済みの状態で行うこと。
// ============================================================
static void handleBootTare() {
  if (!s_bootButtonHeld) return;

  Serial.println("[TARE] 起動時ボタン押下を検出。離すのを待っています...");
  Serial.flush();

  // ボタンが離されるのを待つ（押されている間は青点灯で「受付中」を示す）
  statusBootBlueStrong();
  unsigned long t0 = millis();
  while (digitalRead(USER_BUTTON_PIN) == LOW) {
    if (millis() - t0 >= BOOT_TARE_RELEASE_TIMEOUT_MS) {
      // 離されないままタイムアウト → ボタン固着の疑い。タレは実行しない。
      statusErrorRed();
      logEvent("TARE_STUCK", (int32_t)(millis() - t0));
      Serial.println("[TARE] タイムアウト: ボタンが離されません（固着の疑い）。タレを中止します。");
      Serial.flush();
      delay(2000);   // 赤点灯を現場で視認できる時間だけ保持
      rgbOff();
      return;
    }
    delay(10);
  }

  delay(50);  // 離す際のチャタリングが収まるのを待つ

  Serial.println("[TARE] ボタンが離されました。タレを実行します。");
  Serial.flush();

  performTare();
}

// 小さな配列のバブルソートで中央値（外れ値に強い簡易ロバスト化）を求める。
// outRange が非NULLの場合、ソート済み配列の最大-最小（そのサイクル内のブレ幅）も返す。
// ※ レンジは BLE/LoRaモードのみペイロードに使用する（Sigfoxモードでは未使用）。
// ※ 最大・最小の値自体はペイロードに含めない（rangeのみ使用）。
static float medianWithRange(float *a, int n, float *outRange) {
  float t[MEASURE_COUNT];
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

// n個のint配列を昇順ソートしてメジアンを返す（配列はソートされる。VL53L4CDで使用）
static int medianInt(int *a, int n) {
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - i - 1; j++)
      if (a[j] > a[j + 1]) { int x = a[j]; a[j] = a[j + 1]; a[j + 1] = x; }
  return a[n / 2];
}

// 指定チャネル（1〜4）の HX711 に MUX を合わせてからライブラリ begin
static void hxBegin(uint8_t ch) {

  Serial.print("[HX BEGIN CH");
  Serial.print(ch);
  Serial.println("]");

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

// HX711 から SAMPLES_PER_AVG サンプルの平均を1回取得する。
// 戻り値: true=正常、false=タイムアウト（ERR_HX711_TIMEOUT をセット）
static bool hxReadAvg(float *outAvg) {
  unsigned long start = millis();
  while (!hx.is_ready()) {
    if (millis() - start > 1000) {
      Serial.println("[HX TIMEOUT]");
      s_errors |= ERR_HX711_TIMEOUT;
      statusErrorRed();
      *outAvg = 0;
      return false;
    }
  }
  float sum = 0;
  for (int i = 0; i < SAMPLES_PER_AVG; i++) {
    // get_value() = read() - tare_offset（タレ補正済み生値）
    // read() だとタレ値が反映されないため get_value() を使う
    sum += hx.get_value();
  }
  *outAvg = sum / SAMPLES_PER_AVG;
  return true;
}

// MEASURE_COUNT 回の平均値を取得し、そのメジアンを返す（有野川子基板と同じ2段方式）。
// outRange が非NULLの場合、MEASURE_COUNT回の平均値群の最大-最小（ブレ幅）も返す。
// 最大・最小はペイロードには含めない（rangeのみBLE/LoRaモードで使用）。
static bool hxRead(int *out, float *outRange = nullptr) {
  float avgs[MEASURE_COUNT];
  for (int i = 0; i < MEASURE_COUNT; i++) {
    if (!hxReadAvg(&avgs[i])) {
      *out = 0;
      if (outRange != nullptr) {
        *outRange = 0;
      }
      return false;
    }
  }

  float range = 0;
  *out = (int)medianWithRange(avgs, MEASURE_COUNT, &range);
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
// タレ実行・短押しリセット等の発生時刻をフラッシュに記録し、シリアルへ即時出力する。
// 起動毎に消去するため世代をまたいだ履歴保持はしない（履歴は Gateway 側の受信ログ
// CSV で確認する）。循環バッファ形式（古いものから上書き）。
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

// イベントを1件追加してフラッシュに保存する。
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

  Serial.print("[EVENTLOG] 記録: ");
  Serial.print(e.timestamp);
  Serial.print(" ");
  Serial.print(e.type);
  Serial.print(" extra=");
  Serial.println(e.extra);
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
    Serial.println("[DS3231] temp read error");
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
    Serial.print("[LSM6] init failed (addr=0x");
    Serial.print(MPU_ADDR, HEX);
    Serial.println(")");
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

  Serial.print("[LSM6] ax="); Serial.print(ax);
  Serial.print(" ay="); Serial.print(ay);
  Serial.print(" az="); Serial.println(az);

  float pitch =
      atan2f((float)ax, sqrtf((float)ay * (float)ay + (float)az * (float)az)) * 180.0f / (float)PI;
  return (int)(pitch * 10);
}

// ============================================================
// VL53L4CD（I2C ToF距離センサ）— 距離[mm]を返す
//
// I2Cモジュール（MPU6050/LSM6DS と同一配線、TCA9546A 経由）を使用。
// 想定測距レンジ: 500〜800mm程度（VL53L4CDの最大測距範囲 約1.3m 以内）。
// 3V3_SW サイクルごとに電源が切れるため、毎サイクル begin()〜StartRanging() から実行する。
//
// 計測方式: HX711と同じ2段メジアン方式。
//   VL53_SAMPLES_PER_MEDIAN 回の生サンプルからメジアンを1回求め、
//   それを VL53_MEASURE_COUNT 回繰り返して、その最終メジアンを返す。
// ※ サンプル数・回数を大きくするほど1CHあたりの計測時間が延びる点に注意
//   （タイミングバジェット VL53_TIMING_BUDGET_MS × サンプル総数が概算所要時間）。
// ============================================================

static int measureVL53L4CD() {
  // XSHUT（シャットダウン制御ピン）は未配線のため -1 を指定（制御しない）。
  // 3V3_SW サイクルごとに電源が切れて毎回パワーオンリセットされるため問題ない。
  VL53L4CD vl53(&Wire, -1);

  if (vl53.begin() != 0 || vl53.InitSensor() != 0) {
    s_errors |= ERR_VL53L4CD_I2C;
    statusErrorRed();
    Serial.println("[VL53L4CD] init failed");
    return 0;
  }

  // タイミングバジェット（積分時間）・連続測定間隔0（都度手動トリガー相当）。
  vl53.VL53L4CD_SetRangeTiming(VL53_TIMING_BUDGET_MS, 0);
  vl53.VL53L4CD_StartRanging();

  int medians[VL53_MEASURE_COUNT];
  bool failed = false;

  for (int m = 0; m < VL53_MEASURE_COUNT; m++) {
    int samples[VL53_SAMPLES_PER_MEDIAN];

    for (int s = 0; s < VL53_SAMPLES_PER_MEDIAN; s++) {
      uint8_t dataReady = 0;
      unsigned long t0 = millis();
      while (!dataReady) {
        vl53.VL53L4CD_CheckForDataReady(&dataReady);
        if (millis() - t0 > 1000) {
          s_errors |= ERR_VL53L4CD_I2C;
          statusErrorRed();
          Serial.println("[VL53L4CD] timeout waiting for data");
          failed = true;
          break;
        }
        delay(5);
      }
      if (failed) break;

      VL53L4CD_Result_t results;
      vl53.VL53L4CD_GetResult(&results);
      vl53.VL53L4CD_ClearInterrupt();
      samples[s] = (int)results.distance_mm;
    }

    if (failed) break;
    medians[m] = medianInt(samples, VL53_SAMPLES_PER_MEDIAN);
    Serial.print("[VL53L4CD] median[");
    Serial.print(m);
    Serial.print("]=");
    Serial.print(medians[m]);
    Serial.println("mm (50サンプル分のメジアン)");
  }

  vl53.VL53L4CD_StopRanging();

  if (failed) {
    return 0;
  }

  int finalDistance = medianInt(medians, VL53_MEASURE_COUNT);

  Serial.print("[VL53L4CD] distance(median of ");
  Serial.print(VL53_MEASURE_COUNT);
  Serial.print(" x ");
  Serial.print(VL53_SAMPLES_PER_MEDIAN);
  Serial.print(")=");
  Serial.print(finalDistance);
  Serial.println("mm");

  return finalDistance;
}

// ============================================================
// 1 サイクル分の計測結果（グローバル: Sigfox/LoRa 組み立てで参照）
// ============================================================

// ch[0..3]: 各スロットの生値（CH_ASSIGN に応じて HX711 または MPU）
int ch[4], tempV, battV;

#if defined(COMM_MODE_BLE) || defined(COMM_MODE_LORA)
// chRange[0..3]: HX711チャネルのMEASURE_COUNT回（平均値群）の最大-最小（ブレ幅）。
// BLE/LoRaのペイロードにのみ使用（Sigfoxモードでは送信しない）。
int chRange[4];
#endif

// CH_ASSIGN に従い 4 スロット分を順に計測し、温度・電池を末尾に追加
static void measureAll() {

  Serial.println("----- MEASURE START -----");

  // 計測フェーズ: 緑点灯（電源 ON の強青 500 ms は setup で済ませ、その後〜計測開始は setup がディム青）
  statusMeasureGreen();

  // ── パス1: DS18B20（CH_ASSIGN=3）を最初に計測 ────────────────────
  // HX711 が D6 を OUTPUT に設定する前に 1-Wire 通信を完了させる。
  // HX711 → DS18B20 の順に処理すると D6 の状態干渉により
  // DS18B20 が応答しない問題を根本回避する。
  for (int i = 0; i < 4; i++) {
    if (CH_ASSIGN[i] == 3) {
      ch[i] = measureDS18B20((uint8_t)(i + 1));
      Serial.print("[CH");
      Serial.print(i + 1);
      Serial.print("] ");
      Serial.println(ch[i]);
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
    } else if (CH_ASSIGN[i] == 4) {
      // TCA9546A のチャネル i（0 origin）を選択して VL53L4CD を読む
      if (tcaSelect((uint8_t)i) != 0) {
        s_errors |= ERR_TCA_I2C;
        statusErrorRed();
        ch[i] = 0;
        Serial.print("[CH");
        Serial.print(i + 1);
        Serial.println("] tcaSelect failed（TCA9546Aが応答していません）");
      } else {
        {
          char label[16];
          snprintf(label, sizeof(label), "CH%d (TCA ch%d)", i + 1, i);
          scanI2CBus(label);
        }
        ch[i] = measureVL53L4CD();
      }
      tcaDisable();
    } else {
      // CH_ASSIGN[i] == 3 はパス1で処理済み → スキップ
      continue;
    }

    Serial.print("[CH");
    Serial.print(i + 1);
    Serial.print("] ");
    Serial.println(ch[i]);
  }

  tempV = measureTemp();
  battV = measureBatt();

#if USE_DS3231_TIMESTAMP
  {
    Ds3231Time ts;
    if (ds3231GetTime(ts)) {
      // タイムスタンプをシリアルに出力（YYYY-MM-DD HH:MM:SS 形式）
      char tsbuf[20];
      snprintf(tsbuf, sizeof(tsbuf), "%04u-%02u-%02u %02u:%02u:%02u",
               (unsigned)ts.year, (unsigned)ts.month, (unsigned)ts.day,
               (unsigned)ts.hour, (unsigned)ts.min, (unsigned)ts.sec);
      Serial.print("[DS3231] ");
      Serial.println(tsbuf);
    } else {
      Serial.println("[DS3231] time read error");
      s_errors |= ERR_DS3231_I2C;
    }
  }
#endif  // USE_DS3231_TIMESTAMP

  Serial.print("[TEMP(DS3231)] ");
  Serial.print(tempV / 10);
  Serial.print(".");
  Serial.print(abs(tempV % 10));
  Serial.println(" degC");
  Serial.print("[BATT] ");
  Serial.println(battV);
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

  Serial.print(">> ");
  Serial.println(cmd);

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

  Serial.println(response);

  return response;
}

// ch[0..3]・温度・電池を連結した 16 進ペイロードで AT$SF= を送信
static void sendSigfox() {

  if (s_errors != 0U) {
    Serial.println("[SIGFOX] skipped (errors)");
    return;
  }

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
      Serial.println("[SIGFOX] waiting for module ready...");
    }
    if (!ready) {
      Serial.println("[SIGFOX] module not ready: TX skipped (will retry next cycle)");
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

  Serial.print("[SIGFOX] ");
  Serial.println(cmd);

  // Sigfox データ送信中: 緑点滅（sendAT 内で tick）
  statusSigfoxBlinkReset();

  String result = sendAT(cmd, 10000);

  if (result.indexOf("OK") >= 0) {
    Serial.println("[SIGFOX] 送信成功");
  } else {
    s_errors |= ERR_SIGFOX_AT;
    statusErrorRed();
    Serial.println("[SIGFOX] 送信失敗");
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

  Serial.print("[LORA] config read ");
  Serial.println(!readOk ? "失敗" : (matches ? "一致" : "不一致→書込"));
  if (readOk) {
    loraPrintRegs("実測値(読込)", cur);
    uint8_t expected[LORA_CFG_REG_LEN] = {LORA_CFG_ADDH, LORA_CFG_ADDL, LORA_CFG_REG0,
                                           LORA_CFG_REG1, LORA_CFG_REG2, LORA_CFG_REG3};
    loraPrintRegs("期待値      ", expected);
  }

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
      Serial.print("[LORA] config write 確認(");
      Serial.print(attempt); Serial.print("/2): ");
      Serial.println(verifyOk ? "OK" : "NG");
      if (verifyReadOk) loraPrintRegs("書込後の実測値", verify);
      else              Serial.println("[LORA] 書込後の読込自体に失敗（応答なし）");
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
  // ★一時的な緩和（iPEC疎通テスト用）: センサー・ADC未実装のためHX711/DS18B20が
  //   毎回タイムアウト/未検出エラーになるが、本テストの目的はLoRa→Gateway→iPEC
  //   の通信経路の疎通確認であり、CH値が0（未実装により正常）でも送信して構わない。
  //   センサー実装後・本番運用時は、この分岐を削除して元のスキップ動作に戻すこと。
  if (s_errors != 0U) {
    Serial.print("[LORA] errors present (0x");
    Serial.print(s_errors, HEX);
    Serial.println(") だが疎通テストのため送信を続行");
  }

  if (!loraCheckAndConfigure()) {
    statusErrorRed();
    Serial.println("[LORA] config check failed, TX skipped");
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

  statusSigfoxBlinkReset();
  for (uint8_t rep = 0; rep < LORA_TX_REPEAT; rep++) {
    loraSendFrame(msd, sizeof(msd));
    delay(LORA_TX_COMPLETE_DELAY_MS);  // 送信完了待ち（電源断・次送信前に確保する）
    if (rep + 1 < LORA_TX_REPEAT) delay(LORA_TX_REPEAT_GAP_MS);
  }

  Serial.print("[LORA] TX FW="); Serial.print(FW_VERSION);
  Serial.print(" CH="); for (int i=0;i<4;i++){Serial.print(ch[i]); Serial.print(" ");}
  Serial.print("RANGE="); for (int i=0;i<4;i++){Serial.print(chRange[i]); Serial.print(" ");}
  Serial.print("BATT="); Serial.print(battV);
  Serial.print(" TIME="); Serial.print(msd[13]); Serial.print(":"); Serial.println(msd[14]);
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
//   [17]   CH1 Range  : uint8_t（MEASURE_COUNT回の平均値群の最大-最小。0〜255にクランプ）
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
}
#endif  // COMM_MODE_BLE

// ============================================================
// Arduino エントリ
// ============================================================

void setup() {

  // ── 起動理由の確認（RESETREAS） ─────────────────────
  // ボタン誤作動・ウォッチドッグ・ブラウンアウト等、リセットの原因切り分けのため
  // 他の初期化より先に読み取る（他コードがRESETREASに触れる前に読むこと）。
  {
    uint32_t reason = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = 0xFFFFFFFFUL;  // 読み取り後すぐクリア（次回リセット時に前回分と混ざらないように）

    Serial.begin(115200);
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

  wdtInit(WDT_TIMEOUT_MS);  // 無人運用の安全網。以降 65 分キックが無ければ自動リセット

  rgbHwBegin();
  rgbOff();

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);  // 3V3_SW ON

  pinMode(USER_BUTTON_PIN, INPUT_PULLUP);
  pinMode(SPARE_GPIO_PIN,  INPUT_PULLUP);

  // 電源投入時にボタンが押されているかを、ここで1回だけ読み取る。
  // 実際のタレ実行は各種初期化（Wire/TCA9534/タレオフセット復元）が済んだ後、
  // 「ボタンが離されたこと」を確認してから行う（後述の handleBootTare()）。
  // ※ 割り込み（attachInterrupt/GPIOTE）は使わない。スリープ電流が約14µA増えるため。
  delay(20);  // チャタリング除去
  s_bootButtonHeld = (digitalRead(USER_BUTTON_PIN) == LOW);

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
  Serial.println("[BLE] init OK");
#endif

  Wire.begin();
  analogReadResolution(12);
  delay(200);

  s_errors = ERR_NONE;

  // ── DS3231 時刻自動設定（アップロード時／初回電源投入時）──────────────
  // OSF（発振停止フラグ）が立っている場合（バックアップ電池切れ・初回電源投入等で
  // 時刻が無効な場合）のみ、コンパイル時刻を自動書き込みする。
  // 時刻が有効な場合は上書きしない（WDT リセット等で頻繁に再起動しても
  // 時刻が巻き戻らないようにするため）。
  {
    bool needsSet = ds3231OscillatorStopped();
    if (needsSet) {
      uint8_t yr2, mo, day, hr, mn, sc;
      getCompileTime(yr2, mo, day, hr, mn, sc);
      bool ok = ds3231SetTime(yr2, mo, day, hr, mn, sc);
      if (ok) ds3231ClearOscillatorStopFlag();
      Serial.print("[DS3231] 時刻が無効だったためビルド時刻を書き込み: ");
      Serial.println(ok ? "OK" : "FAILED");
      if (!ok) s_errors |= ERR_DS3231_I2C;
    }
  }

  if (!tca9534Configure()) {
    s_errors |= ERR_TCA9534_I2C;
    statusErrorRed();
    Serial.println("[TCA9534] init failed");
  }

  // フラッシュから前回のタレオフセットを復元（リセット後も継続して有効にするため）
  loadTareOffsets();

  // ボタンイベント履歴は起動毎に消去する（履歴確認は Gateway 側の受信ログ CSV で行う）
  s_eventLogCount = 0;
  s_eventLogNext  = 0;
  saveEventLog();

  // 起動時にボタンが押されていたらタレを実行する（離されるのを待ってから実行）。
  // loadTareOffsets() の後に置くこと（復元したオフセットを上書きする形になるため）。
  // measureAll() の前に置くことで、直後の計測値に新しいタレが反映される。
  handleBootTare();

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
      // アドバタイズ時間窓（毎時 :00〜:02）外のサイクル。
      // 計測ログはこの上ですでに出ているが、このサイクルではBLE送信をスキップしたことを明示する。
      Serial.print("[BLE] アドバタイズ時間窓外のためスキップ TIME=");
      Serial.print(rtcOk ? rtcT.hour : 0);
      Serial.print(":");
      Serial.print(rtcOk ? rtcT.min : 0);
      Serial.print(rtcOk ? "" : "（DS3231読み取り失敗）");
      Serial.println();
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

  Serial.println("[WAKE]");

  s_errors = ERR_NONE;
  if (!tca9534Configure()) {
    s_errors |= ERR_TCA9534_I2C;
    statusErrorRed();
    Serial.println("[TCA9534] re-init failed");
  }
  // ボタン処理は loop() では行わない。
  // タレはボタンを押しながら電源ONする方式（setup() の handleBootTare()）に変更し、
  // 短押しリセットは電源スイッチ(S1)で代替するため廃止した。
  // これによりスリープ中の割り込み（GPIOTE）が不要になり、約14µA削減できる。

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
      // アドバタイズ時間窓（毎時 :00〜:02）外のサイクル。
      // 計測ログはこの上ですでに出ているが、このサイクルではBLE送信をスキップしたことを明示する。
      Serial.print("[BLE] アドバタイズ時間窓外のためスキップ TIME=");
      Serial.print(rtcOk ? rtcT.hour : 0);
      Serial.print(":");
      Serial.print(rtcOk ? rtcT.min : 0);
      Serial.print(rtcOk ? "" : "（DS3231読み取り失敗）");
      Serial.println();
    }
    deepSleep(MEASURE_INTERVAL_MIN);
#endif
  }
#endif
}
