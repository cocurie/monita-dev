/**
 * 検証 Step5+6: BLE 時刻転送（親機）
 *
 * 役割:
 *   DS3231 から Unix タイムスタンプ（uint32_t, 4バイト）を読み取り、
 *   BLE GATT キャラクタリスティック (0xBB01) に書き込む。
 *   子機が接続して READ または NOTIFY で受け取る。
 *
 * Step5 合格基準:
 *   子機側 DS3231 の時刻が親機と ±1秒以内で一致する。
 *
 * Step6 手順（停電テスト）:
 *   1. Step5 で時刻同期を確認
 *   2. 子機の USB を抜く（停電）
 *   3. 数分放置
 *   4. 子機を再接続 → シリアルを開く
 *   5. 起動直後の時刻が正しければ合格（DS3231 が時刻を保持）
 *
 * シリアルコマンド:
 *   S → DS3231 をコンパイル時刻にセット
 *
 * 配線:
 *   DS3231 SDA → XIAO D4 / SCL → D5 / VCC → 3.3V / GND → GND
 *
 * サービス構成:
 *   Service UUID:        0xBB00
 *   Characteristic UUID: 0xBB01  Property: READ | NOTIFY
 *   Data:                uint32_t (Little-Endian) = Unix timestamp
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <RTClib.h>
#include <bluefruit.h>

#define DEVICE_NAME "TimeParent"

static const int32_t TZ_OFFSET_SEC = 9 * 3600L;  // JST = UTC+9

RTC_DS3231 rtc;

BLEService        timeSvc(0xBB00);
BLECharacteristic timeChar(0xBB01);

// ── 時刻をキャラクタリスティックに書き込む ─────────────
static void updateTimeChar() {
  DateTime now = rtc.now();
  uint32_t unix_t = now.unixtime();
  timeChar.write(&unix_t, sizeof(unix_t));
}

// ── 接続コールバック ───────────────────────────────────
void connectCallback(uint16_t conn_handle) {
  BLEConnection* conn = Bluefruit.Connection(conn_handle);
  char peerName[32] = {0};
  conn->getPeerName(peerName, sizeof(peerName));
  Serial.printf("[BLE] 接続: peer=\"%s\"\n", peerName);

  // 現在時刻を書き込んで NOTIFY
  DateTime utc = rtc.now();
  uint32_t unix_t = utc.unixtime();   // BLE 送信は UTC のまま
  timeChar.write(&unix_t, sizeof(unix_t));
  timeChar.notify(&unix_t, sizeof(unix_t));

  DateTime jst = DateTime(unix_t + TZ_OFFSET_SEC);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:%02d",
    jst.year(), jst.month(), jst.day(),
    jst.hour(), jst.minute(), jst.second());
  Serial.printf("[TIME] 送信: %s JST  (UNIX=%lu)\n", buf, unix_t);
}

void disconnectCallback(uint16_t conn_handle, uint8_t reason) {
  Serial.printf("[BLE] 切断: reason=0x%02X\n", reason);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  // DS3231 初期化
  Wire.begin();
  if (!rtc.begin()) {
    Serial.println(F("[ERROR] DS3231 が見つかりません。配線を確認してください。"));
    while (true) { delay(100); yield(); }   // USB を生かしたままハング
  }
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    Serial.println(F("[RTC] 電源喪失検出 → コンパイル時刻でセット"));
  }
  Serial.println(F("[RTC] OK"));

  // BLE 初期化
  Bluefruit.begin();
  Bluefruit.setName(DEVICE_NAME);
  Bluefruit.setTxPower(4);
  Bluefruit.Periph.setConnectCallback(connectCallback);
  Bluefruit.Periph.setDisconnectCallback(disconnectCallback);

  // GATT サービス設定
  timeSvc.begin();
  timeChar.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
  timeChar.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  timeChar.setFixedLen(4);
  timeChar.begin();
  updateTimeChar();   // 初期値セット

  // アドバタイズ
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addService(timeSvc);
  Bluefruit.Advertising.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(160, 160);
  Bluefruit.Advertising.start(0);

  Serial.println(F("[STEP5] BLE 時刻配信 起動（親機）"));
  Serial.println(F("  's' でコンパイル時刻にセット"));
}

void loop() {
  if (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'S' || c == 's') {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      Serial.println(F("[RTC] コンパイル時刻にセットしました"));
    }
  }

  // 5秒ごとに現在時刻を表示
  static uint32_t last = 0;
  if (millis() - last >= 5000) {
    last = millis();
    DateTime utc = rtc.now();
    DateTime jst = DateTime(utc.unixtime() + TZ_OFFSET_SEC);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:%02d",
      jst.year(), jst.month(), jst.day(),
      jst.hour(), jst.minute(), jst.second());
    Serial.printf("[RTC] %s JST  接続数=%u\n", buf, Bluefruit.connected());
    updateTimeChar();   // 接続中なら最新値に更新しておく
  }
}
