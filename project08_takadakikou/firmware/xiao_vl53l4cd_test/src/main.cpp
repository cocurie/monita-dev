// XIAO + Adafruit VL53L4CX ブレッドボード動作確認テスト
// シリアルモニタに 距離(mm) と レンジステータス を出力する。
// 参考: stm32duino/VL53L4CX 公式サンプル(VL53L4CX_Sat_HelloWorld)をXIAO向けに簡略化

#include <Arduino.h>
#include <Wire.h>
#include <vl53l4cx_class.h>

#define DEV_I2C Wire
#define XSHUT_PIN D6

VL53L4CX tofSensor(&DEV_I2C, XSHUT_PIN);

const char *rangeStatusToText(uint8_t status) {
  switch (status) {
    case 0: return "OK";
    case 1: return "SIGMA_FAIL";      // ばらつき大（信頼度低）
    case 2: return "SIGNAL_FAIL";     // 反射光弱い（対象物・距離・カバー要確認）
    case 4: return "OUT_OF_BOUNDS";   // 測定レンジ外
    case 7: return "WRAP_TARGET_FAIL";
    default: return "UNKNOWN";
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 3000) {
    // USBシリアル接続待ち（最大3秒）。接続なしでも先へ進む。
  }

  DEV_I2C.begin();

  Serial.println();
  Serial.println("=== XIAO + VL53L4CX 動作確認テスト ===");

  tofSensor.begin();
  tofSensor.VL53L4CX_Off();

  int status = tofSensor.InitSensor(0x12);
  if (status != 0) {
    Serial.print("[ERROR] VL53L4CX 初期化失敗 (status=");
    Serial.print(status);
    Serial.println(")。配線(SDA/SCL/XSHUT/3V3/GND)を確認してください。");
    while (1) {
      delay(10000);
    }
  }
  Serial.println("[OK] VL53L4CX 初期化成功");

  tofSensor.VL53L4CX_StartMeasurement();
}

void loop() {
  uint8_t dataReady = 0;
  int status = tofSensor.VL53L4CX_GetMeasurementDataReady(&dataReady);

  if (status == 0 && dataReady != 0) {
    VL53L4CX_MultiRangingData_t rangingData;
    status = tofSensor.VL53L4CX_GetMultiRangingData(&rangingData);

    if (status == 0 && rangingData.NumberOfObjectsFound > 0) {
      uint8_t rangeStatus = rangingData.RangeData[0].RangeStatus;
      int distanceMm = rangingData.RangeData[0].RangeMilliMeter;

      Serial.print("distance_mm=");
      Serial.print(distanceMm);
      Serial.print("  status=");
      Serial.print(rangeStatus);
      Serial.print(" (");
      Serial.print(rangeStatusToText(rangeStatus));
      Serial.println(")");
    } else {
      Serial.println("no object found");
    }

    tofSensor.VL53L4CX_ClearInterruptAndStartMeasurement();
  }

  delay(200);
}
