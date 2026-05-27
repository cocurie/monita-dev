/**
 * BLE 親機 — Manufacturer Data 受信・デコード
 * 基板: Seeed XIAO nRF52840
 *
 * 対応子機: case00_common/nrf52_ble/child_adv/
 *
 * 子機のペイロード形式:
 *   [CompanyID_L][CompanyID_H][CH0_H][CH0_L][CH1_H][CH1_L][CH2_H][CH2_L][CH3_H][CH3_L]
 *   CompanyID = 0xFFFF（テスト用）
 *   CH0〜CH3 = uint16_t big-endian（analogRead 値、0〜4095）
 *
 * シリアルモニタ出力例:
 *   [12s] nRfBLE1  RSSI=-58dBm
 *     CH0: 0x07FF (2047)
 *     CH1: 0x0000 (0)
 *     CH2: 0x0FFF (4095)
 *     CH3: 0x0800 (2048)
 */

#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>
#include <string.h>

// ── 設定 ─────────────────────────────────────────
// フィルタ対象の Company ID（子機と合わせること）
static uint16_t const TARGET_COMPANY_ID = 0xFFFF;

// フィルタ対象のデバイス名
// ※ ADVパケットと Scan Response は別コールバックのため名前フィルタは使わない
// ※ CompanyID だけで判定する（空文字 = フィルタなし）
static char const *const TARGET_NAME = "";

// 子機の送信チャンネル数
static uint8_t const NUM_CHANNELS = 4;

// MSD の最小バイト数: CompanyID(2) + CH×2
static uint8_t const MSD_MIN_LEN = 2 + NUM_CHANNELS * 2;
// ─────────────────────────────────────────────────

// ── スキャンコールバック ──────────────────────────

static void scanCallback(ble_gap_evt_adv_report_t *report) {

  // ① Manufacturer Data を取り出す
  uint8_t msd_buf[32] = {0};
  uint8_t msd_len = Bluefruit.Scanner.parseReportByType(
      report,
      BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA,
      msd_buf,
      sizeof(msd_buf));

  // MSD がない、または短すぎる → スキップ
  if (msd_len < MSD_MIN_LEN) {
    Bluefruit.Scanner.resume();
    return;
  }

  // ② Company ID チェック（little-endian で格納されている）
  uint16_t company_id = (uint16_t)msd_buf[0] | ((uint16_t)msd_buf[1] << 8);
  if (company_id != TARGET_COMPANY_ID) {
    Bluefruit.Scanner.resume();
    return;
  }

  // ③ デバイス名チェック（TARGET_NAME が空でなければフィルタ）
  if (strlen(TARGET_NAME) > 0) {
    uint8_t name_buf[32] = {0};
    bool name_matched = false;

    // Complete Local Name を試みる
    if (Bluefruit.Scanner.parseReportByType(
            report,
            BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME,
            name_buf, sizeof(name_buf))) {
      if (strcmp((const char *)name_buf, TARGET_NAME) == 0) name_matched = true;
    }

    // Short Local Name も試みる
    if (!name_matched) {
      memset(name_buf, 0, sizeof(name_buf));
      if (Bluefruit.Scanner.parseReportByType(
              report,
              BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME,
              name_buf, sizeof(name_buf))) {
        if (strcmp((const char *)name_buf, TARGET_NAME) == 0) name_matched = true;
      }
    }

    if (!name_matched) {
      Bluefruit.Scanner.resume();
      return;
    }
  }

  // ④ ペイロードをデコードして出力
  Serial.println(F("----------------------------------------"));
  Serial.print('[');
  Serial.print(millis() / 1000UL);
  Serial.print(F("s] "));
  Serial.print(TARGET_NAME);
  Serial.print(F("  RSSI="));
  Serial.print(report->rssi);
  Serial.println(F("dBm"));

  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    // big-endian で格納されている
    uint16_t val = ((uint16_t)msd_buf[2 + i * 2] << 8)
                 |  (uint16_t)msd_buf[2 + i * 2 + 1];

    Serial.print(F("  CH"));
    Serial.print(i);
    Serial.print(F(": 0x"));
    if (val < 0x1000) Serial.print('0');
    if (val < 0x0100) Serial.print('0');
    if (val < 0x0010) Serial.print('0');
    Serial.print(val, HEX);
    Serial.print(F("  ("));
    Serial.print(val);
    Serial.println(')');
  }

  Bluefruit.Scanner.resume();
}

// ── setup ────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Serial.println(F("\n--- BLE Parent: MSD Receiver ---"));
  Serial.print(F("Target CompanyID : 0x"));
  Serial.println(TARGET_COMPANY_ID, HEX);
  Serial.print(F("Target Name      : "));
  Serial.println(TARGET_NAME);
  Serial.print(F("Channels         : "));
  Serial.println(NUM_CHANNELS);
  Serial.println(F("Scanning..."));
  Serial.println();

  Bluefruit.begin(0, 1);  // Central のみ（Peripheral 不要）
  Bluefruit.setName("ParentMSDRecv");

  // Active Scan: Scan Response（デバイス名）も取得する
  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.useActiveScan(true);
  Bluefruit.Scanner.setInterval(160, 80);  // 100ms interval / 50ms window
  Bluefruit.Scanner.start(0);              // 0 = 無制限スキャン
}

// ── loop ─────────────────────────────────────────

void loop() {
  // スキャンはコールバック駆動なので loop は空でよい
}
