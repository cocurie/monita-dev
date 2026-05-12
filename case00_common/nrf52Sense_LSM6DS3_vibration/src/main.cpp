#include "LSM6DS3.h"
#include "Wire.h"

// I2CでIMU初期化
LSM6DS3 imu(I2C_MODE, 0x6A);

// TODO: サンプリングレート・バッファサイズを振動解析用に調整する
#define SAMPLE_RATE_HZ  200
#define SAMPLE_COUNT    256

float axBuf[SAMPLE_COUNT];

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
    axBuf[i] = imu.readFloatAccelX();
    delayMicroseconds(1000000 / SAMPLE_RATE_HZ);
  }

  // TODO: FFT処理を実装する（例: ArduinoFFT ライブラリを使用）
  // 暫定: 生波形をシリアル出力
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    Serial.println(axBuf[i], 4);
  }

  delay(1000);
}
