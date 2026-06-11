/**
 * 検証 Step4: BLE GATT 接続（親機 = Peripheral / GATT サーバー）
 *
 * 役割:
 *   GATT サービス (0xAA00) とキャラクタリスティック (0xAA01) を持ち、
 *   子機からの接続を受け付けて文字列 "Hello from Parent" を返す。
 *
 * 合格基準:
 *   子機シリアルに "Hello from Parent" が表示される。
 *
 * サービス構成:
 *   Service UUID:          0xAA00 (カスタム)
 *   Characteristic UUID:   0xAA01  Property: READ
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>

#define DEVICE_NAME  "TimeParent"

// ── GATT サービス定義 ──────────────────────────────────
BLEService        testSvc(0xAA00);
BLECharacteristic testChar(0xAA01);

static const char HELLO_MSG[] = "Hello from Parent";

// ── 接続コールバック ───────────────────────────────────
void connectCallback(uint16_t conn_handle) {
  BLEConnection* conn = Bluefruit.Connection(conn_handle);
  char peerName[32] = {0};
  conn->getPeerName(peerName, sizeof(peerName));
  Serial.printf("[BLE] 接続: handle=%u  peer=\"%s\"\n", conn_handle, peerName);

  // キャラクタリスティックに値をセット（子機が READ で取得）
  testChar.write(HELLO_MSG, strlen(HELLO_MSG));
  Serial.printf("[GATT] 書き込み完了: \"%s\"\n", HELLO_MSG);
}

void disconnectCallback(uint16_t conn_handle, uint8_t reason) {
  Serial.printf("[BLE] 切断: handle=%u  reason=0x%02X\n", conn_handle, reason);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Bluefruit.begin();
  Bluefruit.setName(DEVICE_NAME);
  Bluefruit.setTxPower(4);
  Bluefruit.Periph.setConnectCallback(connectCallback);
  Bluefruit.Periph.setDisconnectCallback(disconnectCallback);

  // サービス開始
  testSvc.begin();

  // キャラクタリスティック設定
  testChar.setProperties(CHR_PROPS_READ);
  testChar.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  testChar.setMaxLen(32);
  testChar.begin();
  testChar.write(HELLO_MSG, strlen(HELLO_MSG));   // 初期値セット

  // アドバタイズ設定
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addService(testSvc);
  Bluefruit.Advertising.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(160, 160);
  Bluefruit.Advertising.start(0);

  Serial.println(F("[STEP4] GATT サーバー起動（親機）"));
  Serial.printf("  デバイス名: %s\n", DEVICE_NAME);
  Serial.printf("  Service:    0x%04X\n", (uint16_t)0xAA00);
  Serial.printf("  Char:       0x%04X  \"%s\"\n", (uint16_t)0xAA01, HELLO_MSG);
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last >= 5000) {
    last = millis();
    Serial.printf("[ADV] アドバタイズ中... 接続数=%u\n",
      Bluefruit.connected());
  }
}
