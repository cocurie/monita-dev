/**
 * Monita LoRa 検証 Step25: ダウンリンク受信テスト用 子機（ブレッドボード版）
 *
 * 【目的】
 *   Gateway→子機方向のLoRaダウンリンク（時刻補正・送信頻度・ひずみ計測の
 *   平均/メジアン回数の変更）を受信・検証・適用・フラッシュ保存する部分だけを
 *   切り出して検証する。test_sketches/22_lora_multi_child をベースにしている。
 *
 * 【22_lora_multi_childとの違い】
 *   - 毎回のアップリンク送信後、DOWNLINK_RX_WINDOW_MS の間だけLoRa受信を待つ
 *   - 受信したフレームがダウンリンクコマンド（Company ID・PktType一致）であれば
 *     内容を検証し、送信頻度／平均回数／メジアン回数をランタイム変数に適用する
 *   - 上記の変更値はInternalFS（フラッシュ）に保存し、リセット後も保持する
 *   - DS3231が無い簡易ブレッドボード版のため、時刻補正フラグを受信した場合は
 *     「本来ならDS3231に書き込む内容」をログ表示するだけに留める
 *     （実装をv3.10_loraへ統合する際にDS3231書き込みへ差し替える）
 *
 * 【配線】22_lora_multi_childと同一
 *   XIAO D8 (TX) → E220 RXD / XIAO D9 (RX) ← E220 TXD
 *   XIAO D1 → E220 M0 / XIAO D2 → E220 M1
 *   XIAO 3V3 → E220 VCC（常時給電）/ XIAO GND → E220 GND
 *
 * 【★書き込み前に必ず変更すること】
 *   DEVICE_ID を台ごとに重複しないユニークな値にする。
 *
 * 【ダウンリンクフレーム仕様（v1）】
 *   フレーム全体: [0xAA][LEN][payload...][checksum]（既存アップリンクと同一の透過送信形式）
 *   REG3=0x80でRSSIバイト有効化しているため、チェックサムの後にRSSIバイトが1つ付加される。
 *
 *   payload（15バイト）:
 *     [0-1]  Company ID固定値（DOWNLINK_COMPANY_ID）。不一致なら即破棄
 *     [2]    PktType = 0x81（ダウンリンクコマンド識別）
 *     [3]    宛先DeviceID。自分のDEVICE_IDと一致した場合のみ処理
 *     [4]    フラグ（ビットマスク）
 *              bit0: 時刻補正あり（[5-10]が有効）
 *              bit1: 送信頻度変更あり（[11-12]が有効）
 *              bit2: 平均/メジアン回数変更あり（[13-14]が有効）
 *     [5]    year（2000+n、例: 26 → 2026年）
 *     [6]    month
 *     [7]    day
 *     [8]    hour
 *     [9]    min
 *     [10]   sec
 *     [11-12] 送信頻度（分）uint16 LE
 *     [13]   平均回数（SAMPLES_PER_AVG相当）
 *     [14]   メジアン回数（MEASURE_COUNT相当）
 */

#include <Arduino.h>
#include <nrf.h>
#include <Adafruit_TinyUSB.h>       // USB CDC（Serial）。USE_TINYUSBビルド時に必要
#include <InternalFileSystem.h>     // ランタイム設定のフラッシュ保存に使用
using namespace Adafruit_LittleFS_Namespace;

// ============================================================
// ▼ 設定（台ごとに変更するのは基本的に DEVICE_ID のみ）
// ============================================================
// ★★★ 書き込み前に必ず変更: 台ごとに重複しない値にする ★★★
static const uint8_t DEVICE_ID = 0x08;
static const uint8_t FW_VERSION = 1;

#define DEBUG_MODE          1     // 1: USB Serialデバッグログ有効

// ランタイム設定のデフォルト値（フラッシュに保存が無い初回起動時に使う）
#define DEFAULT_SLEEP_MINUTES   4     // 送信間隔（分）
#define DEFAULT_SAMPLES_PER_AVG 5     // ひずみ計測: 平均サンプル数
#define DEFAULT_MEASURE_COUNT   5     // ひずみ計測: メジアンをとる回数

#define BOOT_BLUE_MS         500

// 毎回のアップリンク送信後、ダウンリンクを待つ時間（ms）。
// 消費電流とのトレードオフ。本番機ではこれを最小限に抑える設計にする。
#define DOWNLINK_RX_WINDOW_MS 10000UL

// ============================================================
// ピン割当（ブレッドボード配線。22_lora_multi_childと同一）
// ============================================================
#define LORA_TX_PIN 8
#define LORA_RX_PIN 9
#define LORA_M0_PIN 1
#define LORA_M1_PIN 2
#define LORA_UART_BAUD 9600

// ============================================================
// ステータスLED（XIAO nRF52840 Sense 内蔵の離散RGB。アクティブLOW）
// ============================================================
static void rgbHwBegin() {
#ifdef LED_RED
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
#endif
}
static void rgbHwShow(uint8_t r, uint8_t g, uint8_t b) {
#ifdef LED_RED
  analogWrite(LED_RED,   255 - r);
  analogWrite(LED_GREEN, 255 - g);
  analogWrite(LED_BLUE,  255 - b);
#endif
}
static void rgbOff()          { rgbHwShow(0, 0, 0); }
static void statusBootBlue()  { rgbHwShow(0, 0, 255); }
static void statusSendGreen() { rgbHwShow(0, 255, 0); }
static void statusRxWindow()  { rgbHwShow(255, 255, 0); }  // 黄: ダウンリンク受信待ち中
static void statusRxApplied() { rgbHwShow(0, 255, 255); }  // シアン: ダウンリンク適用成功
static void statusErrorRed()  { rgbHwShow(255, 0, 0); }

// ============================================================
// ランタイム設定（ダウンリンクで変更可能。フラッシュに保存してリセット後も保持）
// ============================================================
struct ChildSettings {
  uint16_t sleepMinutes;
  uint8_t  samplesPerAvg;
  uint8_t  measureCount;
};

static ChildSettings s_settings = {
  DEFAULT_SLEEP_MINUTES,
  DEFAULT_SAMPLES_PER_AVG,
  DEFAULT_MEASURE_COUNT,
};

static const char SETTINGS_FILE[] = "/child_settings.bin";

static void saveSettings() {
  if (!InternalFS.begin()) return;
  InternalFS.remove(SETTINGS_FILE);
  File f(InternalFS);
  if (f.open(SETTINGS_FILE, FILE_O_WRITE)) {
    f.write((const uint8_t*)&s_settings, sizeof(s_settings));
    f.close();
#if DEBUG_MODE
    Serial.println("[SETTINGS] flashへ保存しました");
#endif
  }
}

static void loadSettings() {
  if (!InternalFS.begin()) return;
  File f(InternalFS);
  if (f.open(SETTINGS_FILE, FILE_O_READ)) {
    if ((size_t)f.size() == sizeof(s_settings)) {
      f.read((uint8_t*)&s_settings, sizeof(s_settings));
#if DEBUG_MODE
      Serial.println("[SETTINGS] flashから復元しました");
#endif
    }
    f.close();
  }
#if DEBUG_MODE
  Serial.print("[SETTINGS] sleepMinutes="); Serial.print(s_settings.sleepMinutes);
  Serial.print(" samplesPerAvg="); Serial.print(s_settings.samplesPerAvg);
  Serial.print(" measureCount="); Serial.println(s_settings.measureCount);
#endif
}

// ============================================================
// ウォッチドッグタイマー（nRF52840 内蔵 WDT）
// ============================================================
static uint32_t const WDT_TIMEOUT_MS = 10UL * 60UL * 1000UL;  // 10分（送信間隔+RXウィンドウに余裕を持たせた値）

static void wdtInit(uint32_t timeoutMs) {
  NRF_WDT->CONFIG  = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);
  NRF_WDT->CRV     = (uint32_t)((uint64_t)timeoutMs * 32768ULL / 1000ULL);
  NRF_WDT->RREN    = WDT_RREN_RR0_Msk;
  NRF_WDT->TASKS_START = 1;
}
static inline void wdtFeed() { NRF_WDT->RR[0] = WDT_RR_RR_Reload; }

// ============================================================
// スリープ（RTC2 + WFI）。22_lora_multi_childと同一
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

static void deepSleep(uint32_t minutes) {
  wdtFeed();

#if DEBUG_MODE
  Serial.println("[Sleep before wait]");
  delay(1000);
#endif

  rgbOff();
  Serial1.end();

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

  while (!s_rtc2Compare0Wake) {
    __DSB();
    __WFI();
  }

  NVIC_DisableIRQ(RTC2_IRQn);
  NRF_RTC2->INTENCLR = RTC_INTENCLR_COMPARE0_Msk;
  NRF_RTC2->TASKS_STOP = 1;
}

// ============================================================
// LoRa（E220-900T22S(JP)）— v3.10_loraと同じ設定値・フレーム形式
// ============================================================
#define LORA_MODE_SWITCH_DELAY_MS 100U
#define LORA_CFG_REG_START 0x00
#define LORA_CFG_REG_LEN   6

// Gateway実機（gateway_v1.1）と同一値。ここを変える場合はGateway側も合わせて変更すること
static const uint8_t LORA_CFG_ADDH = 0x00;
static const uint8_t LORA_CFG_ADDL = 0x00;
static const uint8_t LORA_CFG_REG0 = 0x68;  // UART9600bps + エア速度(SF7/BW125kHz)
static const uint8_t LORA_CFG_REG1 = 0x01;  // ペイロード長200B/RSSIノイズ無効/送信出力13dBm
static const uint8_t LORA_CFG_REG2 = 0x00;  // チャンネル0
static const uint8_t LORA_CFG_REG3 = 0x80;  // RSSIバイト有効化ON/透過送信モード

static void loraSetMode(bool m0High, bool m1High) {
  digitalWrite(LORA_M0_PIN, m0High ? HIGH : LOW);
  digitalWrite(LORA_M1_PIN, m1High ? HIGH : LOW);
  delay(LORA_MODE_SWITCH_DELAY_MS);
}
static inline void loraModeNormal() { loraSetMode(false, false); }
static inline void loraModeConfig() { loraSetMode(true, true); }

static bool loraReadConfig(uint8_t *out6) {
  while (Serial1.available()) Serial1.read();
  Serial1.write((uint8_t)0xC1);
  Serial1.write((uint8_t)LORA_CFG_REG_START);
  Serial1.write((uint8_t)LORA_CFG_REG_LEN);

  const int respLen = 3 + LORA_CFG_REG_LEN;
  uint8_t resp[3 + LORA_CFG_REG_LEN];
  int idx = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 500UL && idx < respLen) {
    if (Serial1.available()) resp[idx++] = (uint8_t)Serial1.read();
  }
  if (idx < respLen) return false;
  if (resp[0] != 0xC1) return false;
  memcpy(out6, &resp[3], LORA_CFG_REG_LEN);
  return true;
}

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
  delay(200);
  unsigned long t0 = millis();
  while (millis() - t0 < 300UL) { while (Serial1.available()) Serial1.read(); }
}

static bool loraCheckAndConfigureOnce() {
  loraModeConfig();

  uint8_t cur[LORA_CFG_REG_LEN] = {0};
  bool readOk = loraReadConfig(cur);
  bool matches = readOk &&
      cur[0] == LORA_CFG_ADDH && cur[1] == LORA_CFG_ADDL &&
      cur[2] == LORA_CFG_REG0 && cur[3] == LORA_CFG_REG1 &&
      cur[4] == LORA_CFG_REG2 && cur[5] == LORA_CFG_REG3;

#if DEBUG_MODE
  Serial.print("[LORA] config read ");
  Serial.println(!readOk ? "失敗" : (matches ? "一致" : "不一致→書込"));
#endif

  if (!readOk) { loraModeNormal(); return false; }

  if (!matches) {
    loraWriteConfig();
    uint8_t verify[LORA_CFG_REG_LEN] = {0};
    bool verifyOk = loraReadConfig(verify) &&
        verify[0] == LORA_CFG_ADDH && verify[1] == LORA_CFG_ADDL &&
        verify[2] == LORA_CFG_REG0 && verify[3] == LORA_CFG_REG1 &&
        verify[4] == LORA_CFG_REG2 && verify[5] == LORA_CFG_REG3;
#if DEBUG_MODE
    Serial.println(verifyOk ? "[LORA] config write 確認OK" : "[LORA] config write 確認NG");
#endif
    if (!verifyOk) { loraModeNormal(); return false; }
  }

  loraModeNormal();
  return true;
}

#define LORA_CONFIG_RETRY 5
#define LORA_CONFIG_RETRY_DELAY_MS 500U

static bool loraCheckAndConfigure() {
  for (uint8_t attempt = 0; attempt < LORA_CONFIG_RETRY; attempt++) {
    if (loraCheckAndConfigureOnce()) return true;
#if DEBUG_MODE
    Serial.print("[LORA] config check 失敗、リトライ ");
    Serial.print(attempt + 1);
    Serial.print("/");
    Serial.println(LORA_CONFIG_RETRY);
#endif
    delay(LORA_CONFIG_RETRY_DELAY_MS);
  }
  return false;
}

static void loraPrintFrameHex(const uint8_t *msd, uint8_t msdLen, uint8_t sum) {
  Serial.print("[LORA] TXフレーム(HEX): AA ");
  if (msdLen < 0x10) Serial.print('0');
  Serial.print(msdLen, HEX);
  Serial.print(' ');
  for (uint8_t i = 0; i < msdLen; i++) {
    if (msd[i] < 0x10) Serial.print('0');
    Serial.print(msd[i], HEX);
    Serial.print(' ');
  }
  if (sum < 0x10) Serial.print('0');
  Serial.println(sum, HEX);
}

static void loraSendFrame(const uint8_t *msd, uint8_t msdLen) {
  uint8_t sum = (uint8_t)(0xAAU + msdLen);
  for (uint8_t i = 0; i < msdLen; i++) sum = (uint8_t)(sum + msd[i]);

#if DEBUG_MODE
  loraPrintFrameHex(msd, msdLen, sum);
#endif

  Serial1.write((uint8_t)0xAA);
  Serial1.write(msdLen);
  for (uint8_t i = 0; i < msdLen; i++) Serial1.write(msd[i]);
  Serial1.write(sum);
  Serial1.flush();
}

#define LORA_TX_COMPLETE_DELAY_MS 300U
#define LORA_TX_REPEAT 2
#define LORA_TX_REPEAT_GAP_MS 100U

// ============================================================
// ダウンリンク受信・パース
//
// [0xAA][LEN][payload...][checksum][RSSI]（REG3=0x80でRSSIバイトが付加される）
// を、GatewayのloraPoll()と同じ考え方のバイト単位ステートマシンで受信する。
// payload内はCompany ID・PktTypeで検証してから中身を適用する。
// ============================================================
static const uint16_t DOWNLINK_COMPANY_ID = 0xC0DE;  // プロジェクト固定値。要: Gateway側と一致させる
static const uint8_t  DOWNLINK_PKT_TYPE   = 0x81;
static const uint8_t  DOWNLINK_PAYLOAD_LEN = 15;  // [0-1]CompanyID [2]PktType [3]DeviceID [4]Flags [5-10]時刻 [11-12]SleepMin [13]Avg [14]Median

enum DownlinkFlag {
  DL_FLAG_TIME       = 1u << 0,
  DL_FLAG_SLEEP_MIN  = 1u << 1,
  DL_FLAG_AVG_MEDIAN = 1u << 2,
};

enum LoraRxState { LORA_WAIT_SYNC, LORA_WAIT_LEN, LORA_WAIT_BODY, LORA_WAIT_CKSUM, LORA_WAIT_RSSI };

// 受信したダウンリンクフレームを検証・適用する。
// true: 有効なダウンリンクとして処理した（適用有無に関わらず、Company ID一致でパース成功した場合）
static bool applyDownlinkPayload(const uint8_t *payload, uint8_t len) {
  if (len != DOWNLINK_PAYLOAD_LEN) {
#if DEBUG_MODE
    Serial.print("[DOWNLINK] payload長不一致: "); Serial.println(len);
#endif
    return false;
  }

  uint16_t companyId = ((uint16_t)payload[0] << 8) | payload[1];
  if (companyId != DOWNLINK_COMPANY_ID) {
#if DEBUG_MODE
    Serial.print("[DOWNLINK] Company ID不一致（無関係な信号として破棄）: 0x");
    Serial.println(companyId, HEX);
#endif
    return false;
  }

  uint8_t pktType = payload[2];
  if (pktType != DOWNLINK_PKT_TYPE) {
#if DEBUG_MODE
    Serial.print("[DOWNLINK] PktType不一致: 0x"); Serial.println(pktType, HEX);
#endif
    return false;
  }

  uint8_t destId = payload[3];
  if (destId != DEVICE_ID) {
#if DEBUG_MODE
    Serial.print("[DOWNLINK] 宛先違い（自分宛てではない）: 0x"); Serial.println(destId, HEX);
#endif
    return false;
  }

  uint8_t flags = payload[4];
  bool changed = false;

#if DEBUG_MODE
  Serial.println("[DOWNLINK] ★自分宛ての有効なコマンドを受信しました");
#endif

  if (flags & DL_FLAG_TIME) {
    uint16_t year = 2000U + payload[5];
    uint8_t month = payload[6], day = payload[7];
    uint8_t hour  = payload[8], minute = payload[9], sec = payload[10];
#if DEBUG_MODE
    // このブレッドボード版にはDS3231が無いため、書き込み内容をログ表示するだけに留める。
    // v3.10_loraへ統合する際はここを ds3231SetTime() 等への書き込みに差し替える。
    Serial.print("[DOWNLINK] 時刻補正（DS3231未搭載のためログのみ）: ");
    Serial.print(year); Serial.print("-");
    Serial.print(month); Serial.print("-");
    Serial.print(day); Serial.print(" ");
    Serial.print(hour); Serial.print(":");
    Serial.print(minute); Serial.print(":");
    Serial.println(sec);
#endif
  }

  if (flags & DL_FLAG_SLEEP_MIN) {
    uint16_t newSleepMin = ((uint16_t)payload[11] << 8) | payload[12];  // ★注: 下記TXコードもLE/BE統一
    if (newSleepMin > 0 && newSleepMin != s_settings.sleepMinutes) {
      s_settings.sleepMinutes = newSleepMin;
      changed = true;
#if DEBUG_MODE
      Serial.print("[DOWNLINK] 送信頻度を変更: "); Serial.print(newSleepMin); Serial.println("分");
#endif
    }
  }

  if (flags & DL_FLAG_AVG_MEDIAN) {
    uint8_t newAvg = payload[13];
    uint8_t newMedian = payload[14];
    if (newAvg > 0 && newMedian > 0 &&
        (newAvg != s_settings.samplesPerAvg || newMedian != s_settings.measureCount)) {
      s_settings.samplesPerAvg = newAvg;
      s_settings.measureCount  = newMedian;
      changed = true;
#if DEBUG_MODE
      Serial.print("[DOWNLINK] 平均回数/メジアン回数を変更: ");
      Serial.print(newAvg); Serial.print(" / "); Serial.println(newMedian);
#endif
    }
  }

  if (changed) {
    saveSettings();
    statusRxApplied();
    delay(1000);
  }

  return true;
}

// DOWNLINK_RX_WINDOW_MS の間だけSerial1を監視し、ダウンリンクフレームを待つ。
// 受信できなければ何もせずタイムアウトで戻る（消費電流はこの待ち時間に比例する）。
static void downlinkRxWindow(uint32_t windowMs) {
  statusRxWindow();
#if DEBUG_MODE
  Serial.print("[DOWNLINK] RXウィンドウ開始 "); Serial.print(windowMs); Serial.println("ms");
#endif

  LoraRxState state = LORA_WAIT_SYNC;
  uint8_t len = 0, idx = 0, sum = 0, rxByte = 0;
  uint8_t body[32];  // payload最大32バイト想定（今回は15バイト）
  bool got = false;

  unsigned long t0 = millis();
  while (millis() - t0 < windowMs && !got) {
    while (Serial1.available()) {
      rxByte = (uint8_t)Serial1.read();
      switch (state) {
        case LORA_WAIT_SYNC:
          if (rxByte == 0xAA) { sum = rxByte; state = LORA_WAIT_LEN; }
          break;
        case LORA_WAIT_LEN:
          len = rxByte;
          sum = (uint8_t)(sum + rxByte);
          idx = 0;
          if (len == 0 || len > sizeof(body)) { state = LORA_WAIT_SYNC; break; }
          state = LORA_WAIT_BODY;
          break;
        case LORA_WAIT_BODY:
          body[idx++] = rxByte;
          sum = (uint8_t)(sum + rxByte);
          if (idx >= len) state = LORA_WAIT_CKSUM;
          break;
        case LORA_WAIT_CKSUM:
          if (rxByte == sum) {
            state = LORA_WAIT_RSSI;  // REG3=0x80によりRSSIバイトが後続する
          } else {
#if DEBUG_MODE
            Serial.println("[DOWNLINK] checksum不一致、破棄");
#endif
            state = LORA_WAIT_SYNC;
          }
          break;
        case LORA_WAIT_RSSI:
          // RSSIバイト自体は今回未使用（ログにだけ出す）
#if DEBUG_MODE
          Serial.print("[DOWNLINK] RSSI(raw)="); Serial.println(rxByte);
#endif
          applyDownlinkPayload(body, len);
          got = true;
          state = LORA_WAIT_SYNC;
          break;
      }
      if (got) break;
    }
  }

#if DEBUG_MODE
  if (!got) Serial.println("[DOWNLINK] RXウィンドウ内に受信なし");
#endif
  rgbOff();
}

// ============================================================
// ダミーデータ送信
// ============================================================
static uint32_t s_dummyCounter = 0;
static const uint16_t DUMMY_BATT_MV = 3700;

static void sendLoRaDummy() {
  if (!loraCheckAndConfigure()) {
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[LORA] config check failed, TX skipped");
#endif
    return;
  }

  s_dummyCounter++;
  int16_t ch[4] = {
    (int16_t)(s_dummyCounter),
    (int16_t)(s_dummyCounter + 100),
    (int16_t)(s_dummyCounter + 200),
    (int16_t)(s_dummyCounter + 300),
  };

  uint8_t msd[19];
  msd[0] = 0x04;  // Pkt type: Monita Flex v3.10 LoRa（アップリンクと共通）
  msd[1] = DEVICE_ID;
  msd[2] = FW_VERSION;
  for (int i = 0; i < 4; i++) {
    msd[3 + i * 2]     = (uint8_t)(ch[i] & 0xFF);
    msd[3 + i * 2 + 1] = (uint8_t)((ch[i] >> 8) & 0xFF);
  }
  msd[11] = (uint8_t)(DUMMY_BATT_MV & 0xFF);
  msd[12] = (uint8_t)((DUMMY_BATT_MV >> 8) & 0xFF);
  msd[13] = 0;
  msd[14] = 0;
  for (int i = 0; i < 4; i++) msd[15 + i] = 0;

  statusSendGreen();
  for (uint8_t rep = 0; rep < LORA_TX_REPEAT; rep++) {
    loraSendFrame(msd, sizeof(msd));
    delay(LORA_TX_COMPLETE_DELAY_MS);
    if (rep + 1 < LORA_TX_REPEAT) delay(LORA_TX_REPEAT_GAP_MS);
  }

#if DEBUG_MODE
  Serial.print("[LORA] TX DeviceID=0x"); Serial.print(DEVICE_ID, HEX);
  Serial.print(" counter="); Serial.print(s_dummyCounter);
  Serial.print(" CH="); for (int i = 0; i < 4; i++) { Serial.print(ch[i]); Serial.print(" "); }
  Serial.println();
#endif

  // 送信直後、ダウンリンクコマンドが来ていないか短時間だけ受信を待つ。
  // E220は透過モードのままなのでM0/M1は変更不要（送受信兼用）。
  downlinkRxWindow(DOWNLINK_RX_WINDOW_MS);
}

// ============================================================
// Arduino エントリ
// ============================================================
static void loraUartBegin() {
  Serial1.setPins(LORA_RX_PIN, LORA_TX_PIN);
  Serial1.begin(LORA_UART_BAUD);
  delay(500);
}

void setup() {
  wdtInit(WDT_TIMEOUT_MS);

  rgbHwBegin();
  statusBootBlue();
  delay(BOOT_BLUE_MS);
  rgbOff();

  pinMode(LORA_M0_PIN, OUTPUT);
  pinMode(LORA_M1_PIN, OUTPUT);
  digitalWrite(LORA_M0_PIN, LOW);
  digitalWrite(LORA_M1_PIN, LOW);

#if DEBUG_MODE
  Serial.begin(115200);
  for (int rep = 0; rep < 3; rep++) {
    delay(1000);
    Serial.println("\n[Step25] LoRaダウンリンク受信テスト 子機起動");
    Serial.print("DeviceID=0x"); Serial.println(DEVICE_ID, HEX);
  }
#endif

  loadSettings();
  loraUartBegin();

  sendLoRaDummy();
  deepSleep(s_settings.sleepMinutes);
}

void loop() {
  wdtFeed();
  loraUartBegin();

#if DEBUG_MODE
  Serial.println("[WAKE]");
#endif

  sendLoRaDummy();
  deepSleep(s_settings.sleepMinutes);
}
