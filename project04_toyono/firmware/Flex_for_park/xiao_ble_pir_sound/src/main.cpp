/**
 * BLE + PIR + 音声（PDM）統合モニタ — Seeed XIAO nRF52840 Sense
 * project04_toyono — 豊能町 公園人流測定
 *
 * ═══ 動作サイクル（120 秒周期）═══
 *   [SCAN  30s] BLE スキャン + PIR カウント + 音声計測
 *   [SLEEP 90s] BLE 停止    + PIR カウント + 音声計測（継続）
 *
 * ═══ シリアル出力 ═══
 *   1 秒ごと（常時）:
 *     [HH:MM:SS|SCAN ] PIR=N  rms=-38.1 dBFS  peak=2048  L=-44.1  ML=-47.2  MH=-70.3  H=-83.1
 *   スキャン終了時:
 *     [HH:MM:SS] People=5 (devices=12)  pir_scan=8  rms=-40.1  L=-45.0  ML=-48.3  MH=-70.1  H=-82.5
 *
 * ═══ CSV ログ（LittleFS /log.csv）═══
 *   timestamp,people,devices,pir_scan,rms_dbfs,L,ML,MH,H
 *   ※ pir_scan と音声値は 30 秒スキャン窓の集計
 *
 * ═══ シリアルコマンド（115200 bps / 改行: LF）═══
 *   d  ログを全件シリアル出力
 *   e  ログファイルを削除
 *   c<N>  キャリブレーション開始（例: c5）
 *   r  キャリブレーション途中結果
 *   x  キャリブレーション終了
 *
 * ═══ PDM + BLE 共存について ═══
 *   nRF52840 の PDM DMA と BLE SoftDevice は独立したハードウェアブロックで動作する。
 *   SoftDevice 高優先割り込み中に PDM 割り込みが遅延することがあるが、
 *   1 秒平均値への影響は軽微。
 */

#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>
#include <string.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <PDM.h>
#include <arduinoFFT.h>
using namespace Adafruit_LittleFS_Namespace;

// =========================================================================
// 設定
// =========================================================================

// ---- BLE / スキャンサイクル ----------------------------------------------
static bool     const OUTPUT_RAW_LOG    = false;   // true: MAC+RSSI を毎回出力
static bool     const OUTPUT_PEOPLE     = true;
static bool     const ENABLE_LOGGING    = true;    // false: フラッシュ書き込みなし
static char     const* LOG_FILE         = "/log.csv";

static uint32_t const SCAN_DURATION_MS  = 30000;   // スキャン時間
static uint32_t const SLEEP_DURATION_MS = 90000;   // スリープ（BLE停止）時間
static uint32_t const LED_BLINK_MS      = 1000;    // スキャン中 LED 点滅間隔

static int      const MIN_HITS          = 12;      // 有効デバイス最小検出回数
static int      const RSSI_THRESHOLD    = -65;     // dBm 閾値（これ以上を有効）
static int      const RSSI_MERGE_GAP    = 3;       // クラスタ幅（dBm）
static float    const CALIBRATION       = 1.0f;    // 人数補正係数

static uint16_t const SCAN_INTERVAL_MS  = 150;     // スキャン間隔
static uint16_t const SCAN_WINDOW_MS    = 100;     // スキャン窓時間

// ---- PIR ----------------------------------------------------------------
static uint8_t  const PIR_PIN           = D2;      // PIR OUT 接続ピン
static uint32_t const PIR_DEBOUNCE_MS   = 2000;    // 再検出抑止時間

// ---- PDM / 音声 ---------------------------------------------------------
static uint8_t  const PDM_GAIN          = 30;      // 0〜80（クリップなら下げる）
static uint32_t const SENSOR_REPORT_MS  = 1000;    // センサ出力間隔

static const uint16_t FFT_SIZE          = 256;     // FFT 点数（2 のべき乗）
static const float    SAMPLE_RATE       = 16000.0f;
// dBFS 基準: Hann 窓後フルスケール振幅 ≈ A * FFT_SIZE/4
static const double   FFT_FULL_SCALE    = (FFT_SIZE / 4.0) * 32768.0;

static const int      NUM_BANDS         = 4;
static const float    BAND_LOW_HZ []    = { 125.0f,  500.0f, 2000.0f, 4000.0f };
static const float    BAND_HIGH_HZ[]    = { 500.0f, 2000.0f, 4000.0f, 8000.0f };
static const char*    BAND_NAME[]       = { "L", "ML", "MH", "H" };

// =========================================================================
// グローバル変数
// =========================================================================

// ---- BLE ----------------------------------------------------------------
#define MAX_DEVICES 64
struct Device { uint8_t mac[6]; int count; int rssi_sum; };
static Device    devices[MAX_DEVICES];
static int       deviceCount = 0;

// ---- LittleFS -----------------------------------------------------------
static File      logFile(InternalFS);

// ---- キャリブレーション -------------------------------------------------
static bool      calibMode       = false;
static int       calibActual     = 0;
static int       calibSamples    = 0;
static float     calibRatioSum   = 0.0f;
static int       calibMinHitsSum = 0;
#define CALIB_MIN_SAMPLES 5

// ---- シリアルコマンドバッファ -------------------------------------------
static char      cmdBuf[16];
static int       cmdLen = 0;

// ---- PIR ----------------------------------------------------------------
static uint32_t  pirLifetime    = 0;    // 通算検出数
static uint32_t  pirScanCount   = 0;    // 現スキャン窓の検出数（ログ用）
static uint32_t  pir1sCount     = 0;    // 1 秒ウィンドウの検出数（表示用）
static uint32_t  pirLastDetect  = 0;
static bool      pirPrevState   = false;

// ---- PDM ----------------------------------------------------------------
static const int    PDM_BUF_SAMPLES = 512;
static int16_t      pdmBuf[PDM_BUF_SAMPLES];
static volatile int pdmReady        = 0;

static const uint16_t RING_SIZE = FFT_SIZE * 4;   // 1024（2 のべき乗）
static const uint16_t RING_MASK = RING_SIZE - 1;
static int16_t      ringBuf[RING_SIZE];
static uint16_t     ringHead = 0;
static uint16_t     ringTail = 0;

static float        vReal[FFT_SIZE];
static float        vImag[FFT_SIZE];
static ArduinoFFT<float> FFT(vReal, vImag, FFT_SIZE, SAMPLE_RATE);

// ---- 1 秒ウィンドウ集計 ------------------------------------------------
static double    rmsAccumSq  = 0.0;
static uint32_t  rmsAccumN   = 0;
static int16_t   rmsAccumPk  = 0;
static double    bandAccum[NUM_BANDS] = {};
static uint32_t  bandWindows = 0;

// ---- スキャン窓集計（30 秒平均） ----------------------------------------
static double    scanRmsSum              = 0.0;
static double    scanBandSum[NUM_BANDS]  = {};
static int       scanSnapCount           = 0;

// ---- タイミング ---------------------------------------------------------
static uint32_t  lastSensorMs = 0;

// =========================================================================
// BLE ユーティリティ
// =========================================================================

static int findDevice(uint8_t* mac) {
  for (int i = 0; i < deviceCount; i++)
    if (memcmp(devices[i].mac, mac, 6) == 0) return i;
  return -1;
}

static void updateDevice(uint8_t* mac, int rssi) {
  if (rssi < RSSI_THRESHOLD) return;
  int idx = findDevice(mac);
  if (idx >= 0) {
    devices[idx].count++;
    devices[idx].rssi_sum += rssi;
  } else if (deviceCount < MAX_DEVICES) {
    memcpy(devices[deviceCount].mac, mac, 6);
    devices[deviceCount].count    = 1;
    devices[deviceCount].rssi_sum = rssi;
    deviceCount++;
  }
}

static int estimatePeople() {
  int rssiList[MAX_DEVICES];
  int n = 0;
  for (int i = 0; i < deviceCount; i++)
    if (devices[i].count >= MIN_HITS)
      rssiList[n++] = devices[i].rssi_sum / devices[i].count;
  if (n == 0) return 0;

  // バブルソート（強い順）
  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
      if (rssiList[i] < rssiList[j]) { int t = rssiList[i]; rssiList[i] = rssiList[j]; rssiList[j] = t; }

  // クラスタリング
  bool used[MAX_DEVICES] = {};
  int people = 0;
  for (int i = 0; i < n; i++) {
    if (used[i]) continue;
    people++; used[i] = true;
    for (int j = i + 1; j < n; j++)
      if (!used[j] && abs(rssiList[i] - rssiList[j]) <= RSSI_MERGE_GAP) used[j] = true;
  }
  return (int)(people * CALIBRATION);
}

// =========================================================================
// タイムスタンプ出力（HH:MM:SS）
// =========================================================================

static void printTimestamp() {
  uint32_t s = millis() / 1000UL;
  char buf[10];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", s/3600, (s%3600)/60, s%60);
  Serial.print(buf);
}

// =========================================================================
// LittleFS ログ
// =========================================================================

static void logRecord(int people, int devCnt,
                      uint32_t pirScan,
                      double   rmsDb,
                      double   bDb[]) {
  if (!ENABLE_LOGGING) return;
  logFile.open(LOG_FILE, FILE_O_WRITE);
  if (!logFile) return;
  logFile.seek(logFile.size());  // 末尾追記

  uint32_t s = millis() / 1000UL;
  char ts[10];
  snprintf(ts, sizeof(ts), "%02lu:%02lu:%02lu", s/3600, (s%3600)/60, s%60);

  logFile.print(ts);        logFile.print(',');
  logFile.print(people);    logFile.print(',');
  logFile.print(devCnt);    logFile.print(',');
  logFile.print(pirScan);   logFile.print(',');
  logFile.print(rmsDb, 1);
  for (int b = 0; b < NUM_BANDS; b++) { logFile.print(','); logFile.print(bDb[b], 1); }
  logFile.println();
  logFile.close();
}

static void dumpLog() {
  if (!ENABLE_LOGGING) { Serial.println(F("[LOG] Logging disabled.")); return; }
  logFile.open(LOG_FILE, FILE_O_READ);
  if (!logFile) { Serial.println(F("[LOG] No log file.")); return; }
  Serial.println(F("=== LOG DUMP ==="));
  while (logFile.available()) Serial.write(logFile.read());
  logFile.close();
  Serial.println(F("\n=== END ==="));
}

static void eraseLog() {
  if (!ENABLE_LOGGING) { Serial.println(F("[LOG] Logging disabled.")); return; }
  if (InternalFS.exists(LOG_FILE)) {
    InternalFS.remove(LOG_FILE);
    Serial.println(F("[LOG] Erased."));
  } else {
    Serial.println(F("[LOG] No file to erase."));
  }
}

// =========================================================================
// キャリブレーション（xiao_ble_scan から流用）
// =========================================================================

static void findBestParams(int actual, int& outMinHits, float& outCalib) {
  outMinHits = MIN_HITS; outCalib = CALIBRATION;
  float bestScore = 1e9f;
  for (int minH = 1; minH <= 20; minH++) {
    int q = 0;
    for (int i = 0; i < deviceCount; i++) if (devices[i].count >= minH) q++;
    if (q == 0) break;
    float calib = (float)actual / (float)q;
    float score = fabsf(calib - 1.0f);
    if (score < bestScore) { bestScore = score; outMinHits = minH; outCalib = calib; }
  }
}

static void printCalibResult() {
  if (calibSamples == 0) { Serial.println(F("[CALIB] No data. Send c<N>.")); return; }
  int   minHits = (calibMinHitsSum + calibSamples / 2) / calibSamples;
  float calib   = calibRatioSum / (float)calibSamples;
  Serial.println(F("=== CALIB RESULT ==="));
  Serial.print(F("  Samples : ")); Serial.println(calibSamples);
  Serial.println(F("  --- Paste into your code ---"));
  Serial.print(F("  static int   const MIN_HITS    = ")); Serial.print(minHits);   Serial.println(';');
  Serial.print(F("  static float const CALIBRATION = ")); Serial.print(calib, 2); Serial.println(';');
  Serial.println(F("===================="));
}

static void processCommand(const char* cmd) {
  if (!cmd[0]) return;
  if      (cmd[0]=='d'||cmd[0]=='D') { dumpLog(); }
  else if (cmd[0]=='e'||cmd[0]=='E') { eraseLog(); }
  else if (cmd[0]=='c'||cmd[0]=='C') {
    const char* p = cmd + 1; while (*p==' ') p++;
    int n = atoi(p);
    if (n > 0) {
      calibMode=true; calibActual=n; calibSamples=0; calibRatioSum=0; calibMinHitsSum=0;
      Serial.print(F("[CALIB] actual=")); Serial.print(n);
      Serial.print(F("  collecting ")); Serial.print(CALIB_MIN_SAMPLES); Serial.println(F(" samples..."));
    } else { Serial.println(F("[CALIB] Usage: c<N>  e.g. c5")); }
  }
  else if (cmd[0]=='r'||cmd[0]=='R') { printCalibResult(); }
  else if (cmd[0]=='x'||cmd[0]=='X') { calibMode=false; Serial.println(F("[CALIB] Exited.")); }
  else { Serial.println(F("[CMD] d=dump  e=erase  c<N>=calib  r=result  x=exit")); }
}

static void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c=='\n'||c=='\r') {
      if (cmdLen > 0) { cmdBuf[cmdLen]='\0'; processCommand(cmdBuf); cmdLen=0; }
    } else if (cmdLen < (int)sizeof(cmdBuf)-1) { cmdBuf[cmdLen++]=c; }
  }
}

// =========================================================================
// PIR
// =========================================================================

static void checkPIR() {
  bool cur = (bool)digitalRead(PIR_PIN);
  uint32_t now = millis();
  if (cur && !pirPrevState) {                        // 立ち上がりエッジ
    if (now - pirLastDetect >= PIR_DEBOUNCE_MS) {
      pirLifetime++;
      pirScanCount++;   // スキャン窓カウント（sleep 中も累積するが scan 開始時にリセット）
      pir1sCount++;     // 1 秒表示用
      pirLastDetect = now;
    }
  }
  pirPrevState = cur;
}

// =========================================================================
// PDM コールバック（割り込みコンテキスト）
// =========================================================================

static void onPDMdata() {
  int bytes = PDM.available();
  if (bytes > (int)(PDM_BUF_SAMPLES * sizeof(int16_t)))
    bytes = PDM_BUF_SAMPLES * sizeof(int16_t);
  PDM.read(pdmBuf, bytes);
  pdmReady = bytes / (int)sizeof(int16_t);
}

// =========================================================================
// PDM 処理（main loop から毎フレーム呼ぶ）
// =========================================================================

static void calcBandMag(float outMag[]) {
  const float freqRes = SAMPLE_RATE / (float)FFT_SIZE;
  for (int b = 0; b < NUM_BANDS; b++) {
    int binLo = max(1,               (int)(BAND_LOW_HZ[b]  / freqRes + 0.5f));
    int binHi = min((int)(FFT_SIZE/2),(int)(BAND_HIGH_HZ[b] / freqRes + 0.5f));
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
      int16_t a = s < 0 ? -s : s;
      if (a > rmsAccumPk) rmsAccumPk = a;
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

// =========================================================================
// 1 秒センサティック — 計算・出力・スキャン窓累積・リセット
// =========================================================================

static void sensorTick(bool isScan) {
  // ---- 1 秒 RMS / dBFS ----
  double rms  = (rmsAccumN > 0) ? sqrt(rmsAccumSq / (double)rmsAccumN) : 0.0;
  double dbfs = (rms > 0.0)     ? 20.0 * log10(rms / 32768.0)          : -120.0;

  // ---- 帯域 dBFS ----
  double bDb[NUM_BANDS];
  for (int b = 0; b < NUM_BANDS; b++) {
    double avg = (bandWindows > 0) ? bandAccum[b] / (double)bandWindows : 0.0;
    bDb[b] = (avg > 0.0) ? 20.0 * log10(avg / FFT_FULL_SCALE) : -120.0;
  }

  // ---- シリアル出力 ----
  Serial.print('['); printTimestamp();
  Serial.print(isScan ? F("|SCAN ] ") : F("|SLEEP] "));
  Serial.print(F("PIR="));   Serial.print(pir1sCount);
  Serial.print(F("  rms="));  Serial.print(dbfs, 1); Serial.print(F(" dBFS"));
  Serial.print(F("  peak=")); Serial.print(rmsAccumPk);
  for (int b = 0; b < NUM_BANDS; b++) {
    Serial.print(F("  ")); Serial.print(BAND_NAME[b]);
    Serial.print('=');     Serial.print(bDb[b], 1);
  }
  Serial.println();

  // ---- スキャン窓集計（dBFS を加算平均）----
  if (isScan) {
    scanRmsSum += dbfs;
    for (int b = 0; b < NUM_BANDS; b++) scanBandSum[b] += bDb[b];
    scanSnapCount++;
  }

  // ---- リセット ----
  rmsAccumSq = 0.0; rmsAccumN = 0; rmsAccumPk = 0;
  for (int b = 0; b < NUM_BANDS; b++) bandAccum[b] = 0.0;
  bandWindows = 0;
  pir1sCount  = 0;
}

// =========================================================================
// BLE スキャンコールバック
// =========================================================================

static void scanCallback(ble_gap_evt_adv_report_t* report) {
  updateDevice(report->peer_addr.addr, report->rssi);

  if (OUTPUT_RAW_LOG) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             report->peer_addr.addr[5], report->peer_addr.addr[4],
             report->peer_addr.addr[3], report->peer_addr.addr[2],
             report->peer_addr.addr[1], report->peer_addr.addr[0]);
    Serial.print('['); Serial.print(millis()/1000UL); Serial.print("s] ");
    Serial.print(mac); Serial.print(" RSSI="); Serial.println(report->rssi);
  }

  Bluefruit.Scanner.resume();
}

// =========================================================================
// setup
// =========================================================================

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Serial.println(F("\n=== BLE + PIR + Audio Monitor  /  Toyono Park ==="));
  Serial.println(F("Commands: d=dump  e=erase  c<N>=calib  r=result  x=exit calib"));
  Serial.println(F("Bands: L=125-500Hz  ML=500-2kHz  MH=2k-4kHz  H=4k-8kHz"));
  Serial.println(F("---"));

  // ---- LittleFS ----------------------------------------------------------
  if (ENABLE_LOGGING) {
    InternalFS.begin();
    if (!InternalFS.exists(LOG_FILE)) {
      logFile.open(LOG_FILE, FILE_O_WRITE);
      logFile.println(F("timestamp,people,devices,pir_scan,rms_dbfs,L,ML,MH,H"));
      logFile.close();
      Serial.println(F("[LOG] Created log.csv"));
    } else {
      Serial.println(F("[LOG] Appending to log.csv  (d=dump / e=erase)"));
    }
  }

  // ---- LED ---------------------------------------------------------------
  pinMode(LED_BLUE, OUTPUT);
  digitalWrite(LED_BLUE, HIGH);   // 消灯（アクティブ LOW）

  // ---- PIR ---------------------------------------------------------------
  pinMode(PIR_PIN, INPUT);
  Serial.print(F("[PIR] pin=D")); Serial.print(PIR_PIN);
  Serial.print(F("  debounce=")); Serial.print(PIR_DEBOUNCE_MS); Serial.println(F(" ms"));

  // ---- PDM ---------------------------------------------------------------
  PDM.onReceive(onPDMdata);
  PDM.setGain(PDM_GAIN);
  if (!PDM.begin(1, (int)SAMPLE_RATE)) {
    Serial.println(F("[ERR] PDM.begin() failed — Sense ボードか確認"));
    while (1) delay(500);
  }
  Serial.print(F("[PDM] mono 16kHz  gain=")); Serial.println(PDM_GAIN);

  // ---- BLE ---------------------------------------------------------------
  Bluefruit.begin(1, 0);
  Bluefruit.setName("ToyonoPark");
  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.useActiveScan(false);
  Bluefruit.Scanner.setInterval(
    (uint16_t)(SCAN_INTERVAL_MS * 1000 / 625),
    (uint16_t)(SCAN_WINDOW_MS   * 1000 / 625)
  );
  Serial.println(F("[BLE] Scanner ready"));
  Serial.println(F("---"));

  lastSensorMs = millis();
}

// =========================================================================
// loop — SCAN(30s) → 集計出力 → SLEEP(90s) を繰り返す
// =========================================================================

void loop() {
  // ──────────────────────── SCAN フェーズ ────────────────────────────────
  deviceCount  = 0;
  pirScanCount = 0;
  scanRmsSum   = 0.0;
  scanSnapCount = 0;
  for (int b = 0; b < NUM_BANDS; b++) scanBandSum[b] = 0.0;

  Bluefruit.Scanner.start(0);
  Serial.print(F("\n[SCAN] ")); Serial.print(SCAN_DURATION_MS / 1000); Serial.println(F("s start"));

  uint32_t scanEnd   = millis() + SCAN_DURATION_MS;
  uint32_t lastBlink = millis();
  bool     ledOn     = false;

  while (millis() < scanEnd) {
    handleSerial();
    checkPIR();
    processPDM();

    if (millis() - lastSensorMs >= SENSOR_REPORT_MS) {
      sensorTick(true);
      lastSensorMs += SENSOR_REPORT_MS;
    }
    if (millis() - lastBlink >= LED_BLINK_MS) {
      ledOn = !ledOn;
      digitalWrite(LED_BLUE, ledOn ? LOW : HIGH);
      lastBlink = millis();
    }
    delay(5);
  }

  Bluefruit.Scanner.stop();
  digitalWrite(LED_BLUE, HIGH);

  // ── 人数推定 ──────────────────────────────────────────────────────────
  int    people  = estimatePeople();
  double avgRms  = (scanSnapCount > 0) ? scanRmsSum / (double)scanSnapCount : -120.0;
  double avgBand[NUM_BANDS];
  for (int b = 0; b < NUM_BANDS; b++)
    avgBand[b] = (scanSnapCount > 0) ? scanBandSum[b] / (double)scanSnapCount : -120.0;

  // ── スキャン結果シリアル出力 ──────────────────────────────────────────
  if (OUTPUT_PEOPLE) {
    Serial.println(F("=============================="));
    Serial.print('['); printTimestamp(); Serial.print(F("] "));
    Serial.print(F("People=")); Serial.print(people);
    Serial.print(F(" (devices=")); Serial.print(deviceCount); Serial.print(')');
    Serial.print(F("  pir_scan=")); Serial.print(pirScanCount);
    Serial.print(F("  rms=")); Serial.print(avgRms, 1);
    for (int b = 0; b < NUM_BANDS; b++) {
      Serial.print(F("  ")); Serial.print(BAND_NAME[b]); Serial.print('='); Serial.print(avgBand[b], 1);
    }
    Serial.println();
  }

  // ── フラッシュ記録 ────────────────────────────────────────────────────
  logRecord(people, deviceCount, pirScanCount, avgRms, avgBand);

  // ── キャリブレーション ────────────────────────────────────────────────
  if (calibMode) {
    int   bestMinHits; float bestCalib;
    findBestParams(calibActual, bestMinHits, bestCalib);
    calibMinHitsSum += bestMinHits; calibRatioSum += bestCalib; calibSamples++;
    Serial.print(F("[CALIB] #")); Serial.print(calibSamples);
    Serial.print(F("  actual="));        Serial.print(calibActual);
    Serial.print(F("  MIN_HITS→"));     Serial.print(bestMinHits);
    Serial.print(F("  CALIBRATION→")); Serial.println(bestCalib, 2);
    if (calibSamples >= CALIB_MIN_SAMPLES) { printCalibResult(); calibMode = false; }
  }

  // ──────────────────────── SLEEP フェーズ ───────────────────────────────
  Serial.print(F("[SLEEP] ")); Serial.print(SLEEP_DURATION_MS / 1000); Serial.println(F("s"));
  Serial.flush();

  uint32_t sleepEnd = millis() + SLEEP_DURATION_MS;
  while (millis() < sleepEnd) {
    handleSerial();
    checkPIR();
    processPDM();

    if (millis() - lastSensorMs >= SENSOR_REPORT_MS) {
      sensorTick(false);
      lastSensorMs += SENSOR_REPORT_MS;
    }
    delay(5);
  }
}
