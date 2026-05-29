/**
 * PDM マイク 音量モニタ（RMS + FFT 帯域） — Seeed XIAO nRF52840 Sense
 *
 * 動作概要:
 *   オンボード PDM マイク（MSM261D3526H1CPM）から 16kHz mono で PCM サンプルを取得し、
 *   1 秒ごとに以下をシリアル出力する。
 *     - RMS 振幅・dBFS・ピーク値
 *     - 4 帯域の FFT 帯域 dBFS（L / ML / MH / H）
 *
 * FFT 設定:
 *   FFT_SIZE = 256 → 周波数分解能 = 16000 / 256 = 62.5 Hz/bin
 *   ウィンドウ関数: Hann
 *   1 秒間に得られる FFT ウィンドウ数 ≈ 62 枚（非オーバーラップ）
 *   各帯域 magnitude を全ウィンドウで線形平均 → dBFS 変換して出力
 *
 * 帯域定義:
 *   L  :  125 –  500 Hz（低域：人の動き・低音）
 *   ML :  500 – 2000 Hz（中低域：声・環境音主成分）
 *   MH : 2000 – 4000 Hz（中高域：声の明瞭度帯）
 *   H  : 4000 – 8000 Hz（高域：高調波・ambient）
 *
 * 出力形式:
 *   <ms>  rms=<val>  dBFS=<val>  peak=<val>  L=<dBFS>  ML=<dBFS>  MH=<dBFS>  H=<dBFS>
 *
 * 用途:
 *   豊能町 公園人流測定デバイスへの音センシング追加前の単体動作確認。
 *   BLE・PIR との統合は 正本ファーム（案件/project04_toyono/firmware/）で行う。
 *
 * 依存ライブラリ:
 *   arduinoFFT v2（platformio.ini: lib_deps = kosme/arduinoFFT）
 */

#include <Arduino.h>
#include <PDM.h>
#include <arduinoFFT.h>

// =========================================================================
// 設定
// =========================================================================

/** 集計・出力間隔（ミリ秒） */
static const uint32_t REPORT_INTERVAL_MS = 1000;

/**
 * PDM マイクゲイン（0〜80, デフォルト 20）
 * 静かな環境: 40〜60、屋外通常: 20〜40 あたりから試す。
 * クリッピング（peak=32767 が続く）なら下げること。
 */
static const uint8_t PDM_GAIN = 30;

/** FFT 点数（2 のべき乗） */
static const uint16_t FFT_SIZE = 256;

/** PDM サンプルレート [Hz] */
static const float SAMPLE_RATE = 16000.0f;

/**
 * FFT dBFS のフルスケール基準。
 * Hann 窓後の bin magnitude は入力振幅 A に対し約 A * FFT_SIZE/4 になるため、
 * フルスケール（A = 32768）基準として FFT_SIZE/4 * 32768 を使う。
 * 帯域間・時系列の相対比較に使う値なので、絶対 dB SPL への換算は現地キャリブ後に行う。
 */
static const double FFT_FULL_SCALE = (FFT_SIZE / 4.0) * 32768.0;

// ---- 帯域定義 [Hz] -------------------------------------------------------
static const int   NUM_BANDS      = 4;
static const float BAND_LOW_HZ [] = { 125.0f,  500.0f, 2000.0f, 4000.0f };
static const float BAND_HIGH_HZ[] = { 500.0f, 2000.0f, 4000.0f, 8000.0f };
static const char* BAND_NAME[]    = { "L", "ML", "MH", "H" };

// =========================================================================
// バッファ・グローバル変数
// =========================================================================

// ---- PDM コールバック用受信バッファ ----------------------------------------
static const int PDM_BUF_SAMPLES = 512;
static int16_t  pdmBuf[PDM_BUF_SAMPLES];
static volatile int pdmReady = 0;   // コールバックが書いたサンプル数

// ---- FFT 用リングバッファ（main loop のみが読み書き） ----------------------
// サイズを FFT_SIZE * 4 にして余裕をもたせる（マスクで高速剰余）
static const uint16_t RING_SIZE = FFT_SIZE * 4;  // 1024、2 のべき乗
static const uint16_t RING_MASK = RING_SIZE - 1;
static int16_t  ringBuf[RING_SIZE];
static uint16_t ringHead = 0;   // 書き込み位置
static uint16_t ringTail = 0;   // 読み出し位置

// ---- FFT 用作業バッファ（float） -----------------------------------------
static float vReal[FFT_SIZE];
static float vImag[FFT_SIZE];

ArduinoFFT<float> FFT(vReal, vImag, FFT_SIZE, SAMPLE_RATE);

// ---- 集計バッファ --------------------------------------------------------
// RMS
static double   rmsAccumSq  = 0.0;
static uint32_t rmsAccumN   = 0;
static int16_t  rmsAccumPk  = 0;
// FFT 帯域（1 秒間の全 FFT ウィンドウで線形平均）
static double   bandAccum[NUM_BANDS] = {};
static uint32_t bandWindows = 0;

static uint32_t lastReportMs = 0;

// =========================================================================
// ヘルパー
// =========================================================================

/**
 * vReal（complexToMagnitude 後）から各帯域の bin RMS magnitude を計算する。
 * フルスケール基準は FFT_FULL_SCALE（グローバル定数）。
 */
static void calcBandMag(float outMag[NUM_BANDS]) {
  const float freqRes = SAMPLE_RATE / (float)FFT_SIZE;
  for (int b = 0; b < NUM_BANDS; b++) {
    int binLo = max(1, (int)(BAND_LOW_HZ[b]  / freqRes + 0.5f));
    int binHi = min((int)(FFT_SIZE / 2),
                    (int)(BAND_HIGH_HZ[b] / freqRes + 0.5f));
    double sumSq = 0.0;
    int    cnt   = 0;
    for (int i = binLo; i < binHi; i++) {
      sumSq += (double)vReal[i] * (double)vReal[i];
      cnt++;
    }
    outMag[b] = (cnt > 0) ? (float)sqrt(sumSq / cnt) : 0.0f;
  }
}

// =========================================================================
// PDM コールバック（割り込みコンテキスト — 最小限の処理のみ）
// =========================================================================

void onPDMdata() {
  int bytes = PDM.available();
  if (bytes > (int)(PDM_BUF_SAMPLES * sizeof(int16_t)))
    bytes = PDM_BUF_SAMPLES * sizeof(int16_t);
  PDM.read(pdmBuf, bytes);
  pdmReady = bytes / (int)sizeof(int16_t);
}

// =========================================================================
// setup
// =========================================================================

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 3000) delay(10);

  Serial.println(F("=== XIAO nRF52840 Sense — PDM RMS + FFT monitor ==="));
  Serial.print  (F("gain="));        Serial.print(PDM_GAIN);
  Serial.print  (F("  FFT_SIZE="));  Serial.print(FFT_SIZE);
  Serial.print  (F("  freq_res="));  Serial.print(SAMPLE_RATE / FFT_SIZE, 1);
  Serial.println(F(" Hz/bin  window=Hann"));
  Serial.println(F("bands:  L=125-500Hz  ML=500-2kHz  MH=2k-4kHz  H=4k-8kHz"));
  Serial.println(F("fmt: ms  rms=  dBFS=  peak=  L=  ML=  MH=  H=  (band unit: dBFS)"));
  Serial.println();

  PDM.onReceive(onPDMdata);
  PDM.setGain(PDM_GAIN);
  if (!PDM.begin(1, (int)SAMPLE_RATE)) {
    Serial.println(F("[ERR] PDM.begin() failed — Sense ボードか確認"));
    while (1) delay(500);
  }
  Serial.println(F("[OK] PDM started: mono 16 kHz"));
  lastReportMs = millis();
}

// =========================================================================
// loop
// =========================================================================

void loop() {
  // -----------------------------------------------------------------------
  // 1. PDM コールバックで受け取ったサンプルを処理
  // -----------------------------------------------------------------------
  if (pdmReady > 0) {
    int n = pdmReady;
    pdmReady = 0;  // フラグクリア（コールバック再入前に）

    for (int i = 0; i < n; i++) {
      int16_t s = pdmBuf[i];

      // リングバッファへ書き込み（オーバーフロー時は古いデータを上書き）
      ringBuf[ringHead & RING_MASK] = s;
      ringHead++;

      // RMS 累積
      rmsAccumSq += (double)s * (double)s;
      int16_t a = s < 0 ? -s : s;
      if (a > rmsAccumPk) rmsAccumPk = a;
    }
    rmsAccumN += (uint32_t)n;
  }

  // -----------------------------------------------------------------------
  // 2. リングバッファに FFT_SIZE サンプル溜まったら FFT 処理
  // -----------------------------------------------------------------------
  while ((uint16_t)(ringHead - ringTail) >= FFT_SIZE) {
    // float バッファへコピー（Hann 窓は FFT.windowing() が適用）
    for (int i = 0; i < FFT_SIZE; i++) {
      vReal[i] = (float)ringBuf[(ringTail + i) & RING_MASK];
      vImag[i] = 0.0f;
    }
    ringTail += FFT_SIZE;  // 非オーバーラップ

    FFT.windowing(FFTWindow::Hann, FFTDirection::Forward);
    FFT.compute(FFTDirection::Forward);
    FFT.complexToMagnitude();  // vReal[i] = magnitude

    // 帯域 magnitude を累積
    float bandMag[NUM_BANDS];
    calcBandMag(bandMag);
    for (int b = 0; b < NUM_BANDS; b++) {
      bandAccum[b] += (double)bandMag[b];
    }
    bandWindows++;
  }

  // -----------------------------------------------------------------------
  // 3. 1 秒ごとに出力
  // -----------------------------------------------------------------------
  uint32_t now = millis();
  if ((now - lastReportMs) >= REPORT_INTERVAL_MS && rmsAccumN > 0) {
    // RMS・dBFS
    double rms  = sqrt(rmsAccumSq / (double)rmsAccumN);
    double dbfs = (rms > 0.0) ? 20.0 * log10(rms / 32768.0) : -120.0;

    Serial.print(now);
    Serial.print(F("  rms="));  Serial.print(rms,  1);
    Serial.print(F("  dBFS=")); Serial.print(dbfs, 1);
    Serial.print(F("  peak=")); Serial.print(rmsAccumPk);

    // 帯域 dBFS（線形平均 magnitude → dBFS 変換）
    for (int b = 0; b < NUM_BANDS; b++) {
      double avgMag = (bandWindows > 0)
                    ? bandAccum[b] / (double)bandWindows
                    : 0.0;
      double bDb = (avgMag > 0.0)
                 ? 20.0 * log10(avgMag / FFT_FULL_SCALE)
                 : -120.0;
      Serial.print(F("  "));
      Serial.print(BAND_NAME[b]);
      Serial.print(F("="));
      Serial.print(bDb, 1);
    }
    Serial.println();

    // リセット
    rmsAccumSq = 0.0;  rmsAccumN = 0;  rmsAccumPk = 0;
    for (int b = 0; b < NUM_BANDS; b++) bandAccum[b] = 0.0;
    bandWindows  = 0;
    lastReportMs = now;
  }
}
