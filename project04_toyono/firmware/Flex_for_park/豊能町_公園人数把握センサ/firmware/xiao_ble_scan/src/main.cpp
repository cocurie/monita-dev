/**
 * BLE 親機 — 人数推定（RSSIクラスタリング付き）
 * 基板: Seeed XIAO nRF52840
 *
 * ═══════════════════════════════════════════════════
 * シリアルコマンド一覧（115200 bps / 改行コード: LF）
 * ═══════════════════════════════════════════════════
 *
 * d        フラッシュログを全件シリアル出力する（ENABLE_LOGGING=true 時のみ有効）
 *
 * e        フラッシュログを削除する（ENABLE_LOGGING=true 時のみ有効）
 *          削除後は次回書き込み時にヘッダ行付きで新規作成される
 *
 * c<N>     キャリブレーション開始。<N> に実際の人数を指定する。
 *          例: c18  → 18人いる状態でキャリブレーション開始
 *          ・SCAN_DURATION_MS（デフォルト30秒）のスキャンごとにサンプルを自動収集する
 *          ・CALIB_MIN_SAMPLES（デフォルト5回）集まると推奨パラメータを出力
 *          ・人数が変わったら再度 c<N> を送信することでリセットして再開できる
 *          出力例:
 *            [CALIB] #1  actual=18  MIN_HITS→3  CALIBRATION→1.20
 *            [CALIB] #2  actual=18  MIN_HITS→3  CALIBRATION→1.18
 *            ...
 *            === CALIB RESULT ===
 *              Samples : 5
 *              --- Paste into your code ---
 *              static int   const MIN_HITS    = 3;
 *              static float const CALIBRATION = 1.19;
 *            ====================
 *          → 表示された値をコード上部のパラメータに貼り付けて再ビルドする
 *
 * r        キャリブレーションの途中経過（現時点の推奨値）を出力する
 *
 * x        キャリブレーションモードを終了する
 *
 * ═══════════════════════════════════════════════════
 * キャリブレーションの仕組み
 * ═══════════════════════════════════════════════════
 * MIN_HITS を 1〜20 で総当たりし、ウィンドウ内の有効デバイス数が
 * 実際の人数に最も近くなる値を探す。その時の補正係数を CALIBRATION として記録。
 * 複数サンプルの平均を最終推奨値とする。
 *
 * ═══════════════════════════════════════════════════
 * フラッシュログ（ENABLE_LOGGING）
 * ═══════════════════════════════════════════════════
 * ENABLE_LOGGING = true にすると、スキャンサイクルごとの計測結果を
 * 内部フラッシュ（LittleFS）の /log.csv に追記する。
 * フォーマット: timestamp,people,devices,rms_dbfs,L,ML,MH,H
 * 電源を切ってもデータは保持され、次回起動時は追記される。
 * 容量目安: 約9日分（1分1件ペース）
 * ※ 音声列追加前の古い log.csv（3列）が残っている場合は e コマンドで削除してから使うこと。
 *
 * ═══════════════════════════════════════════════════
 * 音声（賑わい）解析
 * ═══════════════════════════════════════════════════
 * オンボード PDM マイク（Sense 版のみ搭載）で環境音を16kHzサンプリングし、
 * 1秒ごとに全体音圧（RMS dBFS）と4帯域のエネルギー（dBFS）を算出してシリアル出力する。
 *   帯域: L=125-500Hz  ML=500-2kHz  MH=2k-4kHz  H=4k-8kHz
 * 生の音声波形は保存・送信しない。集計値（dBFS）のみを記録する。
 * BLE のスキャン/スリープに関わらず常時サンプリングし、スキャン窓（30秒）の平均値を
 * スキャンサマリ・CSV ログに記録する。
 *
 * ═══════════════════════════════════════════════════
 * ログをCSVファイルとしてMacに保存する方法
 * ═══════════════════════════════════════════════════
 * firmware/ フォルダにある dump_log.py を使う。
 *
 * 手順:
 *   1. VS Code のシリアルモニタを閉じる（ポートの競合を避けるため）
 *   2. ターミナルで以下を実行:
 *        cd .../xiao_ble_scan
 *        python3 dump_log.py
 *      ポートを手動指定する場合:
 *        python3 dump_log.py /dev/cu.usbmodem101
 *   3. ~/Desktop/ble_log_YYYYMMDD_HHMMSS.csv に保存される
 *
 * 注意: pyserial が必要。未インストールの場合は以下でインストール:
 *        pip3 install pyserial
 */

#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>
#include <string.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <PDM.h>
#include <arduinoFFT.h>
using namespace Adafruit_LittleFS_Namespace;

// ── 出力 ─────────────────────────
static bool const OUTPUT_RAW_LOG = false;
static bool const OUTPUT_PEOPLE  = true;

// ── ログ設定 ──────────────────────
static bool const ENABLE_LOGGING = false;    // false にするとフラッシュ書き込みなし
static char const* LOG_FILE      = "/log.csv";

// ── スキャン/スリープ設定 ──────────
static uint32_t const SCAN_DURATION_MS  = 30000;  // スキャン時間 (ms)
static uint32_t const SLEEP_DURATION_MS = 90000;  // スリープ時間 (ms)

// ── LED設定 ──────────────────────
// XIAO nRF52840 の LED はアクティブ LOW（LOW=点灯, HIGH=消灯）
static uint32_t const LED_BLINK_MS = 1000;  // 点滅間隔 (ms)

// ── パラメータ ───────────────────
static int const MIN_HITS         = 12;
static int const RSSI_THRESHOLD   = -65;
static int const RSSI_MERGE_GAP   = 3;   // ★クラスタ幅
static float const CALIBRATION    = 1.0;

// ── BLEスキャン ─────────────────
static uint16_t const SCAN_INTERVAL_MS = 150;
static uint16_t const SCAN_WINDOW_MS   = 100;

// ── PDM / 音声 ───────────────────
static uint8_t  const PDM_GAIN         = 30;      // 0〜80（クリップなら下げる）
static uint32_t const SENSOR_REPORT_MS = 1000;    // 音声値のシリアル出力間隔

static uint16_t const FFT_SIZE      = 256;        // FFT 点数（2 のべき乗）
static float    const SAMPLE_RATE   = 16000.0f;
// dBFS 基準: Hann 窓後フルスケール振幅 ≈ A * FFT_SIZE/4
static double   const FFT_FULL_SCALE = (FFT_SIZE / 4.0) * 32768.0;

static int   const NUM_BANDS      = 4;
static float const BAND_LOW_HZ [] = { 125.0f,  500.0f, 2000.0f, 4000.0f };
static float const BAND_HIGH_HZ[] = { 500.0f, 2000.0f, 4000.0f, 8000.0f };
static char const* BAND_NAME[]    = { "L", "ML", "MH", "H" };

// ── デバイス保持 ────────────────
#define MAX_DEVICES 64

struct Device {
  uint8_t mac[6];
  int count;
  int rssi_sum;
};

Device devices[MAX_DEVICES];
int deviceCount = 0;

// ── LittleFS ファイルハンドル ────
File logFile(InternalFS);

// ── キャリブレーション状態 ─────────
static bool  calibMode       = false;
static int   calibActual     = 0;
static int   calibSamples    = 0;
static float calibRatioSum   = 0.0f;
static int   calibMinHitsSum = 0;
#define CALIB_MIN_SAMPLES 5

// ── Serial コマンドバッファ ─────────
static char cmdBuf[16];
static int  cmdLen = 0;

// ── PDM / 音声 ───────────────────
static int16_t const PDM_BUF_SAMPLES = 512;
static int16_t      pdmBuf[PDM_BUF_SAMPLES];
static volatile int pdmReady = 0;

static uint16_t const RING_SIZE = FFT_SIZE * 4;   // 1024（2 のべき乗）
static uint16_t const RING_MASK = RING_SIZE - 1;
static int16_t      ringBuf[RING_SIZE];
static uint16_t     ringHead = 0;
static uint16_t     ringTail = 0;

static float vReal[FFT_SIZE];
static float vImag[FFT_SIZE];
static ArduinoFFT<float> FFT(vReal, vImag, FFT_SIZE, SAMPLE_RATE);

// 1 秒ウィンドウ集計
static double   rmsAccumSq = 0.0;
static uint32_t rmsAccumN  = 0;
static double   bandAccum[NUM_BANDS] = {};
static uint32_t bandWindows = 0;

// スキャン窓（30秒）集計
static double scanRmsSum             = 0.0;
static double scanBandSum[NUM_BANDS] = {};
static int    scanSnapCount          = 0;

static uint32_t lastSensorMs = 0;

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
void logRecord(int people, int devCount, double rmsDb, double bandDb[]) {
  if (!ENABLE_LOGGING) return;

  logFile.open(LOG_FILE, FILE_O_WRITE);
  if (!logFile) return;
  logFile.seek(logFile.size());  // 末尾に追記

  char ts[10];
  uint32_t s = millis() / 1000UL;
  snprintf(ts, sizeof(ts), "%02lu:%02lu:%02lu",
           s / 3600, (s % 3600) / 60, s % 60);

  logFile.print(ts);
  logFile.print(",");
  logFile.print(people);
  logFile.print(",");
  logFile.print(devCount);
  logFile.print(",");
  logFile.print(rmsDb, 1);
  for (int b = 0; b < NUM_BANDS; b++) {
    logFile.print(",");
    logFile.print(bandDb[b], 1);
  }
  logFile.println();
  logFile.close();
}

/** フラッシュログファイルを削除する */
void eraseLog() {
  if (!ENABLE_LOGGING) {
    Serial.println("[LOG] Logging is disabled.");
    return;
  }
  if (InternalFS.exists(LOG_FILE)) {
    InternalFS.remove(LOG_FILE);
    Serial.println("[LOG] Log file erased.");
  } else {
    Serial.println("[LOG] No log file to erase.");
  }
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

// ───────────────────────────────
// キャリブレーション
// ───────────────────────────────

/**
 * MIN_HITS を 1〜20 で総当たりし、
 * 有効デバイス数が actual に最も近く
 * かつ CALIBRATION が 1.0 に近くなる組み合わせを返す
 */
void findBestParams(int actual, int &outMinHits, float &outCalib) {
  outMinHits = MIN_HITS;
  outCalib   = CALIBRATION;
  float bestScore = 1e9f;

  for (int minH = 1; minH <= 20; minH++) {
    int q = 0;
    for (int i = 0; i < deviceCount; i++) {
      if (devices[i].count >= minH) q++;
    }
    if (q == 0) break;
    float calib = (float)actual / (float)q;
    float score = fabsf(calib - 1.0f);  // 1.0 に近いほど良い
    if (score < bestScore) {
      bestScore  = score;
      outMinHits = minH;
      outCalib   = calib;
    }
  }
}

/** キャリブレーション結果を出力 */
void printCalibResult() {
  if (calibSamples == 0) {
    Serial.println("[CALIB] No data. Send c<N> to start. e.g. c18");
    return;
  }
  int   minHits = (calibMinHitsSum + calibSamples / 2) / calibSamples;
  float calib   = calibRatioSum / (float)calibSamples;

  Serial.println("=== CALIB RESULT ===");
  Serial.print  ("  Samples : "); Serial.println(calibSamples);
  Serial.println("  --- Paste into your code ---");
  Serial.print  ("  static int   const MIN_HITS    = "); Serial.print(minHits);    Serial.println(";");
  Serial.print  ("  static float const CALIBRATION = "); Serial.print(calib, 2);  Serial.println(";");
  Serial.println("====================");
}

/** シリアルコマンドを解釈して実行 */
void processCommand(const char* cmd) {
  if (cmd[0] == '\0') return;

  if (cmd[0] == 'd' || cmd[0] == 'D') {
    dumpLog();

  } else if (cmd[0] == 'e' || cmd[0] == 'E') {
    eraseLog();

  } else if (cmd[0] == 'c' || cmd[0] == 'C') {
    const char* p = cmd + 1;
    while (*p == ' ') p++;
    int n = atoi(p);
    if (n > 0) {
      calibMode       = true;
      calibActual     = n;
      calibSamples    = 0;
      calibRatioSum   = 0.0f;
      calibMinHitsSum = 0;
      Serial.print("[CALIB] Ground truth = ");
      Serial.print(n);
      Serial.print(" people. Collecting ");
      Serial.print(CALIB_MIN_SAMPLES);
      Serial.println(" samples...");
    } else {
      Serial.println("[CALIB] Usage: c<N>  e.g. c18");
    }

  } else if (cmd[0] == 'r' || cmd[0] == 'R') {
    printCalibResult();

  } else if (cmd[0] == 'x' || cmd[0] == 'X') {
    calibMode = false;
    Serial.println("[CALIB] Exited calibration mode.");

  } else {
    Serial.println("[CMD] d=dump  e=erase  c<N>=calib  r=result  x=exit calib");
  }
}

/** シリアル入力を読んでコマンドバッファに蓄積し、行完結時に実行 */
void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) {
        cmdBuf[cmdLen] = '\0';
        processCommand(cmdBuf);
        cmdLen = 0;
      }
    } else if (cmdLen < (int)sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
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
// PDM コールバック（割り込みコンテキスト）
// ───────────────────────────────

static void onPDMdata() {
  int bytes = PDM.available();
  if (bytes > (int)(PDM_BUF_SAMPLES * sizeof(int16_t)))
    bytes = PDM_BUF_SAMPLES * sizeof(int16_t);
  PDM.read(pdmBuf, bytes);
  pdmReady = bytes / (int)sizeof(int16_t);
}

// ───────────────────────────────
// PDM 処理（main loop から毎フレーム呼ぶ）
// ───────────────────────────────

static void calcBandMag(float outMag[]) {
  const float freqRes = SAMPLE_RATE / (float)FFT_SIZE;
  for (int b = 0; b < NUM_BANDS; b++) {
    int binLo = max(1,                (int)(BAND_LOW_HZ[b]  / freqRes + 0.5f));
    int binHi = min((int)(FFT_SIZE/2), (int)(BAND_HIGH_HZ[b] / freqRes + 0.5f));
    double sumSq = 0.0; int cnt = 0;
    for (int i = binLo; i < binHi; i++) { sumSq += (double)vReal[i]*(double)vReal[i]; cnt++; }
    outMag[b] = (cnt > 0) ? (float)sqrt(sumSq / cnt) : 0.0f;
  }
}

static void processPDM() {
  // --- PDM コールバックのサンプルをリングバッファへ転記 ---
  if (pdmReady > 0) {
    int n = pdmReady; pdmReady = 0;
    for (int i = 0; i < n; i++) {
      int16_t s = pdmBuf[i];
      ringBuf[ringHead & RING_MASK] = s;
      ringHead++;
      rmsAccumSq += (double)s * (double)s;
    }
    rmsAccumN += (uint32_t)n;
  }
  // --- FFT_SIZE サンプル溜まったら FFT 処理 ---
  while ((uint16_t)(ringHead - ringTail) >= FFT_SIZE) {
    for (int i = 0; i < FFT_SIZE; i++) {
      vReal[i] = (float)ringBuf[(ringTail + i) & RING_MASK];
      vImag[i] = 0.0f;
    }
    ringTail += FFT_SIZE;
    FFT.windowing(FFTWindow::Hann, FFTDirection::Forward);
    FFT.compute(FFTDirection::Forward);
    FFT.complexToMagnitude();
    float mag[NUM_BANDS]; calcBandMag(mag);
    for (int b = 0; b < NUM_BANDS; b++) bandAccum[b] += (double)mag[b];
    bandWindows++;
  }
}

// ───────────────────────────────
// 1 秒センサティック — 計算・出力・スキャン窓累積・リセット
// ───────────────────────────────

static void sensorTick(bool isScan) {
  double rms  = (rmsAccumN > 0) ? sqrt(rmsAccumSq / (double)rmsAccumN) : 0.0;
  double dbfs = (rms > 0.0)     ? 20.0 * log10(rms / 32768.0)          : -120.0;

  double bDb[NUM_BANDS];
  for (int b = 0; b < NUM_BANDS; b++) {
    double avg = (bandWindows > 0) ? bandAccum[b] / (double)bandWindows : 0.0;
    bDb[b] = (avg > 0.0) ? 20.0 * log10(avg / FFT_FULL_SCALE) : -120.0;
  }

  Serial.print("[");
  printTimestamp();
  Serial.print(isScan ? "|SCAN ] " : "|SLEEP] ");
  Serial.print("rms="); Serial.print(dbfs, 1); Serial.print(" dBFS");
  for (int b = 0; b < NUM_BANDS; b++) {
    Serial.print("  "); Serial.print(BAND_NAME[b]);
    Serial.print("="); Serial.print(bDb[b], 1);
  }
  Serial.println();

  if (isScan) {
    scanRmsSum += dbfs;
    for (int b = 0; b < NUM_BANDS; b++) scanBandSum[b] += bDb[b];
    scanSnapCount++;
  }

  rmsAccumSq = 0.0; rmsAccumN = 0;
  for (int b = 0; b < NUM_BANDS; b++) bandAccum[b] = 0.0;
  bandWindows = 0;
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
  Serial.println("  Commands: d=dump  e=erase  c<N>=calib(e.g.c18)  r=result  x=exit calib");

  // ── フラッシュ初期化 ─────────────
  if (ENABLE_LOGGING) {
    InternalFS.begin();
    if (!InternalFS.exists(LOG_FILE)) {
      logFile.open(LOG_FILE, FILE_O_WRITE);
      logFile.println("timestamp,people,devices,rms_dbfs,L,ML,MH,H");
      logFile.close();
      Serial.println("[LOG] Created new log file.");
    } else {
      Serial.println("[LOG] Log file found. Appending.");
    }
    Serial.println("[LOG] Send 'd' to dump log.");
  }

  // ── LED 初期化 ───────────────────
  pinMode(LED_BLUE, OUTPUT);
  digitalWrite(LED_BLUE, HIGH);  // 消灯

  // ── PDM 初期化 ───────────────────
  PDM.onReceive(onPDMdata);
  PDM.setGain(PDM_GAIN);
  if (!PDM.begin(1, (int)SAMPLE_RATE)) {
    Serial.println("[ERR] PDM.begin() failed — Sense ボードか確認");
    while (1) delay(500);
  }
  Serial.print("[PDM] mono 16kHz  gain="); Serial.println(PDM_GAIN);
  lastSensorMs = millis();

  Bluefruit.begin(1, 0);
  Bluefruit.setName("PeopleCounter");

  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.useActiveScan(false);
  Bluefruit.Scanner.setInterval(
    (uint16_t)(SCAN_INTERVAL_MS * 1000 / 625),
    (uint16_t)(SCAN_WINDOW_MS   * 1000 / 625)
  );

}

// ───────────────────────────────
// loop  — スキャン → 出力 → スリープ のサイクル
// ───────────────────────────────

void loop() {

  // ① デバイスリスト・音声スキャン窓集計をリセットしてスキャン開始
  deviceCount   = 0;
  scanRmsSum    = 0.0;
  scanSnapCount = 0;
  for (int b = 0; b < NUM_BANDS; b++) scanBandSum[b] = 0.0;

  Bluefruit.Scanner.start(0);
  Serial.print("[SCAN] ");
  Serial.print(SCAN_DURATION_MS / 1000);
  Serial.println("s start");

  // ② SCAN_DURATION_MS の間スキャン（青LED点滅・コマンドも受け付ける）
  uint32_t scanEnd   = millis() + SCAN_DURATION_MS;
  uint32_t lastBlink = millis();
  bool     ledOn     = false;

  while (millis() < scanEnd) {
    handleSerial();
    processPDM();
    if (millis() - lastSensorMs >= SENSOR_REPORT_MS) {
      sensorTick(true);
      lastSensorMs += SENSOR_REPORT_MS;
    }

    // 青LED点滅
    if (millis() - lastBlink >= LED_BLINK_MS) {
      ledOn = !ledOn;
      digitalWrite(LED_BLUE, ledOn ? LOW : HIGH);
      lastBlink = millis();
    }

    delay(5);
  }

  // ③ スキャン停止・LED消灯
  Bluefruit.Scanner.stop();
  digitalWrite(LED_BLUE, HIGH);  // 消灯（スリープ中は消す）

  // ④ 人数推定・音声集計・出力
  double avgRms = (scanSnapCount > 0) ? scanRmsSum / (double)scanSnapCount : -120.0;
  double avgBand[NUM_BANDS];
  for (int b = 0; b < NUM_BANDS; b++)
    avgBand[b] = (scanSnapCount > 0) ? scanBandSum[b] / (double)scanSnapCount : -120.0;

  if (OUTPUT_PEOPLE) {
    int people = estimatePeople();

    Serial.println("==============================");
    Serial.print("[");
    printTimestamp();
    Serial.print("] People=");
    Serial.print(people);
    Serial.print(" (devices=");
    Serial.print(deviceCount);
    Serial.print(")");
    Serial.print("  rms="); Serial.print(avgRms, 1);
    for (int b = 0; b < NUM_BANDS; b++) {
      Serial.print("  "); Serial.print(BAND_NAME[b]); Serial.print("="); Serial.print(avgBand[b], 1);
    }
    Serial.println();

    logRecord(people, deviceCount, avgRms, avgBand);
  }

  // ⑤ キャリブレーション処理
  if (calibMode) {
    int   bestMinHits;
    float bestCalib;
    findBestParams(calibActual, bestMinHits, bestCalib);
    calibMinHitsSum += bestMinHits;
    calibRatioSum   += bestCalib;
    calibSamples++;

    Serial.print("[CALIB] #"); Serial.print(calibSamples);
    Serial.print("  actual=");       Serial.print(calibActual);
    Serial.print("  MIN_HITS→");    Serial.print(bestMinHits);
    Serial.print("  CALIBRATION→"); Serial.println(bestCalib, 2);

    if (calibSamples >= CALIB_MIN_SAMPLES) {
      printCalibResult();
      calibMode = false;
    }
  }

  // ⑥ スリープ（delay は nRF52 では低消費電力スリープ）
  Serial.print("[SLEEP] ");
  Serial.print(SLEEP_DURATION_MS / 1000);
  Serial.println("s");
  Serial.flush();

  uint32_t sleepEnd = millis() + SLEEP_DURATION_MS;
  while (millis() < sleepEnd) {
    handleSerial();
    processPDM();
    if (millis() - lastSensorMs >= SENSOR_REPORT_MS) {
      sensorTick(false);
      lastSensorMs += SENSOR_REPORT_MS;
    }
    delay(5);
  }
}
