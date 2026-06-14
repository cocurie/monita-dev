/**
 * Monita Flex v3.03 — BLE 親機スケッチ
 *
 * 動作:
 *   - DS3231 の時刻を基準に毎時 :50 に起床
 *   - 20分間 BLE スキャン（:50〜:10 をカバー）
 *   - 子機のアドバタイズ（毎時 :00〜:10）を受信してシリアルモニタへ表示
 *   - スキャン終了後、次の :50 まで RTC2 ディープスリープ
 *
 * タイミング例:
 *   :50 起床 → スキャン開始（20分）
 *   :00 子機がアドバタイズ開始 → 受信
 *   :10 スキャン終了 → 次の :50 まで約40分スリープ
 *   (次の :50) 起床 → スキャン開始 → ...
 *
 * 将来拡張:
 *   - 受信データを LTE-M で送信
 *   - 子機への時刻同期（親機アドバタイズに時刻を載せる）
 *
 * 対象ハード: XIAO nRF52840 + DS3231 + BLE（GPIO: DS3231 I2C = D4/D5）
 *   ※ Gateway 基板完成前の暫定構成（ブレッドボード or Flex 基板で動作可）
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <bluefruit.h>
#include <nrf.h>

// ============================================================
// ▼ 設定
// ============================================================

#define DEBUG_MODE         1      // 1: シリアルログ有効
#define DEBUG_NO_SLEEP     0      // 1: スリープスキップ（USB デバッグ用）

// スキャンウィンドウ設定
// 子機が :00〜:10 にアドバタイズするため、:50〜:10（20分間）スキャン
static const uint8_t  SCAN_START_MIN  = 50;   // スキャン開始分（:50）
static const uint32_t SCAN_DURATION_MIN = 20; // スキャン継続時間（分）

// DS3231 設定
#define DS3231_ADDR  0x68
#define DS3231_SET_TIME  0     // 1: 起動時に時刻書き込み（後で 0 に戻す）
#define DS3231_YEAR  26
#define DS3231_MONTH 6
#define DS3231_DAY   15
#define DS3231_HOUR  9
#define DS3231_MIN   50
#define DS3231_SEC   0

// BLE フィルタ（子機と一致させる）
static const uint8_t MFR_COMPANY_LO = 0xFF;
static const uint8_t MFR_COMPANY_HI = 0xFF;
static const uint8_t PKT_TYPE       = 0x03;   // Monita Flex v3.03

// ============================================================
// RTC2 ディープスリープ（BLE 非使用時）
// ============================================================
#define RTC2_PRESCALER        4095U
#define RTC2_TICKS_PER_SECOND 8U
#define RTC2_COUNTER_MASK     0x00FFFFFFU

static volatile bool s_rtc2Wake = false;

extern "C" void RTC2_IRQHandler(void) {
  if (NRF_RTC2->EVENTS_COMPARE[0]) {
    NRF_RTC2->EVENTS_COMPARE[0] = 0;
    (void)NRF_RTC2->EVENTS_COMPARE[0];
    NRF_RTC2->INTENCLR = RTC_INTENCLR_COMPARE0_Msk;
    NRF_RTC2->TASKS_STOP = 1;
    s_rtc2Wake = true;
  }
}

static void deepSleep(uint32_t minutes) {
#if DEBUG_MODE && DEBUG_NO_SLEEP
  Serial.printf("[Sleep SKIP] %u 分\n", minutes);
  delay(5000);
  return;
#endif

#if DEBUG_MODE
  Serial.printf("[Sleep] %u 分\n", minutes);
  Serial.flush();
  delay(100);
#endif

  if (minutes == 0) minutes = 1;
  uint64_t ticks64 = (uint64_t)minutes * 60ULL * RTC2_TICKS_PER_SECOND;
  if (ticks64 > RTC2_COUNTER_MASK) ticks64 = RTC2_COUNTER_MASK;
  uint32_t ticks = (uint32_t)ticks64;

  s_rtc2Wake = false;
  NRF_RTC2->TASKS_STOP  = 1;
  NRF_RTC2->TASKS_CLEAR = 1;
  NRF_RTC2->PRESCALER   = RTC2_PRESCALER;
  NRF_RTC2->EVTENCLR    = 0xFFFFFFFFU;
  NRF_RTC2->EVENTS_COMPARE[0] = 0;
  (void)NRF_RTC2->EVENTS_COMPARE[0];
  NRF_RTC2->CC[0]    = ticks;
  NRF_RTC2->INTENCLR = 0xFFFFFFFFU;
  NRF_RTC2->INTENSET = RTC_INTENSET_COMPARE0_Msk;
  NVIC_SetPriority(RTC2_IRQn, 7);
  NVIC_ClearPendingIRQ(RTC2_IRQn);
  NVIC_EnableIRQ(RTC2_IRQn);
  NRF_RTC2->TASKS_START = 1;

  while (!s_rtc2Wake) { __DSB(); __WFI(); }

  Wire.begin();
}

// ============================================================
// DS3231 時刻
// ============================================================
struct RtcTime { uint8_t hour, min, sec; };

static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

static bool ds3231GetTime(RtcTime &t) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(DS3231_ADDR, (uint8_t)3) < 3) return false;
  t.sec  = bcd2dec(Wire.read() & 0x7F);
  t.min  = bcd2dec(Wire.read() & 0x7F);
  t.hour = bcd2dec(Wire.read() & 0x3F);
  return true;
}

static void ds3231SetTime(uint8_t yr2, uint8_t mo, uint8_t dy,
                          uint8_t hr, uint8_t mn, uint8_t sc) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  Wire.write(dec2bcd(sc));
  Wire.write(dec2bcd(mn));
  Wire.write(dec2bcd(hr));
  Wire.write(0x01);
  Wire.write(dec2bcd(dy));
  Wire.write(dec2bcd(mo));
  Wire.write(dec2bcd(yr2));
  Wire.endTransmission();
}

// ============================================================
// 次のスキャン開始まで何分か計算
// スキャン開始 = 毎時 SCAN_START_MIN 分
// ============================================================
static uint32_t minutesUntilNextScan(const RtcTime &t) {
  uint32_t nowMin  = (uint32_t)t.hour * 60 + t.min;
  uint32_t scanMin = (nowMin / 60) * 60 + SCAN_START_MIN;  // 今時間の :50
  if (scanMin <= nowMin) scanMin += 60;                     // 過ぎていたら次の :50
  return scanMin - nowMin;
}

// 現在がスキャンウィンドウ内か
// ウィンドウ: :50〜:10（SCAN_START_MIN から SCAN_DURATION_MIN 分間）
static bool inScanWindow(const RtcTime &t) {
  uint8_t m = t.min;
  // :50〜:59 OR :00〜:09
  if (m >= SCAN_START_MIN) return true;
  if (m < (SCAN_START_MIN + SCAN_DURATION_MIN - 60)) return true;
  return false;
}

// ============================================================
// BLE スキャン
// ============================================================
struct ChildData {
  uint8_t  deviceId;
  int16_t  ch[4];
  uint16_t battMv;
  uint8_t  hour;
  uint8_t  min;
  int8_t   rssi;
  uint32_t count;    // 受信パケット数
};

static const int MAX_CHILDREN = 8;
static ChildData s_children[MAX_CHILDREN];
static int       s_childCount = 0;

// Device ID からインデックスを検索（なければ新規登録）
static int findOrAddChild(uint8_t deviceId) {
  for (int i = 0; i < s_childCount; i++) {
    if (s_children[i].deviceId == deviceId) return i;
  }
  if (s_childCount >= MAX_CHILDREN) return -1;
  int idx = s_childCount++;
  memset(&s_children[idx], 0, sizeof(ChildData));
  s_children[idx].deviceId = deviceId;
  return idx;
}

static void scanCallback(ble_gap_evt_adv_report_t* report) {
  uint8_t buf[32];
  uint8_t len = Bluefruit.Scanner.parseReportByType(
      report, BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, buf, sizeof(buf));

  if (len >= 16
      && buf[0] == MFR_COMPANY_LO
      && buf[1] == MFR_COMPANY_HI
      && buf[2] == PKT_TYPE) {

    uint8_t devId = buf[3];
    int idx = findOrAddChild(devId);
    if (idx < 0) { Bluefruit.Scanner.resume(); return; }

    ChildData &c = s_children[idx];
    for (int i = 0; i < 4; i++) {
      c.ch[i] = (int16_t)(buf[4 + i*2] | ((uint16_t)buf[5 + i*2] << 8));
    }
    c.battMv = (uint16_t)(buf[12] | ((uint16_t)buf[13] << 8));
    c.hour   = buf[14];
    c.min    = buf[15];
    c.rssi   = report->rssi;
    c.count++;

    // 初受信時のみ詳細ログ
    if (c.count == 1) {
#if DEBUG_MODE
      Serial.printf("\n  [子機検出] ID=0x%02X  RSSI=%d dBm  時刻=%02d:%02d\n",
                    devId, report->rssi, c.hour, c.min);
      for (int i = 0; i < 4; i++) {
        Serial.printf("    CH%d: %d\n", i + 1, c.ch[i]);
      }
      Serial.printf("    BATT: %u mV\n", c.battMv);
#endif
    } else if (c.count % 10 == 0) {
#if DEBUG_MODE
      Serial.print('.');
#endif
    }
  }
  Bluefruit.Scanner.resume();
}

static void printScanResult() {
  Serial.println(F("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
  Serial.println(F("  【スキャン結果】"));
  Serial.printf("  子機数: %d\n", s_childCount);
  for (int i = 0; i < s_childCount; i++) {
    ChildData &c = s_children[i];
    Serial.printf("  ─ ID=0x%02X  受信=%u pkts  RSSI=%d dBm\n",
                  c.deviceId, c.count, c.rssi);
    Serial.printf("    計測時刻: %02d:%02d\n", c.hour, c.min);
    for (int j = 0; j < 4; j++) {
      Serial.printf("    CH%d: %d\n", j + 1, c.ch[j]);
    }
    Serial.printf("    BATT: %u mV\n", c.battMv);
  }
  if (s_childCount == 0) {
    Serial.println(F("  [警告] 子機データなし"));
  }
  Serial.println(F("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));

  // TODO: LTE-M 送信（将来実装）
}

// ============================================================
// Arduino エントリ
// ============================================================
void setup() {
#if DEBUG_MODE
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();
  Serial.println(F("\n[v3.03 BLE親機] 起動"));
  Serial.printf("  スキャン: 毎時 :%02d〜（%u 分間）\n",
                SCAN_START_MIN, SCAN_DURATION_MIN);
#endif

  Wire.begin();

#if DS3231_SET_TIME
  ds3231SetTime(DS3231_YEAR, DS3231_MONTH, DS3231_DAY,
                DS3231_HOUR, DS3231_MIN, DS3231_SEC);
  Serial.println(F("[DS3231] 時刻設定完了。DS3231_SET_TIME を 0 に戻してください。"));
#endif

  // BLE 初期化（Central = スキャン専用）
  Bluefruit.begin(0, 1);
  Bluefruit.setName("Monita-Parent");
  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.useActiveScan(false);
  // 連続スキャン（interval = window = 100ms）→ 取りこぼし 0%
  Bluefruit.Scanner.setInterval(160, 160);

#if DEBUG_MODE
  Serial.println(F("[BLE] init OK"));
#endif
}

void loop() {
  Wire.begin();

  // DS3231 時刻確認
  RtcTime rtc;
  bool rtcOk = ds3231GetTime(rtc);

#if DEBUG_MODE
  if (rtcOk) {
    Serial.printf("\n[時刻] %02d:%02d:%02d\n", rtc.hour, rtc.min, rtc.sec);
  }
#endif

  if (!rtcOk || !inScanWindow(rtc)) {
    // スキャンウィンドウ外 → 次の :50 まで眠る
    uint32_t sleepMin = rtcOk ? minutesUntilNextScan(rtc) : 55;
    if (sleepMin == 0) sleepMin = 1;

#if DEBUG_MODE
    Serial.printf("[待機] 次のスキャンまで %u 分\n", sleepMin);
#endif
    deepSleep(sleepMin);
    return;
  }

  // ── スキャンウィンドウ ── (:50〜:10)
  s_childCount = 0;
  memset(s_children, 0, sizeof(s_children));

#if DEBUG_MODE
  Serial.printf("[SCAN] 開始（%u 分間）\n", SCAN_DURATION_MIN);
#endif

  Bluefruit.Scanner.start(0);  // 無期限（手動で stop）

  uint32_t scanMs = (uint32_t)SCAN_DURATION_MIN * 60000UL;
  uint32_t start  = millis();
  while (millis() - start < scanMs) {
    delay(1000);
    yield();
  }

  Bluefruit.Scanner.stop();

#if DEBUG_MODE
  Serial.println(F("\n[SCAN] 終了"));
#endif

  printScanResult();

  // 次の :50 まで眠る（約 40 分）
  if (ds3231GetTime(rtc)) {
    uint32_t sleepMin = minutesUntilNextScan(rtc);
    if (sleepMin == 0) sleepMin = 55;
    deepSleep(sleepMin);
  } else {
    deepSleep(40);  // DS3231 失敗時は固定 40 分
  }
}
