#include "LSM6DS3.h"
#include "Wire.h"
#include "arduinoFFT.h"

LSM6DS3 imu(I2C_MODE, 0x6A);

#define SAMPLE_RATE_HZ  200
#define SAMPLE_COUNT    256  // 2のべき乗であること

double vReal[SAMPLE_COUNT];
double vImag[SAMPLE_COUNT];

ArduinoFFT<double> FFT(vReal, vImag, SAMPLE_COUNT, SAMPLE_RATE_HZ);

void setup() {
  Serial.begin(115200);
  while (!Serial);

  if (imu.begin() != 0) {
    Serial.println("IMU init failed");
    while (1);
  }

  Serial.println("IMU OK");
  Serial.println("freq_hz,amplitude_g");
}

void loop() {
  // 加速度データをバッファに収集
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    vReal[i] = imu.readFloatAccelX();
    vImag[i] = 0.0;
    delayMicroseconds(1000000 / SAMPLE_RATE_HZ);
  }

  // FFT処理
  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  // DC成分(i=0)を除き、ナイキスト周波数(SAMPLE_COUNT/2)までを出力
  double freqResolution = (double)SAMPLE_RATE_HZ / SAMPLE_COUNT;
  for (int i = 1; i < SAMPLE_COUNT / 2; i++) {
    double freq = i * freqResolution;
    Serial.print(freq, 2);
    Serial.print(",");
    Serial.println(vReal[i], 6);
  }

  Serial.println("---");
  delay(1000);
}
