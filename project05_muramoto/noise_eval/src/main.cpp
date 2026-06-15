#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <LSM6DS3.h>
#include <LSM6DSO32Sensor.h>
#include <math.h>

/*
 * MONITA ノイズフロア評価スケッチ
 *
 * 目的:
 *   平均化サンプル数 N を変えたときの角度測定ノイズ（σ）を計測し、
 *   「これ以上平均化しても改善しなくなる上限（ノイズフロア）」を実測で特定する
 *
 * 評価条件:
 *   N = 10, 100, 1000, 10000（各Nでreps回繰り返してσを算出）
 *
 * センサ:
 *   CH1: LSM6DS3TR-C（XIAO内蔵, 0x6A）
 *   CH2: LSM6DSO32  （外付け,   0x6B）
 *
 * 所要時間（目安）:
 *   N=10    × 30回 ≈   6秒
 *   N=100   × 30回 ≈  60秒
 *   N=1000  × 20回 ≈ 400秒（約7分）
 *   N=10000 × 10回 ≈ 2000秒（約33分）
 *   合計 ≈ 40分程度
 *
 * 使い方:
 *   書き込み後、センサを静置してシリアルモニタ（115200bps）を開く
 *   完了まで放置するだけ（全条件が終わると "=== 完了 ===" が表示される）
 */

// =====================================================
// センサ
// =====================================================

LSM6DS3         imu1(I2C_MODE, 0x6A);
LSM6DSO32Sensor imu2(&Wire, LSM6DSO32_I2C_ADD_H);

// =====================================================
// ユーティリティ
// =====================================================

float calcTheta(float ax, float az)
{
    return atan2(ax, az) * 180.0f / PI;
}

// N サンプルの平均からθ1, θ2を算出（1回分）
void measureOnce(int n, float &theta1, float &theta2)
{
    double ax1_sum = 0, az1_sum = 0;
    double ax2_sum = 0, az2_sum = 0;

    for (int i = 0; i < n; i++) {
        ax1_sum += imu1.readFloatAccelX();
        az1_sum += imu1.readFloatAccelZ();

        int32_t acc[3], gyr[3];
        imu2.Get_X_Axes(acc);
        imu2.Get_G_Axes(gyr);
        ax2_sum += acc[0] / 1000.0;
        az2_sum += acc[2] / 1000.0;

        delay(2);
    }

    theta1 = calcTheta((float)(ax1_sum / n), (float)(az1_sum / n));
    theta2 = calcTheta((float)(ax2_sum / n), (float)(az2_sum / n));
}

// =====================================================
// ノイズ評価（1条件分）
// =====================================================

void evalNoise(int n, int reps)
{
    Serial.printf("\n┌─────────────────────────────────────\n");
    Serial.printf("│ N = %5d  繰り返し = %d 回\n", n, reps);
    Serial.printf("│ （所要時間目安: 約 %d 秒）\n", n * 2 * reps / 1000 + reps);
    Serial.printf("├─────────────────────────────────────\n");

    double sum1 = 0, sum2 = 0;
    double sum1sq = 0, sum2sq = 0;

    for (int r = 0; r < reps; r++) {
        float t1, t2;
        measureOnce(n, t1, t2);

        sum1   += t1;
        sum2   += t2;
        sum1sq += (double)t1 * t1;
        sum2sq += (double)t2 * t2;

        Serial.printf("│  [%2d/%2d]  θ1 = %8.4f°   θ2 = %8.4f°\n",
                      r + 1, reps, t1, t2);
    }

    float mean1  = (float)(sum1  / reps);
    float mean2  = (float)(sum2  / reps);
    float sigma1 = sqrt(fabs((sum1sq / reps) - (double)mean1 * mean1));
    float sigma2 = sqrt(fabs((sum2sq / reps) - (double)mean2 * mean2));

    // 5m壁換算（mm）
    float wall1 = sigma1 * (PI / 180.0f) * 5000.0f;
    float wall2 = sigma2 * (PI / 180.0f) * 5000.0f;

    Serial.printf("├─────────────────────────────────────\n");
    Serial.printf("│  平均  θ1 = %8.4f°   θ2 = %8.4f°\n", mean1, mean2);
    Serial.printf("│  σ     θ1 = %8.5f°   θ2 = %8.5f°\n", sigma1, sigma2);
    Serial.printf("│  5m壁  θ1 = %7.3f mm  θ2 = %7.3f mm\n", wall1, wall2);
    Serial.printf("└─────────────────────────────────────\n");
}

// =====================================================
// setup
// =====================================================

void setup()
{
    delay(1000);
    Serial.begin(115200);
    while (!Serial) delay(10);

    Wire.begin();

    if (imu1.begin() != 0) {
        Serial.println("[ERROR] LSM6DS3（CH1）初期化失敗");
    } else {
        Serial.println("[OK] LSM6DS3（CH1）初期化完了");
    }

    imu2.begin();
    imu2.Enable_X();
    imu2.Enable_G();
    Serial.println("[OK] LSM6DSO32（CH2）初期化完了");

    Serial.println("\n センサ安定化中（5秒）...");
    delay(5000);

    Serial.println("\n================================================");
    Serial.println("  MONITA ノイズフロア評価");
    Serial.println("  CH1: LSM6DS3TR-C（内蔵, 0x6A）");
    Serial.println("  CH2: LSM6DSO32  （外付け, 0x6B）");
    Serial.println("================================================");

    // N=10: 30回繰り返し
    evalNoise(10, 30);

    // N=100: 30回繰り返し
    evalNoise(100, 30);

    // N=1000: 20回繰り返し
    evalNoise(1000, 20);

    // N=10000: 10回繰り返し
    evalNoise(10000, 10);

    // サマリー表示
    Serial.println("\n================================================");
    Serial.println("  完了");
    Serial.println("  σ が N を増やしても改善しなくなった点が");
    Serial.println("  このセンサのノイズフロア（平均化の上限）です");
    Serial.println("================================================");
}

// =====================================================
// loop — 何もしない（setup で完結）
// =====================================================

void loop()
{
    delay(10000);
}
