/**
 * Monita Flex v3.02 — 検証 Step12: BLE親機（スキャン・SD カード記録）
 *
 * 動作:
 *   1. 初回スキャン: 120秒（子機の起動タイミングが不明なため長めに設定）
 *   2. 2回目以降: 子機の「次回計測まで秒数」を使いスキャン開始タイミングを決定
 *      スキャン開始 = 前回受信時刻 + next_wake_sec - SCAN_PRE_MARGIN_SEC
 *      スキャン時間 = SCAN_WINDOW_SEC（60秒）
 *   3. スキャン終了後にシリアルモニタへ平均値・受信パケット数を表示
 *   4. ENABLE_SD_LOG=1 の場合、集計結果を SD カードの CSV に追記
 *   5. LTE-M 送信なし（BLE受信確認のみ）
 *
 * フィルタ条件:
 *   Manufacturer Data の Company ID = 0xFFFF、Pkt type = 0x01、Device ID = 0x01
 *
 * Manufacturer Data フォーマット（子機と共通、17バイト）:
 *   [0-1]  Company ID  0xFF 0xFF
 *   [2]    Pkt type    0x01
 *   [3]    Device ID   0x01 = "test01"
 *   [4]    FW Version  子機ファームのバージョン（コミットごとに+1。git logと突き合わせて特定する）
 *   [5-6]  CH1 ひずみ  int16_t LE（生値 / 100）
 *   [7-8]  CH2 ひずみ  int16_t LE
 *   [9-10] CH3 ひずみ  int16_t LE
 *   [11-12] CH4 ひずみ int16_t LE
 *   [13-14] バッテリー uint16_t LE（mV）
 *   [15-16] 次回計測まで uint16_t LE（秒）
 *
 * SD カード CSV フォーマット（log.csv）:
 *   session,elapsed_sec,pkt_count,fw_version,parent_fw_version,ch1_avg,ch2_avg,ch3_avg,ch4_avg,batt_mv,next_wake_sec
 *   ※ elapsed_sec は起動からの経過秒（RTC 未実装のため）
 *   ※ ch*_avg は ×100 の整数値（子機パケットそのまま）
 *   ※ fw_version は子機ファームのバージョン。スキャンウィンドウ内で最後に受信したパケットの値（子機は基本一定）
 *   ※ parent_fw_version は本スケッチ（親機）自身のバージョン。PARENT_FW_VERSION 定数（コミットごとに+1）
 *
 * LED:
 *   Blue 点灯    : スキャン中
 *   Green 2チカ  : データ受信・集計完了（SD 書き込み成功含む）
 *   Red 3チカ    : スキャンウィンドウ内でデータ未受信
 *   Red 1チカ    : SD 書き込みエラー
 *   Blue heartbeat: 次回スキャン待ち（5秒ごと）
 *
 * SD カード配線（v3.04 本番ピン / テスト環境と同じ）:
 *   DM3AT pin2 CS   → D3
 *   DM3AT pin3 MOSI → D1
 *   DM3AT pin5 SCK  → D10
 *   DM3AT pin7 MISO → D2
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <SPI.h>
#include <SdFat.h>
#include <bluefruit.h>
#include <string.h>

// ══════════════════════════════════════════════
// ▼ 設定（ここを変更する）
// ══════════════════════════════════════════════
static const uint8_t  TARGET_DEVICE_ID  = 0x01;   // 受信対象の子機 ID
static const uint8_t  MFR_COMPANY_LO    = 0xFF;   // Company ID（子機と一致させる）
static const uint8_t  MFR_COMPANY_HI    = 0xFF;
static const uint8_t  PKT_TYPE          = 0x01;
static const uint8_t  PARENT_FW_VERSION = 1;      // 親機（本スケッチ）自身のバージョン。コミットのたびに+1すること

// スキャン時間設定
static const uint32_t FIRST_SCAN_SEC    = 120;    // 初回スキャン時間（秒）
static const uint32_t SCAN_WINDOW_SEC   = 60;     // 2回目以降スキャン時間（秒）

// BLE スキャンパラメータ: interval = window = 100ms → 連続スキャン（取りこぼし 0%）
static const uint16_t BLE_SCAN_UNITS = 160;  // 160 × 0.625ms = 100ms

// ── SD カード設定 ──────────────────────────────
// 0: SD 記録なし（シリアルモニタのみ）
// 1: SD カードに CSV 追記
#define ENABLE_SD_LOG   1

#define PIN_SD_CS   D3
#define PIN_SD_MISO D2
#define PIN_SD_SCK  D10
#define PIN_SD_MOSI D1
#define SD_SPEED_MHZ 4

#define LOG_FILE "log.csv"
// ─────────────────────────────────────────────

// ══════════════════════════════════════════════
// SD カード
// ══════════════════════════════════════════════
#if ENABLE_SD_LOG
static SPIClass  SD_SPI(NRF_SPIM2, PIN_SD_MISO, PIN_SD_SCK, PIN_SD_MOSI);
static SdFat     sd;
static bool      s_sdReady = false;
#endif

// ══════════════════════════════════════════════
// 状態管理
// ══════════════════════════════════════════════
static bool     s_scanning        = false;
static bool     s_firstScan       = true;
static uint32_t s_scanEndMs       = 0;
static uint32_t s_nextScanStartMs = 0;
static uint32_t s_lastPktMs       = 0;
static uint32_t s_sessionNo       = 0;    // スキャン回数カウンタ

// 受信データ集計（同一スキャンウィンドウ内）
static int32_t  s_sum[4]      = {0};
static int32_t  s_battSum     = 0;
static uint16_t s_nextWakeSec = 600;
static int      s_pktCount    = 0;
static uint8_t  s_lastFwVersion = 0;  // スキャンウィンドウ内で最後に受信した子機ファームバージョン

// ══════════════════════════════════════════════
// LED ヘルパー（アクティブ LOW）
// ══════════════════════════════════════════════
static void ledInit() {
  pinMode(LED_RED,   OUTPUT); digitalWrite(LED_RED,   HIGH);
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, HIGH);
  pinMode(LED_BLUE,  OUTPUT); digitalWrite(LED_BLUE,  HIGH);
}
static void ledOff()           { digitalWrite(LED_RED,HIGH); digitalWrite(LED_GREEN,HIGH); digitalWrite(LED_BLUE,HIGH); }
static void pinOn(uint8_t pin) { ledOff(); digitalWrite(pin, LOW); }
static void ledBlink(uint8_t pin, int n) {
  for (int i = 0; i < n; i++) { digitalWrite(pin,LOW); delay(150); digitalWrite(pin,HIGH); if(i<n-1) delay(150); }
}

// ══════════════════════════════════════════════
// SD 初期化
// ══════════════════════════════════════════════
#if ENABLE_SD_LOG
static void sdInit() {
  SdSpiConfig cfg(PIN_SD_CS, DEDICATED_SPI, SD_SCK_MHZ(SD_SPEED_MHZ), &SD_SPI);
  if (!sd.begin(cfg)) {
    Serial.println(F("[SD] 初期化失敗 — SD 記録をスキップします"));
    sd.initErrorPrint(&Serial);
    s_sdReady = false;
    ledBlink(LED_RED, 1);
    return;
  }
  s_sdReady = true;
  Serial.println(F("[SD] 初期化 OK"));

  // ヘッダ行がなければ書き込む
  if (!sd.exists(LOG_FILE)) {
    FsFile f = sd.open(LOG_FILE, O_WRITE | O_CREAT);
    if (f) {
      f.println(F("session,elapsed_sec,pkt_count,fw_version,parent_fw_version,ch1_avg,ch2_avg,ch3_avg,ch4_avg,batt_mv,next_wake_sec"));
      f.close();
      Serial.println(F("[SD] " LOG_FILE " を新規作成しました"));
    }
  } else {
    Serial.println(F("[SD] " LOG_FILE " に追記します"));
  }
}
#endif

// ══════════════════════════════════════════════
// SD 書き込み（1行 CSV 追記）
// ══════════════════════════════════════════════
#if ENABLE_SD_LOG
static void sdLog(uint32_t sessionNo, int pktCount, uint8_t fwVersion,
                  int32_t ch1, int32_t ch2, int32_t ch3, int32_t ch4,
                  uint32_t battMv, uint16_t nextWakeSec) {
  if (!s_sdReady) return;

  FsFile f = sd.open(LOG_FILE, O_WRITE | O_APPEND);
  if (!f) {
    Serial.println(F("[SD] 書き込みエラー"));
    ledBlink(LED_RED, 1);
    return;
  }

  uint32_t elapsedSec = millis() / 1000UL;
  f.print(sessionNo);     f.print(',');
  f.print(elapsedSec);    f.print(',');
  f.print(pktCount);      f.print(',');
  f.print(fwVersion);     f.print(',');
  f.print(PARENT_FW_VERSION); f.print(',');
  f.print(ch1);           f.print(',');
  f.print(ch2);           f.print(',');
  f.print(ch3);           f.print(',');
  f.print(ch4);           f.print(',');
  f.print(battMv);        f.print(',');
  f.println(nextWakeSec);
  f.close();

  Serial.print(F("[SD] 書き込み完了 → session="));
  Serial.print(sessionNo);
  Serial.print(F(" elapsed="));
  Serial.print(elapsedSec);
  Serial.println(F("s"));
}
#endif

// ══════════════════════════════════════════════
// スキャン開始・停止
// ══════════════════════════════════════════════
static void startScan(uint32_t durationSec) {
  s_sum[0] = s_sum[1] = s_sum[2] = s_sum[3] = 0;
  s_battSum   = 0;
  s_pktCount  = 0;
  s_lastPktMs = 0;
  s_scanning  = true;
  s_scanEndMs = millis() + durationSec * 1000UL;
  s_sessionNo++;

  Bluefruit.Scanner.start(0);
  pinOn(LED_BLUE);

  Serial.println(F("\n──────────────────────────────────"));
  Serial.print(F("[SCAN START] session="));
  Serial.print(s_sessionNo);
  Serial.print(F("  duration="));
  Serial.print(durationSec);
  Serial.println(F("s"));
}

static void stopScan() {
  Bluefruit.Scanner.stop();
  s_scanning = false;
  ledOff();

  Serial.println(F("[SCAN STOP]"));
  Serial.print(F("  受信パケット数: "));
  Serial.println(s_pktCount);

  if (s_pktCount > 0) {
    int32_t avgCh[4];
    for (int i = 0; i < 4; i++) avgCh[i] = s_sum[i] / s_pktCount;
    uint32_t avgBatt = (uint32_t)s_battSum / (uint32_t)s_pktCount;

    Serial.println(F("  ─── 平均値 ───────────────────────"));
    for (int i = 0; i < 4; i++) {
      Serial.print(F("  CH")); Serial.print(i + 1);
      Serial.print(F(" ひずみ(×100): ")); Serial.println(avgCh[i]);
    }
    Serial.print(F("  バッテリー: ")); Serial.print(avgBatt); Serial.println(F(" mV"));
    Serial.print(F("  次回計測まで: ")); Serial.print(s_nextWakeSec); Serial.println(F(" sec"));
    Serial.print(F("  子機FWバージョン: ")); Serial.println(s_lastFwVersion);
    Serial.print(F("  親機FWバージョン: ")); Serial.println(PARENT_FW_VERSION);
    Serial.println(F("  ──────────────────────────────────"));

#if ENABLE_SD_LOG
    sdLog(s_sessionNo, s_pktCount, s_lastFwVersion,
          avgCh[0], avgCh[1], avgCh[2], avgCh[3],
          avgBatt, s_nextWakeSec);
#endif

    // 次回スキャン開始時刻
    s_nextScanStartMs = s_lastPktMs + (uint32_t)s_nextWakeSec * 1000UL;
    uint32_t waitSec = (s_nextScanStartMs - millis()) / 1000UL;
    Serial.print(F("  次回スキャン開始まで約 ")); Serial.print(waitSec); Serial.println(F(" 秒"));

    ledBlink(LED_GREEN, 2);
  } else {
    Serial.println(F("  [警告] データ受信なし → 60秒後に再スキャン"));
    s_nextScanStartMs = millis() + 60000UL;
    ledBlink(LED_RED, 3);
  }
}

// ══════════════════════════════════════════════
// BLE スキャンコールバック
// ══════════════════════════════════════════════
static void scanCallback(ble_gap_evt_adv_report_t* report) {
  uint8_t buf[32];
  uint8_t len = Bluefruit.Scanner.parseReportByType(
      report, BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, buf, sizeof(buf));

  if (len >= 17
      && buf[0] == MFR_COMPANY_LO
      && buf[1] == MFR_COMPANY_HI
      && buf[2] == PKT_TYPE
      && buf[3] == TARGET_DEVICE_ID) {

    uint8_t  fwVersion = buf[4];
    int16_t  ch[4];
    uint16_t battMv, nextWake;
    for (int i = 0; i < 4; i++) {
      ch[i] = (int16_t)(buf[5 + i * 2] | ((uint16_t)buf[6 + i * 2] << 8));
    }
    battMv   = (uint16_t)(buf[13] | ((uint16_t)buf[14] << 8));
    nextWake = (uint16_t)(buf[15] | ((uint16_t)buf[16] << 8));

    for (int i = 0; i < 4; i++) s_sum[i] += ch[i];
    s_battSum      += battMv;
    s_nextWakeSec   = nextWake;
    s_lastFwVersion = fwVersion;
    s_pktCount++;
    s_lastPktMs = millis();

    if (s_pktCount == 1) {
      Serial.println(F("\n  [子機検出] ─────────────────────"));
      char mac[18];
      snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
               report->peer_addr.addr[5], report->peer_addr.addr[4],
               report->peer_addr.addr[3], report->peer_addr.addr[2],
               report->peer_addr.addr[1], report->peer_addr.addr[0]);
      Serial.print(F("  MAC: ")); Serial.println(mac);
      Serial.print(F("  RSSI: ")); Serial.print(report->rssi); Serial.println(F(" dBm"));
      Serial.print(F("  FW Version: ")); Serial.println(fwVersion);
      Serial.print(F("  CH1-4: "));
      for (int i = 0; i < 4; i++) { Serial.print(ch[i]); Serial.print(' '); }
      Serial.println();
      Serial.print(F("  BATT: ")); Serial.print(battMv); Serial.println(F(" mV"));
      Serial.print(F("  next_wake: ")); Serial.print(nextWake); Serial.println(F(" sec"));
    } else if (s_pktCount % 5 == 0) {
      Serial.print('.');
    }
  }

  Bluefruit.Scanner.resume();
}

// ══════════════════════════════════════════════
// Arduino エントリ
// ══════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  ledInit();
  ledBlink(LED_BLUE, 2);

  Serial.println(F("\n[STEP12] BLE親機 スキャン + SD カード記録"));
  Serial.print(F("SD ログ: "));
#if ENABLE_SD_LOG
  Serial.println(F("有効 (ENABLE_SD_LOG=1)"));
  sdInit();
#else
  Serial.println(F("無効 (ENABLE_SD_LOG=0)"));
#endif

  Serial.println(F("フィルタ: Company=0xFFFF / Type=0x01 / DeviceID=0x01(test01)"));
  Serial.print(F("初回スキャン: ")); Serial.print(FIRST_SCAN_SEC); Serial.println(F("秒"));
  Serial.print(F("2回目以降:    ")); Serial.print(SCAN_WINDOW_SEC); Serial.println(F("秒"));
  Serial.println(F("──────────────────────────────────"));

  Bluefruit.begin(0, 1);
  Bluefruit.setName("Monita-Parent");
  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.useActiveScan(false);
  Bluefruit.Scanner.setInterval(BLE_SCAN_UNITS, BLE_SCAN_UNITS);

  s_nextScanStartMs = 0;
  startScan(FIRST_SCAN_SEC);
}

// ══════════════════════════════════════════════
// loop
// ══════════════════════════════════════════════
static uint32_t s_lastHeartbeat = 0;

void loop() {
  uint32_t now = millis();

  if (s_scanning) {
    if (now >= s_scanEndMs) {
      stopScan();
      s_firstScan = false;
    }
  } else {
    if (now - s_lastHeartbeat >= 5000UL) {
      s_lastHeartbeat = now;
      ledBlink(LED_BLUE, 1);
      if (now < s_nextScanStartMs) {
        uint32_t remain = (s_nextScanStartMs - now) / 1000UL;
        Serial.print(F("[待機] 次回スキャンまで "));
        Serial.print(remain);
        Serial.println(F(" 秒"));
      }
    }
    if (now >= s_nextScanStartMs) {
      startScan(s_firstScan ? FIRST_SCAN_SEC : SCAN_WINDOW_SEC);
    }
  }

  yield();
}
