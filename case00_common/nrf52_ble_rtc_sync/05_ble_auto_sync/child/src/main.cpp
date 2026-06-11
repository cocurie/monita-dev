/**
 * 検証 Step7: スリープ＋起床時自動同期（子機）
 *
 * 動作サイクル（CYCLE_SEC 秒ごとに繰り返す）:
 *   1. 起床
 *   2. BLE スキャン（SCAN_TIMEOUT_SEC 秒）
 *      → 親機が見つかれば: 接続 → 時刻同期 → 切断
 *      → 見つからなければ: スキャンタイムアウト後スキップ
 *   3. 模擬計測: DS3231 から時刻を読んでタイムスタンプ付きデータを出力
 *   4. 待機（CYCLE_SEC 秒の残り時間）
 *   5. 1 に戻る
 *
 * 注意:
 *   本スケッチでは delay() で待機を表現（ブレッドボード検証用）。
 *   Flex v3.02 に統合する際は deepSleep(RTC2) に置き換える。
 *
 * 親機:
 *   04_ble_time_sync/parent をそのまま使用する。
 *
 * 合格基準:
 *   - 起床のたびに時刻同期ログが出る（親機が近くにある場合）
 *   - タイムスタンプ付き模擬計測データが CYCLE_SEC ごとに出力される
 *   - 親機が遠い/オフの場合はスキャンタイムアウト後も計測が継続される
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <RTClib.h>
#include <bluefruit.h>

// ══════════════════════════════════════════════
// ▼ 設定
// ══════════════════════════════════════════════
static const uint32_t CYCLE_SEC       = 30;   // 計測サイクル（秒）★本番は3600等に変更
static const uint16_t SCAN_TIMEOUT_SEC = 10;  // BLEスキャンのタイムアウト（秒）
static const char     TARGET_NAME[]   = "TimeParent";

// ══════════════════════════════════════════════
RTC_DS3231 rtc;

BLEClientService        timeSvc(0xBB00);
BLEClientCharacteristic timeChar(0xBB01);

static volatile bool s_syncDone  = false;  // このサイクルで同期完了したか
static volatile bool s_scanDone  = false;  // スキャン/接続フローが完了したか
static uint32_t      s_cycleCount = 0;

// ── BLE コールバック ───────────────────────────────────
void scanCallback(ble_gap_evt_adv_report_t* report) {
  uint8_t nameBuf[32] = {0};
  Bluefruit.Scanner.parseReportByType(report,
    BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, nameBuf, sizeof(nameBuf));
  if (strcmp((char*)nameBuf, TARGET_NAME) == 0) {
    Serial.printf("[SCAN] '%s' 発見 → 接続\n", TARGET_NAME);
    Bluefruit.Central.connect(report);
  } else {
    Bluefruit.Scanner.resume();
  }
}

void scanStopCallback() {
  // タイムアウトでスキャンが止まった
  if (!s_syncDone) {
    Serial.println(F("[SCAN] タイムアウト（親機未発見）"));
    s_scanDone = true;
  }
}

void connectCallback(uint16_t conn_handle) {
  Serial.println(F("[BLE] 接続成立"));

  if (!timeSvc.discover(conn_handle) || !timeChar.discover()) {
    Serial.println(F("[GATT] サービス/キャラクタリスティック未発見 → 切断"));
    Bluefruit.disconnect(conn_handle);
    return;
  }

  uint32_t unix_t = 0;
  uint16_t len = timeChar.read(&unix_t, sizeof(unix_t));

  if (len == 4) {
    rtc.adjust(DateTime(unix_t));
    s_syncDone = true;

    DateTime now = rtc.now();
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:%02d",
      now.year(), now.month(), now.day(),
      now.hour(), now.minute(), now.second());
    Serial.printf("[SYNC] 同期完了: %s\n", buf);
  }

  Bluefruit.disconnect(conn_handle);
}

void disconnectCallback(uint16_t conn_handle, uint8_t reason) {
  Serial.printf("[BLE] 切断: reason=0x%02X\n", reason);
  s_scanDone = true;
}

// ── BLE 時刻同期を試みる ───────────────────────────────
static void attemptSync() {
  s_syncDone = false;
  s_scanDone = false;

  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.setStopCallback(scanStopCallback);
  Bluefruit.Scanner.setInterval(160, 80);
  Bluefruit.Scanner.useActiveScan(true);
  Bluefruit.Scanner.start(SCAN_TIMEOUT_SEC);  // タイムアウト付きスキャン

  Serial.printf("[SYNC] 親機スキャン開始（%u秒）\n", SCAN_TIMEOUT_SEC);

  // スキャン完了またはタイムアウトまで待つ
  uint32_t t = millis();
  while (!s_scanDone && (millis() - t < (SCAN_TIMEOUT_SEC + 3) * 1000UL)) {
    yield();
  }
}

// ── 模擬計測 ──────────────────────────────────────────
static void doMeasurement() {
  DateTime now = rtc.now();
  char timeBuf[32];
  snprintf(timeBuf, sizeof(timeBuf), "%04d/%02d/%02d %02d:%02d:%02d",
    now.year(), now.month(), now.day(),
    now.hour(), now.minute(), now.second());

  // 模擬センサ値（本番では HX711 等に差し替え）
  int32_t mockStrain = random(-10, 10);  // ゼロ付近のノイズ

  Serial.printf("[MEAS] #%lu  time=%s  strain=%ld  synced=%s\n",
    s_cycleCount, timeBuf, mockStrain, s_syncDone ? "YES" : "NO");
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Wire.begin();
  if (!rtc.begin()) {
    Serial.println(F("[ERROR] DS3231 が見つかりません。配線を確認してください。"));
    while (true) { delay(100); yield(); }   // USB を生かしたままハング
  }

  DateTime now = rtc.now();
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:%02d",
    now.year(), now.month(), now.day(),
    now.hour(), now.minute(), now.second());
  Serial.printf("[RTC] 起動時刻: %s\n", buf);

  Bluefruit.begin(0, 1);
  Bluefruit.setName("TimeChild");

  timeSvc.begin();
  timeChar.begin();

  Bluefruit.Central.setConnectCallback(connectCallback);
  Bluefruit.Central.setDisconnectCallback(disconnectCallback);

  Serial.println(F("[STEP7] 自動同期サイクル 起動"));
  Serial.printf("  サイクル: %lu秒 / スキャンタイムアウト: %u秒\n",
    CYCLE_SEC, SCAN_TIMEOUT_SEC);
}

void loop() {
  s_cycleCount++;
  Serial.printf("\n=== サイクル #%lu ===\n", s_cycleCount);

  uint32_t cycleStart = millis();

  // 1. BLE 時刻同期を試みる
  attemptSync();

  // 2. 計測
  doMeasurement();

  // 3. 次のサイクルまで待機
  uint32_t elapsed = (millis() - cycleStart) / 1000UL;
  uint32_t remaining = (elapsed < CYCLE_SEC) ? (CYCLE_SEC - elapsed) : 0;
  if (remaining > 0) {
    Serial.printf("[WAIT] 次のサイクルまで %lu秒待機...\n", remaining);
    delay(remaining * 1000UL);
  }
}
