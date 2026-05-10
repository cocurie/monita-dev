/**
 * BLE 公園人流測定 — 周辺デバイスのアドバタイジングをシリアルへログ
 * 基板: Seeed XIAO nRF52840 Sense / Adafruit Bluefruit52 Central スキャン
 *
 * 要件メモ: ble_people_count_requirements.md §3.2 Active Scan
 * SoftDevice v6: 各 ADV レポート後に Scanner.resume() が必須
 */

#include <bluefruit.h>

/** 1 にするとローカル名等を AD から分解して追記（ログ量増） */
#ifndef DEBUG_VERBOSE
#define DEBUG_VERBOSE 0
#endif

/** 連続スキャンの開始間隔（ミリ秒）。「10秒に一回」スキャン周期の基準 */
#ifndef SCAN_CYCLE_MS
#define SCAN_CYCLE_MS 10000u
#endif
/**
 * 1回のスキャン継続時間。Adafruit BLEScanner::start(timeout) の timeout は
 * Nordic SoftDevice の「スキャン持続時間」= 10ms 単位（0 のときは無制限）
 */
#ifndef SCAN_DURATION_10MS
#define SCAN_DURATION_10MS 200u /* 200 × 10ms = 2s。長くしたいときは例: 1000 = 10s */
#endif

static void scan_callback(ble_gap_evt_adv_report_t *report);
static uint32_t last_scan_cycle_ms;

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }

  Serial.println();
  Serial.println(F("=== BLE park scan — advertising log ==="));
  Serial.println(F("fmt: ms MAC_RSSI [ADV|SR] payload(hex)"));
#if DEBUG_VERBOSE
  Serial.println(F("(DEBUG_VERBOSE: name/MSD lines follow each packet)"));
#endif
  Serial.println();

  Bluefruit.begin(0, 1);
  Bluefruit.setTxPower(4);
  Bluefruit.setName("XiaoParkScan");

  Bluefruit.Scanner.setRxCallback(scan_callback);
  Bluefruit.Scanner.restartOnDisconnect(true);
  Bluefruit.Scanner.setInterval(160, 80);
  Bluefruit.Scanner.useActiveScan(true);
  /* start(0) は使わず loop で周期起動（省電力・ログ間引き） */
  last_scan_cycle_ms = 0;

  Serial.print(F("Scan cycle: every "));
  Serial.print(SCAN_CYCLE_MS / 1000);
  Serial.print(F(" s, burst "));
  Serial.print((unsigned long)SCAN_DURATION_10MS * 10UL);
  Serial.println(F(" ms (active), then idle"));
  Serial.println();
}

void scan_callback(ble_gap_evt_adv_report_t *report) {
  Serial.print((unsigned long)millis());
  Serial.print(' ');
  Serial.printBufferReverse(report->peer_addr.addr, 6, ':');
  Serial.print(' ');
  Serial.print(report->rssi);
  Serial.print(F("dBm "));
  Serial.print(report->type.scan_response ? F("[SR] ") : F("[ADV] "));
  Serial.printBuffer(report->data.p_data, report->data.len, '-');
  Serial.println();

#if DEBUG_VERBOSE
  uint8_t buf[32];
  memset(buf, 0, sizeof(buf));
  if (Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME, buf,
                                          sizeof(buf))) {
    Serial.print(F("      name(short): "));
    Serial.println((const char *)buf);
  }
  memset(buf, 0, sizeof(buf));
  if (Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, buf,
                                          sizeof(buf))) {
    Serial.print(F("      name(complete): "));
    Serial.println((const char *)buf);
  }
  memset(buf, 0, sizeof(buf));
  uint8_t len = Bluefruit.Scanner.parseReportByType(
      report, BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, buf, sizeof(buf));
  if (len) {
    Serial.print(F("      MSD ("));
    Serial.print(len);
    Serial.print(F("): "));
    Serial.printBuffer(buf, len, '-');
    Serial.println();
  }
#endif

  Bluefruit.Scanner.resume();
}

void loop() {
  if (Bluefruit.Scanner.isRunning()) {
    return;
  }

  uint32_t const now = millis();
  if (last_scan_cycle_ms != 0 && (now - last_scan_cycle_ms) < SCAN_CYCLE_MS) {
    return;
  }

  last_scan_cycle_ms = now;
  Serial.print(F("[scan start] "));
  Serial.println((unsigned long)now);

  if (!Bluefruit.Scanner.start(SCAN_DURATION_10MS)) {
    Serial.println(F("[err] Scanner.start failed"));
    delay(50);
  }
}
