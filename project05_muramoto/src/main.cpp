#include <Arduino.h>
#include <Wire.h>
#include <LSM6DS3.h>
#include <LSM6DSO32Sensor.h>
#include <math.h>

/*
 * XIAO nRF52840 Sense
 * 2センサー傾斜比較 + 温度確認
 *
 * ・内蔵 LSM6DS3TR-C  (0x6A)
 * ・外付け LSM6DSO32 (0x6B)
 *
 * 処理：
 * ① 1計測で10回読む → 瞬間ノイズ除去
 * ② 直近5回移動平均 → 表示安定化
 *
 * 外付け温度はレジスタ直読み
 */

// =====================================================
// センサー
// =====================================================

// 内蔵IMU
LSM6DS3 imu1(I2C_MODE, 0x6A);

// 外付けIMU
LSM6DSO32Sensor imu2(&Wire, LSM6DSO32_I2C_ADD_H);

// =====================================================
// 設定
// =====================================================

const int SAMPLE_INTERVAL_MS = 1000;

const int READ_N = 10;   // 1回計測の内部平均
const int AVG_N  = 5;    // 移動平均数

// =====================================================
// 移動平均バッファ
// =====================================================

float buf1[AVG_N] = {0};
float buf2[AVG_N] = {0};

int  bufIdx  = 0;
bool bufFull = false;

// =====================================================
// 傾き計算
// =====================================================

float calcTheta(float ax, float az)
{
    return atan2(ax, az) * 180.0 / PI;
}

// =====================================================
// 移動平均
// =====================================================

float movingAvg(float* buf)
{
    int count = bufFull ? AVG_N : bufIdx;

    if (count == 0) return 0;

    float sum = 0;

    for (int i = 0; i < count; i++) {
        sum += buf[i];
    }

    return sum / count;
}

// =====================================================
// LSM6DSO32 温度レジスタ直読み
// =====================================================

float readDSO32Temp()
{
    // 温度レジスタ
    const uint8_t TEMP_L = 0x20;
    const uint8_t TEMP_H = 0x21;

    Wire.beginTransmission(0x6B);
    Wire.write(TEMP_L);
    Wire.endTransmission(false);

    Wire.requestFrom(0x6B, 2);

    if (Wire.available() < 2) {
        return NAN;
    }

    uint8_t l = Wire.read();
    uint8_t h = Wire.read();

    int16_t raw = (int16_t)((h << 8) | l);

    // datasheet:
    // Temp = (raw / 256) + 25
    return (raw / 256.0) + 25.0;
}

// =====================================================
// setup
// =====================================================

void setup()
{
    Serial.begin(115200);

    while (!Serial) {
        delay(10);
    }

    Wire.begin();

    Serial.println();
    Serial.println("# ==============================");
    Serial.println("# 2センサー同時精度検証");
    Serial.println("# ==============================");

    // 内蔵IMU
    if (imu1.begin() != 0) {

        Serial.println("# [ERROR] 内蔵LSM6DS3 初期化失敗");

    } else {

        Serial.println("# [OK] 内蔵LSM6DS3 初期化成功");
    }

    // 外付けIMU
    imu2.begin();
    imu2.Enable_X();
    imu2.Enable_G();

    Serial.println("# [OK] 外付けLSM6DSO32 初期化");

    Serial.println("# ------------------------------");

    // CSVヘッダ
    Serial.println(
        "time_ms,"
        "theta1_raw,"
        "theta2_raw,"
        "theta1_avg,"
        "theta2_avg,"
        "T1_C,"
        "T2_C"
    );
}

// =====================================================
// loop
// =====================================================

void loop()
{
    unsigned long t = millis();

    // ---------------------------------------------
    // 10回平均用
    // ---------------------------------------------

    float ax1_sum = 0;
    float az1_sum = 0;

    float ax2_sum = 0;
    float az2_sum = 0;

    // ---------------------------------------------
    // 1回計測内で10回読む
    // ---------------------------------------------

    for (int i = 0; i < READ_N; i++) {

        // 内蔵
        ax1_sum += imu1.readFloatAccelX();
        az1_sum += imu1.readFloatAccelZ();

        // 外付け
        int32_t accel[3];
        int32_t gyro[3];

        imu2.Get_X_Axes(accel);
        imu2.Get_G_Axes(gyro);

        ax2_sum += accel[0] / 1000.0;
        az2_sum += accel[2] / 1000.0;

        delay(2);
    }

    // ---------------------------------------------
    // 10回平均
    // ---------------------------------------------

    float ax1 = ax1_sum / READ_N;
    float az1 = az1_sum / READ_N;

    float ax2 = ax2_sum / READ_N;
    float az2 = az2_sum / READ_N;

    // ---------------------------------------------
    // 傾き計算
    // ---------------------------------------------

    float theta1 = calcTheta(ax1, az1);
    float theta2 = calcTheta(ax2, az2);

    // ---------------------------------------------
    // 温度
    // ---------------------------------------------

    float T1 = imu1.readTempC();
    float T2 = readDSO32Temp();

    // ---------------------------------------------
    // 移動平均
    // ---------------------------------------------

    buf1[bufIdx] = theta1;
    buf2[bufIdx] = theta2;

    bufIdx++;

    if (bufIdx >= AVG_N) {

        bufIdx = 0;
        bufFull = true;
    }

    float avg1 = movingAvg(buf1);
    float avg2 = movingAvg(buf2);

    // ---------------------------------------------
    // CSV出力
    // ---------------------------------------------

    Serial.print(t);
    Serial.print(",");

    Serial.print(theta1, 4);
    Serial.print(",");

    Serial.print(theta2, 4);
    Serial.print(",");

    Serial.print(avg1, 4);
    Serial.print(",");

    Serial.print(avg2, 4);
    Serial.print(",");

    Serial.print(T1, 2);
    Serial.print(",");

    Serial.println(T2, 2);

    delay(SAMPLE_INTERVAL_MS);
}