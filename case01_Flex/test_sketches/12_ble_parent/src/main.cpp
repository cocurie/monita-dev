/**
 * Monita Flex v3.02 — 検証 Step12: BLE親機（スキャン・シリアルモニタ確認用）
 *
 * 動作:
 *   1. 初回スキャン: 120秒（子機の起動タイミングが不明なため長めに設定）
 *   2. 2回目以降: 子機の「次回計測まで秒数」を使いスキャン開始タイミングを決定
 *      スキャン開始 = 前回受信時刻 + next_wake_sec - SCAN_PRE_MARGIN_SEC
 *      スキャン時間 = SCAN_WINDOW_SEC（60秒）
 *   3. スキャン終了後にシリアルモニタへ平均値・受信パケット数を表示
 *   4. LTE-M 送信なし（BLE受信確認のみ）
 *
 * フィルタ条件:
 *   Manufacturer Data の Company ID = 0xFFFF、Pkt type = 0x01、Device ID = 0x01
 *
 * Manufacturer Data フォーマット（子機と共通、16バイト）:
 *   [0-1]  Company ID  0xFF 0xFF
 *   [2]    Pkt type    0x01
 *   [3]    Device ID   0x01 = "test01"
 *   [4-5]  CH1 ひずみ  int16_t LE
 *   [6-7]  CH2 ひずみ  int16_t LE
 *   [8-9]  CH3 ひずみ  int16_t LE
 *   [10-11] CH4 ひずみ int16_t LE
 *   [12-13] バッテリー uint16_t LE（mV）
 *   [14-15] 次回計測まで uint16_t LE（秒）
 *
 * LED:
 *   Blue 点灯    : スキャン中
 *   Green 2チカ  : データ受信・集計完了
 *   Red 3チカ    : スキャンウィンドウ内でデータ未受信
 *   Blue heartbeat: 次回スキャン待ち（5秒ごと）
 *
 * スキャン取りこぼし率の目安（子機 1000ms 間隔・30パケットの場合）:
 *   BLE scan interval = window = 100ms（連続スキャン） → 取りこぼし 0%
 *   BLE scan interval=1000ms / window=100ms (10%デューティ) → 取りこぼし 4.2%
 *   → 本スケッチは連続スキャン（interval = window = 100ms）を採用
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>
#include <string.h>

// ══════════════════════════════════════════════
// ▼ 設定
// ══════════════════════════════════════════════
static const uint8_t  TARGET_DEVICE_ID  = 0x01;   // 受信対象の子機 ID
static const uint8_t  MFR_COMPANY_LO    = 0xFF;   // Company ID（子機と一致させる）
static const uint8_t  MFR_COMPANY_HI    = 0xFF;
static const uint8_t  PKT_TYPE          = 0x01;

// スキャン時間設定
static const uint32_t FIRST_SCAN_SEC    = 120;    // 初回スキャン時間（秒）
static const uint32_t SCAN_WINDOW_SEC   = 60;     // 2回目以降スキャン時間（秒）
// SCAN_PRE_MARGIN_SEC は子機側の NEXT_WAKE_SEC に含まれているため親機側の計算では不要

// BLE スキャンパラメータ: interval = window = 100ms → 連続スキャン（取りこぼし 0%）
// 100ms / 0.625ms = 160 units
static const uint16_t BLE_SCAN_UNITS = 160;

// ══════════════════════════════════════════════
// 状態管理
// ══════════════════════════════════════════════
static bool     s_scanning       = false;
static bool     s_firstScan      = true;
static uint32_t s_scanEndMs      = 0;     // スキャン終了時刻（millis）
static uint32_t s_nextScanStartMs = 0;    // 次回スキャン開始時刻（millis）
static uint32_t s_lastRxMs       = 0;     // 最後にデータを受信した millis

// 受信データ集計（同一スキャンウィンドウ内）
static int32_t  s_sum[4]         = {0};
static int32_t  s_battSum        = 0;
static uint16_t s_nextWakeSec    = 600;   // 子機から受け取った「次回計測まで秒数」
static int      s_pktCount       = 0;     // 受信パケット数
static uint32_t s_lastPktMs      = 0;     // 最後のパケット受信時刻（スケジュール計算用）

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
// スキャン開始・停止
// ══════════════════════════════════════════════
static void startScan(uint32_t durationSec) {
  s_sum[0] = s_sum[1] = s_sum[2] = s_sum[3] = 0;
  s_battSum   = 0;
  s_pktCount  = 0;
  s_lastPktMs = 0;
  s_scanning  = true;
  s_scanEndMs = millis() + durationSec * 1000UL;

  Bluefruit.Scanner.start(0);  // 0 = 無期限（手動 stop）
  pinOn(LED_BLUE);

  Serial.println(F("\n──────────────────────────────────"));
  Serial.print(F("[SCAN START] duration="));
  Serial.print(durationSec);
  Serial.print(F("s  until +"));
  Serial.print(durationSec);
  Serial.println(F("s from now"));
  Serial.print(F("  BLE scan: interval=window="));
  Serial.print((uint32_t)BLE_SCAN_UNITS * 625 / 1000);
  Serial.println(F("ms (連続スキャン)"));
}

static void stopScan() {
  Bluefruit.Scanner.stop();
  s_scanning = false;
  ledOff();

  Serial.println(F("[SCAN STOP]"));
  Serial.print(F("  受信パケット数: "));
  Serial.println(s_pktCount);

  if (s_pktCount > 0) {
    Serial.println(F("  ─── 平均値 ───────────────────────"));
    for (int i = 0; i < 4; i++) {
      int32_t avg = s_sum[i] / s_pktCount;
      Serial.print(F("  CH")); Serial.print(i + 1);
      Serial.print(F(" ひずみ(×100): ")); Serial.println(avg);
    }
    Serial.print(F("  バッテリー: "));
    Serial.print((uint32_t)s_battSum / (uint32_t)s_pktCount);
    Serial.println(F(" mV"));
    Serial.print(F("  次回計測まで: "));
    Serial.print(s_nextWakeSec);
    Serial.println(F(" sec"));
    Serial.println(F("  ──────────────────────────────────"));

    // 次回スキャン開始時刻を計算
    // next_wake_sec = 「最後のパケットからスキャン開始までの秒数」（マージン込み）
    s_nextScanStartMs = s_lastPktMs + (uint32_t)s_nextWakeSec * 1000UL;

    uint32_t waitSec = (s_nextScanStartMs - millis()) / 1000UL;
    Serial.print(F("  次回スキャン開始まで約 "));
    Serial.print(waitSec);
    Serial.println(F(" 秒"));

    ledBlink(LED_GREEN, 2);  // Green 2チカ = 受信成功
  } else {
    Serial.println(F("  [警告] データ受信なし → 60秒後に再スキャン"));
    s_nextScanStartMs = millis() + 60000UL;  // 1分後に再試行
    ledBlink(LED_RED, 3);  // Red 3チカ = 受信なし
  }
}

// ══════════════════════════════════════════════
// BLE スキャンコールバック
// ══════════════════════════════════════════════
static void scanCallback(ble_gap_evt_adv_report_t* report) {
  // Manufacturer Data を取得
  uint8_t buf[32];
  uint8_t len = Bluefruit.Scanner.parseReportByType(
      report, BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, buf, sizeof(buf));

  // 16バイト以上 + Company ID + Pkt type + Device ID のチェック
  if (len >= 16
      && buf[0] == MFR_COMPANY_LO
      && buf[1] == MFR_COMPANY_HI
      && buf[2] == PKT_TYPE
      && buf[3] == TARGET_DEVICE_ID) {

    // パース（LE バイトオーダー）
    int16_t  ch[4];
    uint16_t battMv, nextWake;
    for (int i = 0; i < 4; i++) {
      ch[i] = (int16_t)(buf[4 + i * 2] | ((uint16_t)buf[5 + i * 2] << 8));
    }
    battMv   = (uint16_t)(buf[12] | ((uint16_t)buf[13] << 8));
    nextWake = (uint16_t)(buf[14] | ((uint16_t)buf[15] << 8));

    // 集計
    for (int i = 0; i < 4; i++) s_sum[i] += ch[i];
    s_battSum    += battMv;
    s_nextWakeSec = nextWake;
    s_pktCount++;
    s_lastPktMs = millis();

    // 最初のパケット受信時にヘッダ表示、以降はドットのみ
    if (s_pktCount == 1) {
      Serial.println(F("\n  [子機検出] ─────────────────────"));
      char mac[18];
      snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
               report->peer_addr.addr[5], report->peer_addr.addr[4],
               report->peer_addr.addr[3], report->peer_addr.addr[2],
               report->peer_addr.addr[1], report->peer_addr.addr[0]);
      Serial.print(F("  MAC: ")); Serial.println(mac);
      Serial.print(F("  RSSI: ")); Serial.print(report->rssi); Serial.println(F(" dBm"));
      Serial.print(F("  CH1-4: "));
      for (int i = 0; i < 4; i++) { Serial.print(ch[i]); Serial.print(' '); }
      Serial.println();
      Serial.print(F("  BATT: ")); Serial.print(battMv); Serial.println(F(" mV"));
      Serial.print(F("  next_wake: ")); Serial.print(nextWake); Serial.println(F(" sec"));
    } else if (s_pktCount % 5 == 0) {
      Serial.print('.');  // 5パケットごとにドット
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

  Serial.println(F("\n[STEP12] BLE親機 スキャン確認"));
  Serial.println(F("フィルタ: Company=0xFFFF / Type=0x01 / DeviceID=0x01(test01)"));
  Serial.println(F("スキャン: interval=window=100ms（連続スキャン）"));
  Serial.print(F("初回スキャン: ")); Serial.print(FIRST_SCAN_SEC); Serial.println(F("秒"));
  Serial.print(F("2回目以降:    ")); Serial.print(SCAN_WINDOW_SEC); Serial.println(F("秒"));
  Serial.println(F("スキャン開始マージン: 子機 NEXT_WAKE_SEC に含まれる（親機側計算不要）"));
  Serial.println(F("──────────────────────────────────"));

  Bluefruit.begin(0, 1);  // 0 peripheral, 1 central（スキャン専用）
  Bluefruit.setName("Monita-Parent");

  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.useActiveScan(false);
  Bluefruit.Scanner.setInterval(BLE_SCAN_UNITS, BLE_SCAN_UNITS);  // interval = window（連続）

  // 初回スキャン即開始
  s_nextScanStartMs = 0;
  startScan(FIRST_SCAN_SEC);
}

// ══════════════════════════════════════════════
// loop: スキャン終了判定 + 次回スキャンスケジュール
// ══════════════════════════════════════════════
static uint32_t s_lastHeartbeat = 0;

void loop() {
  uint32_t now = millis();

  if (s_scanning) {
    // スキャン中: 終了時刻に達したら停止
    if (now >= s_scanEndMs) {
      stopScan();
      s_firstScan = false;
    }
  } else {
    // 待機中: Blue heartbeat（5秒ごと）
    if (now - s_lastHeartbeat >= 5000UL) {
      s_lastHeartbeat = now;
      ledBlink(LED_BLUE, 1);

      // 次回スキャン開始まで何秒か表示
      if (now < s_nextScanStartMs) {
        uint32_t remain = (s_nextScanStartMs - now) / 1000UL;
        Serial.print(F("[待機] 次回スキャンまで "));
        Serial.print(remain);
        Serial.println(F(" 秒"));
      }
    }

    // 次回スキャン開始時刻に達したら開始
    if (now >= s_nextScanStartMs) {
      startScan(s_firstScan ? FIRST_SCAN_SEC : SCAN_WINDOW_SEC);
    }
  }

  yield();
}
