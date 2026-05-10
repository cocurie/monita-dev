#include "LSM6DS3.h"
#include "Wire.h"

// I2CでIMU初期化
LSM6DS3 imu(I2C_MODE, 0x6A);

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
  float ax = imu.readFloatAccelX();
  float ay = imu.readFloatAccelY();
  float az = imu.readFloatAccelZ();

  Serial.print("ax: ");
  Serial.print(ax, 3);
  Serial.print("  ay: ");
  Serial.print(ay, 3);
  Serial.print("  az: ");
  Serial.println(az, 3);

  delay(3000);
}