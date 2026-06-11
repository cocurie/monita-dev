/**
 * 検証 Step3: BLE スキャン（子機）
 *
 * 役割:
 *   周辺の BLE デバイスをスキャンし、デバイス名・MAC・RSSI を表示する。
 *   "TimeParent" が見えれば合格。
 *
 * 合格基準:
 *   シリアルに "TimeParent  RSSI=-XX dBm" が表示される。
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>

// ── スキャンコールバック ────────────────────────────────
void scanCallback(ble_gap_evt_adv_report_t* report) {
  // MAC アドレス（リトルエンディアン）
  Serial.printf("[SCAN] %02X:%02X:%02X:%02X:%02X:%02X",
    report->peer_addr.addr[5], report->peer_addr.addr[4],
    report->peer_addr.addr[3], report->peer_addr.addr[2],
    report->peer_addr.addr[1], report->peer_addr.addr[0]);

  Serial.printf("  RSSI=%3d dBm", report->rssi);

  // デバイス名を取得（Complete / Short どちらも試す）
  uint8_t nameBuf[32] = {0};
  if (Bluefruit.Scanner.parseReportByType(report,
        BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, nameBuf, sizeof(nameBuf)) ||
      Bluefruit.Scanner.parseReportByType(report,
        BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME,    nameBuf, sizeof(nameBuf))) {
    Serial.printf("  Name=\"%s\"", (char*)nameBuf);
  }

  // "TimeParent" なら強調表示
  if (strcmp((char*)nameBuf, "TimeParent") == 0) {
    Serial.print(F("  ★ TARGET FOUND"));
  }

  Serial.println();

  Bluefruit.Scanner.resume();  // 次のパケットを受け取るために必要
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Bluefruit.begin(0, 1);   // peripheral=0, central=1
  Bluefruit.setName("TimeChild");

  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.setInterval(160, 80);  // interval=100ms, window=50ms
  Bluefruit.Scanner.useActiveScan(true);   // 名前取得のためアクティブスキャン
  Bluefruit.Scanner.start(0);              // 0 = 無限スキャン

  Serial.println(F("[STEP3] BLE スキャン開始（子機）"));
  Serial.println(F("  'TimeParent' が見えれば合格"));
}

void loop() {
  delay(1000);
}
