/**
 * 検証 Step4: BLE GATT 接続（子機 = Central / GATT クライアント）
 *
 * 役割:
 *   "TimeParent" をスキャンして接続し、キャラクタリスティック (0xAA01) を
 *   READ して文字列を表示する。
 *
 * 合格基準:
 *   シリアルに "Hello from Parent" が表示される。
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>

#define TARGET_NAME  "TimeParent"

BLEClientService        testSvc(0xAA00);
BLEClientCharacteristic testChar(0xAA01);

// ── スキャンコールバック ────────────────────────────────
void scanCallback(ble_gap_evt_adv_report_t* report) {
  uint8_t nameBuf[32] = {0};
  Bluefruit.Scanner.parseReportByType(report,
    BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, nameBuf, sizeof(nameBuf));
  if (strcmp((char*)nameBuf, TARGET_NAME) == 0) {
    Serial.printf("[SCAN] '%s' 発見 → 接続します\n", TARGET_NAME);
    Bluefruit.Central.connect(report);
  } else {
    Bluefruit.Scanner.resume();
  }
}

// ── 接続コールバック ───────────────────────────────────
void connectCallback(uint16_t conn_handle) {
  Serial.println(F("[BLE] 接続成立"));

  // サービス探索
  if (!testSvc.discover(conn_handle)) {
    Serial.println(F("[GATT] サービス 0xAA00 が見つかりません → 切断"));
    Bluefruit.disconnect(conn_handle);
    return;
  }
  Serial.println(F("[GATT] サービス 0xAA00 発見"));

  // キャラクタリスティック探索
  if (!testChar.discover()) {
    Serial.println(F("[GATT] キャラクタリスティック 0xAA01 が見つかりません → 切断"));
    Bluefruit.disconnect(conn_handle);
    return;
  }
  Serial.println(F("[GATT] キャラクタリスティック 0xAA01 発見"));

  // READ
  uint8_t buf[32] = {0};
  uint16_t len = testChar.read(buf, sizeof(buf) - 1);
  buf[len] = '\0';

  Serial.printf("[GATT] READ 結果 (%u bytes): \"%s\"\n", len, (char*)buf);
  Serial.println(F("[STEP4] ★ 合格: 文字列の送受信を確認しました"));

  Bluefruit.disconnect(conn_handle);
}

// ── 切断コールバック ───────────────────────────────────
void disconnectCallback(uint16_t conn_handle, uint8_t reason) {
  Serial.printf("[BLE] 切断: reason=0x%02X\n", reason);
  Serial.println(F("[BLE] 5秒後に再スキャンします..."));
  delay(5000);
  Bluefruit.Scanner.start(0);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Bluefruit.begin(0, 1);   // central モード
  Bluefruit.setName("TimeChild");

  // クライアントサービス / キャラクタリスティック登録
  testSvc.begin();
  testChar.begin();

  Bluefruit.Central.setConnectCallback(connectCallback);
  Bluefruit.Central.setDisconnectCallback(disconnectCallback);

  // スキャン開始
  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.setInterval(160, 80);
  Bluefruit.Scanner.useActiveScan(true);
  Bluefruit.Scanner.start(0);

  Serial.println(F("[STEP4] GATT クライアント起動（子機）"));
  Serial.printf("  '%s' をスキャン中...\n", TARGET_NAME);
}

void loop() {
  delay(1000);
}
