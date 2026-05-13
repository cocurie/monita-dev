#include "LSM6DS3.h"
#include "Wire.h"
#include "arduinoFFT.h"

// ── 出力設定 ──────────────────────────────────────────────────────────────────
#define OUTPUT_WAVEFORM   true   // 生波形を出力するか
#define OUTPUT_FFT        true   // FFT結果を出力するか

// ── 軸選択 ────────────────────────────────────────────────────────────────────
#define AXIS_X  true
#define AXIS_Y  false
#define AXIS_Z  false

// ── FFT周波数レンジ ────────────────────────────────────────────────────────────
#define FFT_FREQ_MAX_HZ  20.0   // この周波数以下のみ出力（ナイキスト100Hzまで有効）

// ── サンプリング設定 ──────────────────────────────────────────────────────────
#define SAMPLE_RATE_HZ  200
#define SAMPLE_COUNT    256     // 2のべき乗であること

// ─────────────────────────────────────────────────────────────────────────────

LSM6DS3 imu(I2C_MODE, 0x6A);

#if AXIS_X
double vRealX[SAMPLE_COUNT];
double vImagX[SAMPLE_COUNT];
ArduinoFFT<double> fftX(vRealX, vImagX, SAMPLE_COUNT, SAMPLE_RATE_HZ);
#endif
#if AXIS_Y
double vRealY[SAMPLE_COUNT];
double vImagY[SAMPLE_COUNT];
ArduinoFFT<double> fftY(vRealY, vImagY, SAMPLE_COUNT, SAMPLE_RATE_HZ);
#endif
#if AXIS_Z
double vRealZ[SAMPLE_COUNT];
double vImagZ[SAMPLE_COUNT];
ArduinoFFT<double> fftZ(vRealZ, vImagZ, SAMPLE_COUNT, SAMPLE_RATE_HZ);
#endif

void setup() {
  Serial.begin(115200);
  while (!Serial);

  if (imu.begin() != 0) {
    Serial.println("IMU init failed");
    while (1);
  }

  Serial.println("IMU OK");
}

void loop() {
  // 加速度データをバッファに収集
  for (int i = 0; i < SAMPLE_COUNT; i++) {
#if AXIS_X
    vRealX[i] = imu.readFloatAccelX();
    vImagX[i] = 0.0;
#endif
#if AXIS_Y
    vRealY[i] = imu.readFloatAccelY();
    vImagY[i] = 0.0;
#endif
#if AXIS_Z
    vRealZ[i] = imu.readFloatAccelZ();
    vImagZ[i] = 0.0;
#endif
    delayMicroseconds(1000000 / SAMPLE_RATE_HZ);
  }

  // 生波形出力
#if OUTPUT_WAVEFORM
  Serial.println("[WAVE]");
#if AXIS_X
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    Serial.print("X,"); Serial.println(vRealX[i], 4);
  }
#endif
#if AXIS_Y
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    Serial.print("Y,"); Serial.println(vRealY[i], 4);
  }
#endif
#if AXIS_Z
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    Serial.print("Z,"); Serial.println(vRealZ[i], 4);
  }
#endif
#endif // OUTPUT_WAVEFORM

  // FFT処理・出力
#if OUTPUT_FFT
  const double freqRes = (double)SAMPLE_RATE_HZ / SAMPLE_COUNT;

  Serial.println("[FFT]");

#if AXIS_X
  fftX.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  fftX.compute(FFTDirection::Forward);
  fftX.complexToMagnitude();
  Serial.println("X_freq_hz,X_amp_g");
  for (int i = 1; i < SAMPLE_COUNT / 2; i++) {
    double freq = i * freqRes;
    if (freq > FFT_FREQ_MAX_HZ) break;
    Serial.print(freq, 2); Serial.print(","); Serial.println(vRealX[i], 6);
  }
#endif

#if AXIS_Y
  fftY.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  fftY.compute(FFTDirection::Forward);
  fftY.complexToMagnitude();
  Serial.println("Y_freq_hz,Y_amp_g");
  for (int i = 1; i < SAMPLE_COUNT / 2; i++) {
    double freq = i * freqRes;
    if (freq > FFT_FREQ_MAX_HZ) break;
    Serial.print(freq, 2); Serial.print(","); Serial.println(vRealY[i], 6);
  }
#endif

#if AXIS_Z
  fftZ.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  fftZ.compute(FFTDirection::Forward);
  fftZ.complexToMagnitude();
  Serial.println("Z_freq_hz,Z_amp_g");
  for (int i = 1; i < SAMPLE_COUNT / 2; i++) {
    double freq = i * freqRes;
    if (freq > FFT_FREQ_MAX_HZ) break;
    Serial.print(freq, 2); Serial.print(","); Serial.println(vRealZ[i], 6);
  }
#endif

  Serial.println("---");
#endif // OUTPUT_FFT

  delay(1000);
}
