/**
 * 豊能町 斜面崩落モニタ — Monita Flex v3.03 / XIAO nRF52840 Sense
 *
 * 【用途】
 *   斜面崩落地の樹木に取り付け、XIAO 内蔵の 6軸 IMU(LSM6DS3TR-C, I2C 0x6A)の
 *   加速度から「鉛直からの総合傾斜角」を求め、傾きの変化を Sigfox でモニタする。
 *   （まだ崩れる可能性があるため、傾きの継続監視が目的）
 *
 * 【計測仕様】
 *   ・1測定 = 加速度 ax/ay/az を 100 サンプル平均（村本建設向けファームの平均回数に合わせる）
 *   ・1サイクル 20 測定（≒4 秒。100 サンプル×delay(2)≒200ms/測定 × 20）
 *   ・各測定で 鉛直からの総合傾斜角 θ = atan2(√(ax²+ay²), az)[deg] を算出
 *   ・20 個の θ から median / max / min を求める（取付方向に依存せず全方向の傾きを検出）
 *   ・XIAO 温度(IMU 内蔵温度センサ)・電池電圧を各 1 回取得
 *   ・11 分間隔で送信（≒131 通/日 < Sigfox デューティ 140 通/日）
 *
 * 【Sigfox ペイロード 10 バイト（little-endian, LSB first）】
 *   [0-1] median傾斜角 int16  ×100 (0.01°分解能, ±327°)
 *   [2-3] max傾斜角    int16  ×100
 *   [4-5] min傾斜角    int16  ×100
 *   [6-7] XIAO温度     int16  ×10  (0.1℃)
 *   [8-9] 電池電圧     uint16 mV
 *   ※ 生存確認優先: IMU 読み取り失敗時も送信する。傾き/温度は無効値
 *      INVALID_TILT/INVALID_TEMP(=INT16_MIN, 0x8000) を入れ、電圧は実測値を送る。
 *
 * 【対象ハード】
 *   Monita Flex v3.03 基板（Sigfox 専用）。ピン正本: Monita_Flex_構成_v3.03.md
 *   ・Sigfox UART: D8=TX, D9=RX, 9600bps
 *   ・3V3_SW: D10(HIGH で周辺レール給電。計測/送信中のみ ON)
 *   ・電池分圧: A3
 *   ・傾きセンサ: XIAO 内蔵 IMU(LSM6DS3TR-C)。XIAO の 3V3 で常時給電され 3V3_SW とは独立。
 *
 * 【スリープ】
 *   nRF52840 内蔵 RTC2 + LFCLK。RTC1 は Arduino コアの FreeRTOS が使うため触らない。
 *   プリスケーラ 4095 → 8 tick/秒。
 */

#include <Arduino.h>
#include <nrf.h>            // RTC2 直接操作
#include <math.h>
#include <string.h>
#include <Wire.h>
#include <LSM6DS3.h>        // XIAO 内蔵 IMU(0x6A) 読み取り
#include <Adafruit_TinyUSB.h>

// ============================================================
// アプリ設定（ここを主に編集する）
// ============================================================

#define DEBUG_MODE       0     // 1: USB Serial デバッグログ有効。本番は 0
#define DEBUG_NO_SLEEP   0     // 1: deepSleep をスキップして即 loop() へ（DEBUG_MODE 1 時のみ）
#define DEBUG_NO_SIGFOX  0     // 1: AT$SF= を送らずログだけ（デューティサイクル節約）
#define SLEEP_MINUTES    11    // 送信間隔（分）

// Sigfox モジュールの恒久設定モード。
//   新品/LoRa モードのモジュールを一度だけ「Sigfox モード＋日本 RC3C」に設定するとき 1 にする。
//   1 で書き込み→シリアルログで「設定成功」を確認→**必ず 0 に戻して再ビルド・書き込み**する。
//   （設定は NVM に保持されるため、以後は通常運転で単純な AT$SF のみで送信できる）
#define SIGFOX_PROVISION_MODE 0

// ── 計測パラメータ ─────────────────────────────────────────────
#define SAMPLES_PER_MEASURE 100  // 1測定あたりの加速度平均サンプル数（村本ファームに合わせる）
#define MEASURE_COUNT       20   // 1サイクルの測定回数（4秒に20回）
#define SAMPLE_DELAY_MS     2    // 1サンプル間の待機（村本ファームに合わせる）

// ── Sigfox 設定 ────────────────────────────────────────────
#define SIGFOX_TX_PIN 8    // D8: XIAO TX → BRKLSM100 RX
#define SIGFOX_RX_PIN 9    // D9: XIAO RX ← BRKLSM100 TX
#define SIGFOX_BAUD   9600

// ── ピン（正本 Monita_Flex_構成_v3.03.md）──────────────────────
#define BATT_ANALOG_PIN A3   // 電池分圧
#define SW_POWER_PIN    10   // D10: MOSFET_GATE → 3V3_SW ON/OFF
#define USER_BUTTON_PIN 0    // D0: タクトスイッチ（GND ショート、内部プルアップ）

#define BOOT_BLUE_MS 500     // 電源 ON 後の青点灯時間(ms)

// XIAO 内蔵 IMU 電源ピン（LSM6DS3TR-C）。Seeed 変種で定義される。
#ifndef PIN_LSM6DS3TR_C_POWER
#warning "PIN_LSM6DS3TR_C_POWER 未定義: 内蔵 IMU 電源ピンのマクロ名をボード変種で確認すること"
#endif

// 傾き/温度の無効値センチネル（生存確認送信時に使用）
static const int16_t INVALID_TILT = INT16_MIN;  // 0x8000 = -327.68° は物理的にあり得ない
static const int16_t INVALID_TEMP = INT16_MIN;

#ifndef PI
#define PI 3.14159265358979323846
#endif

// ============================================================
// センサ（XIAO 内蔵 IMU, I2C 0x6A）
// ============================================================
LSM6DS3 imu(I2C_MODE, 0x6A);

// ============================================================
// エラーフラグ
// ============================================================
enum : uint32_t {
  ERR_NONE      = 0,
  ERR_IMU_INIT  = 1u << 0,  // IMU 初期化失敗
  ERR_SIGFOX_AT = 1u << 1,  // AT$SF= 応答に OK なし／タイムアウト
};
static uint32_t s_errors;

// D0 ボタン: ISR → メインへ押下エッジを伝える
static volatile bool s_btnFlag = false;
static void btnISR() { s_btnFlag = true; }

// ============================================================
// ステータス LED（XIAO 内蔵 RGB LED, アクティブ LOW）
// ============================================================
static void rgbBegin() {
#ifdef LED_RED
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
#endif
}

// r,g,b は 0〜255。brightness8 は全体スケール。アクティブ LOW のため反転して出力。
static void rgbShow(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness8) {
#ifdef LED_RED
  uint16_t rr = (uint16_t)r * brightness8 / 255;
  uint16_t gg = (uint16_t)g * brightness8 / 255;
  uint16_t bb = (uint16_t)b * brightness8 / 255;
  analogWrite(LED_RED,   255 - (int)rr);
  analogWrite(LED_GREEN, 255 - (int)gg);
  analogWrite(LED_BLUE,  255 - (int)bb);
#endif
}

#define RGB_BRIGHT_FULL       255
#define RGB_BRIGHT_SLEEP_BLUE 16   // スリープ中の青（低輝度）

static void rgbOff() {
#ifdef LED_RED
  analogWrite(LED_RED,   255);
  analogWrite(LED_GREEN, 255);
  analogWrite(LED_BLUE,  255);
#endif
}

static void statusBootBlueStrong() { rgbShow(0, 0, 255, RGB_BRIGHT_FULL); }
static void statusIdleBlueDim()    { rgbShow(0, 0, 255, RGB_BRIGHT_SLEEP_BLUE); }
static void statusMeasureGreen()   { rgbShow(0, 255, 0, RGB_BRIGHT_FULL); }
static void statusErrorRed()       { rgbShow(255, 0, 0, RGB_BRIGHT_FULL); }

// Sigfox 送信中の緑点滅（ノンブロッキング）
static unsigned long s_sigfoxBlinkLastMs;
static bool s_sigfoxBlinkOn;
static void statusSigfoxBlinkReset() {
  s_sigfoxBlinkLastMs = millis();
  s_sigfoxBlinkOn = true;
  rgbShow(0, 255, 0, RGB_BRIGHT_FULL);
}
static void statusSigfoxBlinkTick() {
  unsigned long now = millis();
  if (now - s_sigfoxBlinkLastMs >= 200) {
    s_sigfoxBlinkLastMs = now;
    s_sigfoxBlinkOn = !s_sigfoxBlinkOn;
    if (s_sigfoxBlinkOn) rgbShow(0, 255, 0, RGB_BRIGHT_FULL);
    else                 rgbOff();
  }
}

// ============================================================
// スリープ（RTC2 + __WFI）
//   RTC1 は FreeRTOS 用のため触らない。起床は RTC2 COMPARE0 のみ。
//   LFCLK 32768Hz / (4095+1) = 8 tick/秒。
// ============================================================
#define RTC2_PRESCALER        4095U
#define RTC2_TICKS_PER_SECOND 8U
#define RTC2_COUNTER_MASK     0x00FFFFFFU

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

// 周辺レール(3V3_SW)をオフにし、RTC2 で minutes 分待ってから復帰する。
// 待機中の D0 押下はソフトリセット（現地での手動再起動用）。
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

  // スリープ中は周辺 IC 用レールをオフ（消費電流削減）
  digitalWrite(SW_POWER_PIN, LOW);
  // Sigfox モジュール電源オフ後は UART を閉じる（フロート/リーク対策）
  Serial1.end();

#if DEBUG_MODE
  Serial.print("[RTC2 sleep] "); Serial.print(minutes); Serial.println(" min");
  Serial.flush();
#endif

  if (minutes == 0U) minutes = 1U;

  uint64_t ticks64 = (uint64_t)minutes * 60ULL * (uint64_t)RTC2_TICKS_PER_SECOND;
  if (ticks64 > RTC2_COUNTER_MASK) ticks64 = RTC2_COUNTER_MASK;
  if (ticks64 < 1ULL)              ticks64 = 1ULL;
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

  // 計測・送信フェーズ中の押下残りは破棄（スリープ中の押下のみ有効）
  s_btnFlag = false;

  while (!s_rtc2Compare0Wake) {
    __DSB();
    __WFI();
    if (s_btnFlag) {
      s_btnFlag = false;
      delay(20);  // チャタリング除去
#if DEBUG_MODE
      Serial.println("[BTN] press -> reset");
      Serial.flush();
#endif
      NVIC_SystemReset();
    }
  }

  NVIC_DisableIRQ(RTC2_IRQn);
  NRF_RTC2->INTENCLR = RTC_INTENCLR_COMPARE0_Msk;
  NRF_RTC2->TASKS_STOP = 1;
}

// ============================================================
// 計測ユーティリティ
// ============================================================

// 20 個以下の float の中央値（コピーをバブルソートして中央要素）
static float medianF(const float *a, int n) {
  float t[MEASURE_COUNT];
  memcpy(t, a, sizeof(float) * (size_t)n);
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - i - 1; j++)
      if (t[j] > t[j + 1]) { float x = t[j]; t[j] = t[j + 1]; t[j + 1] = x; }
  return t[n / 2];
}

// 内蔵 IMU 電源 ON（XIAO nRF52840 Sense）
static void imuPowerOn() {
#ifdef PIN_LSM6DS3TR_C_POWER
  pinMode(PIN_LSM6DS3TR_C_POWER, OUTPUT);
  digitalWrite(PIN_LSM6DS3TR_C_POWER, HIGH);
  delay(50);  // IMU 電源安定待ち
#endif
}

// ============================================================
// 1 サイクルの計測結果（グローバル: Sigfox 組み立てで参照）
// ============================================================
static int16_t  g_median, g_max, g_min;  // 傾斜角 ×100（0.01°）
static int16_t  g_temp;                   // XIAO 温度 ×10（0.1℃）
static uint16_t g_batt;                   // 電池電圧 mV

// 電池電圧（分圧後の ADC を実効電圧 mV に戻す。係数 1.8696 は v3.03 回路の分圧比）
static uint16_t measureBatt() {
  int raw = analogRead(BATT_ANALOG_PIN);
  float v = raw * (3.3f / 4095.0f);
  v *= 1.8696f;
  return (uint16_t)(v * 1000.0f);
}

// 傾き 20 測定 → median/max/min（度）を算出。成功時 true。
// 各測定は ax/ay/az を SAMPLES_PER_MEASURE 回平均し、鉛直からの総合傾斜角を求める。
static bool measureTilt(float &medDeg, float &maxDeg, float &minDeg) {
  float tilt[MEASURE_COUNT];

  for (int m = 0; m < MEASURE_COUNT; m++) {
    double axs = 0, ays = 0, azs = 0;
    for (int i = 0; i < SAMPLES_PER_MEASURE; i++) {
      axs += imu.readFloatAccelX();
      ays += imu.readFloatAccelY();
      azs += imu.readFloatAccelZ();
      delay(SAMPLE_DELAY_MS);
    }
    float ax = (float)(axs / SAMPLES_PER_MEASURE);
    float ay = (float)(ays / SAMPLES_PER_MEASURE);
    float az = (float)(azs / SAMPLES_PER_MEASURE);

    // 鉛直(重力)軸 Z からの総合傾斜角。取付方向に依存せず全方向の傾きを検出。
    tilt[m] = atan2f(sqrtf(ax * ax + ay * ay), az) * 180.0f / (float)PI;

    // 全サンプルが 0 → IMU 未応答とみなす（生存確認送信へ）
    if (!isfinite(tilt[m]) || (ax == 0.0f && ay == 0.0f && az == 0.0f)) {
      return false;
    }
  }

  medDeg = medianF(tilt, MEASURE_COUNT);
  maxDeg = tilt[0];
  minDeg = tilt[0];
  for (int m = 1; m < MEASURE_COUNT; m++) {
    if (tilt[m] > maxDeg) maxDeg = tilt[m];
    if (tilt[m] < minDeg) minDeg = tilt[m];
  }
  return true;
}

// 1 サイクル分を計測して g_* に格納。IMU 失敗時も電圧は取得し、傾き/温度は無効値。
static void measureAll() {
#if DEBUG_MODE
  Serial.println("----- MEASURE START -----");
#endif
  statusMeasureGreen();

  // 電圧は IMU の成否に関わらず取得（生存確認）
  g_batt = measureBatt();

  float med = 0, mx = 0, mn = 0;
  bool ok = ((s_errors & ERR_IMU_INIT) == 0) && measureTilt(med, mx, mn);

  if (ok) {
    g_median = (int16_t)lroundf(med * 100.0f);
    g_max    = (int16_t)lroundf(mx  * 100.0f);
    g_min    = (int16_t)lroundf(mn  * 100.0f);
    float tC = imu.readTempC();
    g_temp   = isfinite(tC) ? (int16_t)lroundf(tC * 10.0f) : INVALID_TEMP;
  } else {
    // 生存確認: 傾き/温度は無効値、電圧のみ実測を送る
    g_median = g_max = g_min = INVALID_TILT;
    g_temp   = INVALID_TEMP;
#if DEBUG_MODE
    Serial.println("[IMU] measure failed -> liveness payload (tilt/temp invalid)");
#endif
  }

#if DEBUG_MODE
  Serial.printf("[RESULT] median=%d max=%d min=%d (x100 deg) temp=%d(x10) batt=%umV\n",
                g_median, g_max, g_min, g_temp, g_batt);
#endif
}

// ============================================================
// Sigfox ペイロード用: 16bit を little-endian の 4 桁 hex に
// ============================================================
static String hx4(uint16_t u) {
  char b[5];
  snprintf(b, 5, "%02X%02X", (uint8_t)(u & 0xFF), (uint8_t)(u >> 8));
  return String(b);
}

// ============================================================
// Sigfox UART（Serial1）
// ============================================================
static String sendAT(String cmd, int waitMs = 2000) {
  // 送信前に受信バッファの残バイト（前コマンドの応答・起動バナー残渣）を破棄し、
  // 次の応答判定が古いデータで汚れないようにする。
  while (Serial1.available()) Serial1.read();

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

// AT を投げて OK が返る（モジュール準備完了）まで待つ。成功時 true。
static bool sigfoxWaitReady(uint32_t timeoutMs) {
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (sendAT("AT", 1000).indexOf("OK") >= 0) return true;
#if DEBUG_MODE
    Serial.println("[SIGFOX] waiting for module ready...");
#endif
  }
  return false;
}

// ── Sigfox モジュールの恒久設定（一度だけ実行）────────────────────────────
// BRKLSM100(muRata LSM100A) は工場出荷時「LoRa モード」で、その状態では AT$SF など
// Sigfox 系 "$" コマンドが AT_ERROR になる。動作実績のある v3.03_sigfox ファームは
// AT$SF だけで送信できているが、これは対象モジュールが「Sigfox モード＋RC3C」を
// NVM に恒久設定済みだから。本モジュールも一度この関数で設定すれば以後は保持され、
// 通常運転では単純な AT$SF のみで送信できる。
//   AT+MODE=0 … Sigfox モードへ切替（LoRa=1 / Sigfox=0）。切替時はモジュールが再起動し
//               起動バナー（...APPLICATION READY / BOOTALERT）を返す（"OK" は返らない）。
//   AT$RC=3C  … 日本リージョン RC3C（"3" 単独ではなく "3C"）。既定は RC1。
// ※ SIGFOX_PROVISION_MODE=1 のときのみ setup から呼ぶ。設定後は 0 に戻して再ビルドする。
#if SIGFOX_PROVISION_MODE
static void sigfoxProvision() {
  Serial.println("=== Sigfox 恒久設定モード (SIGFOX_PROVISION_MODE=1) ===");

  if (!sigfoxWaitReady(10000)) {
    Serial.println("[PROV] モジュール未応答: 配線/電源を確認してください");
    statusErrorRed();
    return;
  }

  Serial.print("[PROV] 現在のモード応答: ");
  Serial.println(sendAT("AT+MODE=?", 2000));

  // Sigfox モードへ切替（LoRa→Sigfox は再起動する）
  Serial.println("[PROV] AT+MODE=0 → Sigfox モードへ切替（再起動）");
  sendAT("AT+MODE=0", 5000);
  delay(2000);                       // 起動バナー出力待ち
  if (!sigfoxWaitReady(12000)) {     // 再起動完了を疎通で待つ
    Serial.println("[PROV] 切替後の再起動待ちタイムアウト");
    statusErrorRed();
    return;
  }
  Serial.print("[PROV] 切替後モード応答: ");
  Serial.println(sendAT("AT+MODE=?", 2000));

  // 日本リージョン RC3C（SFX_RC3C=3C）
  Serial.println("[PROV] AT$RC=3C → 日本リージョン設定");
  String r = sendAT("AT$RC=3C", 4000);
  if (r.indexOf("READY") >= 0 || r.indexOf("BOOTALERT") >= 0) {
    sigfoxWaitReady(10000);          // 個体により再起動する場合
  }

  // 設定を確実に反映・NVM 保持させるため MCU リセット（AT+MODE は "After a MCU reset" で反映）
  Serial.println("[PROV] ATZ → リセットして設定を反映");
  sendAT("ATZ", 3000);
  delay(2000);
  if (!sigfoxWaitReady(12000)) {
    Serial.println("[PROV] リセット後の再起動待ちタイムアウト");
    statusErrorRed();
    return;
  }

  Serial.print("[PROV] RC 確認応答 (AT$RC=?): ");
  Serial.println(sendAT("AT$RC=?", 2000));   // ここが 3C なら日本リージョン設定 OK

  // テスト送信（1 バイト）。ログの "RF at Freq" が 92x MHz なら日本 RC3、868 MHz なら RC1 のまま。
  Serial.println("[PROV] テスト送信 AT$SF=00（RF Freq が 92xMHz なら日本設定成功）");
  String tx = sendAT("AT$SF=00", 15000);
  if (tx.indexOf("OK") >= 0) {
    Serial.println("[PROV] テスト送信 OK ✅ — 上の RF Freq が 92xMHz か必ず確認してください");
  } else {
    Serial.println("[PROV] テスト送信 NG ❌ — 応答を確認（RF/バックエンド登録も要確認）");
  }

  Serial.println("=== 設定完了。SIGFOX_PROVISION_MODE を 0 に戻して再ビルド・書き込みしてください ===");
}
#endif  // SIGFOX_PROVISION_MODE

// g_* を連結した 10 バイトペイロードで AT$SF= を送信（生存確認のため常に送信を試みる）
static void sendSigfox() {

  String payload = hx4((uint16_t)g_median)
                 + hx4((uint16_t)g_max)
                 + hx4((uint16_t)g_min)
                 + hx4((uint16_t)g_temp)
                 + hx4(g_batt);

#if DEBUG_MODE && DEBUG_NO_SIGFOX
  Serial.print("[SIGFOX] TX SKIPPED (DEBUG_NO_SIGFOX): AT$SF=");
  Serial.println(payload);
  return;
#endif

  // モジュール準備完了待ち（AT ping）。未応答なら今回はスキップ（次サイクル再送）。
  // ※ モジュールは事前に SIGFOX_PROVISION_MODE で「Sigfox モード＋RC3C」を恒久設定済みの前提。
  //    未設定（LoRa モード）だと AT$SF が AT_ERROR になる（動作実績のある v3.03 と同じ単純送信）。
  if (!sigfoxWaitReady(10000)) {
#if DEBUG_MODE
    Serial.println("[SIGFOX] module not ready: TX skipped (retry next cycle)");
#endif
    return;
  }

  String cmd = "AT$SF=" + payload;
#if DEBUG_MODE
  Serial.print("[SIGFOX] "); Serial.println(cmd);
#endif

  statusSigfoxBlinkReset();
  // RC3C は LBT（Listen Before Talk）で送信に時間がかかるため長めに待つ
  String result = sendAT(cmd, 15000);

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
// 共通初期化（setup / loop 起床の両方で呼ぶ）
// ============================================================
static void powerUpPeripherals() {
  digitalWrite(SW_POWER_PIN, HIGH);  // 3V3_SW ON

  // スリープ中に Serial1.end() しているため再初期化（setPins は RX, TX の順）
  Serial1.setPins(SIGFOX_RX_PIN, SIGFOX_TX_PIN);
  Serial1.begin(SIGFOX_BAUD);
  delay(3000);  // BRKLSM100 コールドスタート待ち

  Wire.begin();
  analogReadResolution(12);
  delay(100);
}

// ============================================================
// Arduino エントリ
// ============================================================
void setup() {
  rgbBegin();
  rgbOff();

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);

  pinMode(USER_BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(USER_BUTTON_PIN), btnISR, FALLING);

  statusBootBlueStrong();
  delay((unsigned long)BOOT_BLUE_MS);
  statusIdleBlueDim();

#if DEBUG_MODE
  Serial.begin(115200);
  delay(2000);  // USB CDC 安定待ち
  Serial.println("=== 豊能町 斜面崩落モニタ START ===");
  Serial.flush();
#endif

  imuPowerOn();  // 内蔵 IMU 電源 ON（Sense）

  Serial1.setPins(SIGFOX_RX_PIN, SIGFOX_TX_PIN);
  Serial1.begin(SIGFOX_BAUD);
  delay(3000);  // BRKLSM100 コールドスタート待ち

  Wire.begin();
  analogReadResolution(12);
  delay(200);

  s_errors = ERR_NONE;

#if SIGFOX_PROVISION_MODE
  // ── Sigfox モジュール恒久設定（一度だけ）──────────────────────────────
  // 設定後は電源を切らずに待機（NVM 反映済み・LED 緑点滅で完了表示）。
  // 完了を確認したら SIGFOX_PROVISION_MODE=0 に戻して再ビルドすること。
  sigfoxProvision();
  while (1) {
    statusSigfoxBlinkTick();
    delay(10);
  }
#endif

  if (imu.begin() != 0) {
    s_errors |= ERR_IMU_INIT;
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[IMU] LSM6DS3 初期化失敗");
#endif
  } else {
#if DEBUG_MODE
    Serial.println("[IMU] LSM6DS3 初期化完了 (0x6A)");
#endif
  }

  measureAll();
  sendSigfox();
  deepSleep(SLEEP_MINUTES);
}

void loop() {
  statusIdleBlueDim();
  powerUpPeripherals();

#if DEBUG_MODE
  Serial.println("[WAKE]");
#endif

  s_errors = ERR_NONE;

  // 内蔵 IMU は XIAO 3V3 給電のためスリープ中も電源保持されるが、
  // 電源ピンを冪等に HIGH に戻し、毎サイクル begin() で再初期化する。
  imuPowerOn();
  if (imu.begin() != 0) {
    s_errors |= ERR_IMU_INIT;
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[IMU] re-init 失敗");
#endif
  }

  measureAll();
  sendSigfox();
  deepSleep(SLEEP_MINUTES);
}
