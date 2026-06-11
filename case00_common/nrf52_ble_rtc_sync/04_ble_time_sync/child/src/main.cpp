/**
 * 検証 Step5+6: BLE 時刻転送（子機）
 *
 * 役割:
 *   "TimeParent" に接続し、時刻キャラクタリスティック (0xBB01) を READ して
 *   自分の DS3231 に書き込む（時刻同期）。
 *
 * Step5 合格基準:
 *   同期後、親機・子機のシリアルモニタの時刻が ±1秒以内で一致する。
 *
 * Step6 合格基準:
 *   USB を抜いて数分後に再接続しても、起動直後から正しい時刻が表示される。
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <RTClib.h>
#include <bluefruit.h>

#define TARGET_NAME "TimeParent"

static const int32_t TZ_OFFSET_SEC = 9 * 3600L;  // JST = UTC+9

RTC_DS3231 rtc;

BLEClientService        timeSvc(0xBB00);
BLEClientCharacteristic timeChar(0xBB01);

static bool s_synced = false;

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

  if (!timeSvc.discover(conn_handle)) {
    Serial.println(F("[GATT] 時刻サービス (0xBB00) が見つかりません → 切断"));
    Bluefruit.disconnect(conn_handle);
    return;
  }

  if (!timeChar.discover()) {
    Serial.println(F("[GATT] 時刻キャラクタリスティック (0xBB01) が見つかりません → 切断"));
    Bluefruit.disconnect(conn_handle);
    return;
  }

  // Unix タイムスタンプ（4バイト, Little-Endian）を READ
  uint32_t unix_t = 0;
  uint16_t len = timeChar.read(&unix_t, sizeof(unix_t));

  if (len == 4) {
    // DS3231 に書き込む（UTC で保存）
    rtc.adjust(DateTime(unix_t));
    s_synced = true;
    Bluefruit.Scanner.stop();  // 同期完了 → スキャン停止

    DateTime jst = DateTime(unix_t + TZ_OFFSET_SEC);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:%02d",
      jst.year(), jst.month(), jst.day(),
      jst.hour(), jst.minute(), jst.second());
    Serial.printf("[SYNC] 同期完了: %s JST  (UNIX=%lu)\n", buf, unix_t);
    Serial.println(F("[STEP5] ★ 親機のシリアルと時刻を比較してください"));
  } else {
    Serial.printf("[SYNC] 読み取り失敗: %u bytes\n", len);
  }

  Bluefruit.disconnect(conn_handle);
}

// ── 切断コールバック ───────────────────────────────────
void disconnectCallback(uint16_t conn_handle, uint8_t reason) {
  Serial.printf("[BLE] 切断: reason=0x%02X\n", reason);
  if (s_synced) {
    Bluefruit.Scanner.stop();  // 同期済み → 再スキャンしない
  } else {
    Serial.println(F("[BLE] 未同期 → 5秒後に再スキャン"));
    delay(5000);
    Bluefruit.Scanner.start(0);
  }
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

  {
    DateTime utc = rtc.now();
    DateTime jst = DateTime(utc.unixtime() + TZ_OFFSET_SEC);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:%02d",
      jst.year(), jst.month(), jst.day(),
      jst.hour(), jst.minute(), jst.second());
    Serial.printf("[RTC] 起動時刻: %s JST%s\n",
      buf, rtc.lostPower() ? "  ※電源喪失あり（同期前）" : "");
  }

  // BLE 初期化（Central モード）
  Bluefruit.begin(0, 1);
  Bluefruit.setName("TimeChild");

  timeSvc.begin();
  timeChar.begin();

  Bluefruit.Central.setConnectCallback(connectCallback);
  Bluefruit.Central.setDisconnectCallback(disconnectCallback);

  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.setInterval(160, 80);
  Bluefruit.Scanner.useActiveScan(true);
  Bluefruit.Scanner.start(0);

  Serial.println(F("[STEP5] BLE 時刻同期 起動（子機）"));
  Serial.printf("  '%s' をスキャン中...\n", TARGET_NAME);
}

void loop() {
  // 同期後は3秒ごとに現在時刻を表示
  static uint32_t last = 0;
  if (s_synced && millis() - last >= 3000) {
    last = millis();
    DateTime utc = rtc.now();
    DateTime jst = DateTime(utc.unixtime() + TZ_OFFSET_SEC);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:%02d",
      jst.year(), jst.month(), jst.day(),
      jst.hour(), jst.minute(), jst.second());
    Serial.printf("[RTC] %s JST\n", buf);
  }
}
