/**
 * BLE 親機 — 人数推定（RSSIクラスタリング付き）
 * 基板: Seeed XIAO nRF52840
 */

#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>
#include <string.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;

// ── 出力 ─────────────────────────
static bool const OUTPUT_RAW_LOG = false;
static bool const OUTPUT_PEOPLE  = true;

// ── ログ設定 ──────────────────────
static bool const ENABLE_LOGGING = true;    // false にするとフラッシュ書き込みなし
static char const* LOG_FILE      = "/log.csv";

// ── パラメータ ───────────────────
static uint32_t const WINDOW_MS   = 60000;
static int const MIN_HITS         = 5;
static int const RSSI_THRESHOLD   = -65;
static int const RSSI_MERGE_GAP   = 3;   // ★クラスタ幅
static float const CALIBRATION    = 0.70;

// ── BLEスキャン ─────────────────
static uint16_t const SCAN_INTERVAL_MS = 300;
static uint16_t const SCAN_WINDOW_MS   = 30;

// ── デバイス保持 ────────────────
#define MAX_DEVICES 64

struct Device {
  uint8_t mac[6];
  int count;
  int rssi_sum;
};

Device devices[MAX_DEVICES];
int deviceCount = 0;

uint32_t windowStart = 0;

// ── LittleFS ファイルハンドル ────
File logFile(InternalFS);

// ───────────────────────────────
// ユーティリティ
// ───────────────────────────────

/** 起動からの経過時間を "HH:MM:SS" 形式で返す */
void printTimestamp() {
  uint32_t s = millis() / 1000UL;
  uint32_t h = s / 3600;
  uint32_t m = (s % 3600) / 60;
  uint32_t sec = s % 60;
  char buf[10];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", h, m, sec);
  Serial.print(buf);
}

/** 現在の経過時間を CSV 1行としてフラッシュに追記 */
void logRecord(int people, int devCount) {
  if (!ENABLE_LOGGING) return;

  logFile.open(LOG_FILE, FILE_O_WRITE);
  if (!logFile) return;
  logFile.seek(0, SeekEnd);  // 末尾に追記

  char ts[10];
  uint32_t s = millis() / 1000UL;
  snprintf(ts, sizeof(ts), "%02lu:%02lu:%02lu",
           s / 3600, (s % 3600) / 60, s % 60);

  logFile.print(ts);
  logFile.print(",");
  logFile.print(people);
  logFile.print(",");
  logFile.println(devCount);
  logFile.close();
}

/** フラッシュに保存された全レコードをシリアルに出力 */
void dumpLog() {
  if (!ENABLE_LOGGING) {
    Serial.println("[LOG] Logging is disabled.");
    return;
  }
  logFile.open(LOG_FILE, FILE_O_READ);
  if (!logFile) {
    Serial.println("[LOG] No log file found.");
    return;
  }
  Serial.println("=== LOG DUMP ===");
  while (logFile.available()) {
    Serial.write(logFile.read());
  }
  logFile.close();
  Serial.println("=== END ===");
}

int findDevice(uint8_t* mac) {
  for (int i = 0; i < deviceCount; i++) {
    if (memcmp(devices[i].mac, mac, 6) == 0) return i;
  }
  return -1;
}

void updateDevice(uint8_t* mac, int rssi) {
  if (rssi < RSSI_THRESHOLD) return;

  int idx = findDevice(mac);

  if (idx >= 0) {
    devices[idx].count++;
    devices[idx].rssi_sum += rssi;
  } else if (deviceCount < MAX_DEVICES) {
    memcpy(devices[deviceCount].mac, mac, 6);
    devices[deviceCount].count = 1;
    devices[deviceCount].rssi_sum = rssi;
    deviceCount++;
  }
}

// ───────────────────────────────
// 人数推定（クラスタリング）
// ───────────────────────────────

int estimatePeople() {

  // ① 有効デバイスのRSSI平均を配列へ
  int rssiList[MAX_DEVICES];
  int n = 0;

  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].count >= MIN_HITS) {
      int avg = devices[i].rssi_sum / devices[i].count;
      rssiList[n++] = avg;
    }
  }

  if (n == 0) return 0;

  // ② RSSIでソート（強い順）
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (rssiList[i] < rssiList[j]) {
        int tmp = rssiList[i];
        rssiList[i] = rssiList[j];
        rssiList[j] = tmp;
      }
    }
  }

  // ③ クラスタリング
  int people = 0;
  bool used[MAX_DEVICES] = {false};

  for (int i = 0; i < n; i++) {
    if (used[i]) continue;

    people++;               // 新しい人
    used[i] = true;

    for (int j = i + 1; j < n; j++) {
      if (used[j]) continue;

      if (abs(rssiList[i] - rssiList[j]) <= RSSI_MERGE_GAP) {
        used[j] = true;     // 同一人物として潰す
      }
    }
  }

  // ④ 補正
  return (int)(people * CALIBRATION);
}

// ───────────────────────────────
// スキャンコールバック
// ───────────────────────────────

static void scanCallback(ble_gap_evt_adv_report_t *report) {

  updateDevice(report->peer_addr.addr, report->rssi);

  if (OUTPUT_RAW_LOG) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             report->peer_addr.addr[5], report->peer_addr.addr[4],
             report->peer_addr.addr[3], report->peer_addr.addr[2],
             report->peer_addr.addr[1], report->peer_addr.addr[0]);

    Serial.print("[");
    Serial.print(millis() / 1000UL);
    Serial.print("s] ");
    Serial.print(macStr);
    Serial.print(" RSSI=");
    Serial.println(report->rssi);
  }

  Bluefruit.Scanner.resume();
}

// ───────────────────────────────
// setup
// ───────────────────────────────

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Serial.println("\n--- BLE People Counter (Clustering) ---");

  // ── フラッシュ初期化 ─────────────
  if (ENABLE_LOGGING) {
    InternalFS.begin();
    if (!InternalFS.exists(LOG_FILE)) {
      logFile.open(LOG_FILE, FILE_O_WRITE);
      logFile.println("timestamp,people,devices");
      logFile.close();
      Serial.println("[LOG] Created new log file.");
    } else {
      Serial.println("[LOG] Log file found. Appending.");
    }
    Serial.println("[LOG] Send 'd' to dump log.");
  }

  Bluefruit.begin(1, 0);
  Bluefruit.setName("PeopleCounter");

  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.useActiveScan(false);
  Bluefruit.Scanner.setInterval(
    (uint16_t)(SCAN_INTERVAL_MS * 1000 / 625),
    (uint16_t)(SCAN_WINDOW_MS   * 1000 / 625)
  );

  Bluefruit.Scanner.start(0);

  windowStart = millis();
}

// ───────────────────────────────
// loop
// ───────────────────────────────

void loop() {
  uint32_t now = millis();

  // ── Serial コマンド処理 ──────────
  if (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 'd' || cmd == 'D') dumpLog();
  }

  if (now - windowStart >= WINDOW_MS) {

    if (OUTPUT_PEOPLE) {
      int people = estimatePeople();

      Serial.println("==============================");
      Serial.print("[");
      printTimestamp();
      Serial.print("] People=");
      Serial.print(people);
      Serial.print(" (devices=");
      Serial.print(deviceCount);
      Serial.println(")");

      logRecord(people, deviceCount);  // フラッシュに記録
    }

    // リセット
    deviceCount = 0;
    windowStart = now;
  }
}
