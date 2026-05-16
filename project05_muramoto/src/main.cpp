#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <LSM6DS3.h>
#include <LSM6DSO32Sensor.h>
#include <Adafruit_SHT4x.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>

/*
 * XIAO nRF52840 Sense
 * 2センサー傾斜比較 + 4温度計測 + Sigfox送信（1分間隔）
 *
 * ・内蔵 LSM6DS3TR-C  (0x6A) → θ1, T1
 * ・外付け LSM6DSO32  (0x6B) → θ2, T2
 * ・DS18B20           (D2)   → T3（筐体内面/壁側温度）
 * ・SHT40             (0x44) → T4（筐体内部空気温度）
 * ・Sigfox            (Serial1: D6=TX, D7=RX, 9600bps)
 *
 * Sigfoxペイロード 12バイト:
 *   θ1(int16,0.001°) θ2(int16,0.001°)
 *   T1(int16,0.1°C)  T2(int16,0.1°C)
 *   T3(int16,0.1°C)  T4(int16,0.1°C)
 */

// =====================================================
// センサー
// =====================================================

LSM6DS3         imu1(I2C_MODE, 0x6A);
LSM6DSO32Sensor imu2(&Wire, LSM6DSO32_I2C_ADD_H);
Adafruit_SHT4x  sht40;

#define ONE_WIRE_PIN D2
OneWire          oneWire(ONE_WIRE_PIN);
DallasTemperature ds18b20(&oneWire);

// =====================================================
// Sigfox
// =====================================================

#define SIGFOX_SERIAL  Serial1
#define SIGFOX_BAUD    9600
#define SIGFOX_INTERVAL_MS  60000UL   // 1分

unsigned long lastSigfoxMs = 0;

// Sigfox ATコマンド送信（タイムアウト最大30秒）
bool sigfoxSendAT(const char* cmd)
{
    // 受信バッファクリア
    while (SIGFOX_SERIAL.available()) SIGFOX_SERIAL.read();

    SIGFOX_SERIAL.println(cmd);
    Serial.print("# [SF] >> "); Serial.println(cmd);

    uint32_t deadline = millis() + 30000UL;
    String resp = "";
    while (millis() < deadline) {
        while (SIGFOX_SERIAL.available()) {
            char c = SIGFOX_SERIAL.read();
            resp += c;
        }
        if (resp.indexOf("OK") >= 0) {
            Serial.println("# [SF] OK");
            return true;
        }
        if (resp.indexOf("ERR") >= 0) {
            Serial.print("# [SF] ERR: "); Serial.println(resp);
            return false;
        }
        delay(10);
    }
    Serial.println("# [SF] タイムアウト");
    return false;
}

// 計測値をSigfoxで送信
void sigfoxSend(float theta1, float theta2,
                float T1, float T2, float T3, float T4)
{
    // int16に変換（big-endian）
    int16_t v[6];
    v[0] = (int16_t)(theta1 * 1000);
    v[1] = (int16_t)(theta2 * 1000);
    v[2] = (int16_t)(T1 * 10);
    v[3] = (int16_t)(T2 * 10);
    v[4] = (int16_t)(T3 * 10);
    v[5] = (int16_t)(T4 * 10);

    char cmd[40];
    sprintf(cmd, "AT$SF=%04X%04X%04X%04X%04X%04X",
            (uint16_t)v[0], (uint16_t)v[1],
            (uint16_t)v[2], (uint16_t)v[3],
            (uint16_t)v[4], (uint16_t)v[5]);

    sigfoxSendAT(cmd);
}

// =====================================================
// 設定
// =====================================================

const int SAMPLE_INTERVAL_MS = 1000;
const int READ_N             = 10;

// =====================================================
// 傾き計算
// =====================================================

float calcTheta(float ax, float az)
{
    return atan2(ax, az) * 180.0 / PI;
}

// =====================================================
// LSM6DSO32 温度レジスタ直読み
// =====================================================

float readDSO32Temp()
{
    const uint8_t TEMP_L = 0x20;
    const uint8_t TEMP_H = 0x21;

    Wire.beginTransmission(0x6B);
    Wire.write(TEMP_L);
    Wire.endTransmission(false);
    Wire.requestFrom(0x6B, 2);

    if (Wire.available() < 2) return NAN;

    uint8_t l = Wire.read();
    uint8_t h = Wire.read();
    int16_t raw = (int16_t)((h << 8) | l);

    return (raw / 256.0) + 25.0;
}

// =====================================================
// setup
// =====================================================

void setup()
{
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 5000) { delay(10); }

    Wire.begin();

    // Sigfox初期化
    SIGFOX_SERIAL.begin(SIGFOX_BAUD);
    delay(500);
    sigfoxSendAT("AT");   // 疎通確認

    Serial.println();
    Serial.println("# ==============================");
    Serial.println("# 2センサー傾斜比較 + 4温度 + Sigfox");
    Serial.println("# ==============================");

    if (imu1.begin() != 0) {
        Serial.println("# [ERROR] 内蔵LSM6DS3 初期化失敗");
    } else {
        Serial.println("# [OK] 内蔵LSM6DS3 初期化成功");
    }

    imu2.begin();
    imu2.Enable_X();
    imu2.Enable_G();
    Serial.println("# [OK] 外付けLSM6DSO32 初期化");

    if (!sht40.begin()) {
        Serial.println("# [ERROR] SHT40 初期化失敗（0x44）");
    } else {
        sht40.setPrecision(SHT4X_HIGH_PRECISION);
        sht40.setHeater(SHT4X_NO_HEATER);
        Serial.println("# [OK] SHT40 初期化成功");
    }

    ds18b20.begin();
    ds18b20.setWaitForConversion(true);
    Serial.print("# [OK] DS18B20 検出数: ");
    Serial.println(ds18b20.getDeviceCount());

    Serial.println("# Sigfox送信間隔: 1分");
    Serial.println("# ------------------------------");
    Serial.println(
        "time_ms,"
        "theta1_raw,theta2_raw,"
        "T1_C,T2_C,T3_C,T4_C"
    );

    lastSigfoxMs = millis();
}

// =====================================================
// loop
// =====================================================

void loop()
{
    unsigned long t = millis();

    // DS18B20 変換（ブロッキング約750ms）
    ds18b20.requestTemperatures();

    // 10回平均
    float ax1_sum = 0, az1_sum = 0;
    float ax2_sum = 0, az2_sum = 0;

    for (int i = 0; i < READ_N; i++) {
        ax1_sum += imu1.readFloatAccelX();
        az1_sum += imu1.readFloatAccelZ();

        int32_t accel[3], gyro[3];
        imu2.Get_X_Axes(accel);
        imu2.Get_G_Axes(gyro);

        ax2_sum += accel[0] / 1000.0;
        az2_sum += accel[2] / 1000.0;

        delay(2);
    }

    float theta1 = calcTheta(ax1_sum / READ_N, az1_sum / READ_N);
    float theta2 = calcTheta(ax2_sum / READ_N, az2_sum / READ_N);

    float T1 = imu1.readTempC();
    float T2 = readDSO32Temp();
    float T3 = ds18b20.getTempCByIndex(0);

    sensors_event_t humidity, temp;
    sht40.getEvent(&humidity, &temp);
    float T4 = temp.temperature;

    // CSV出力（毎秒）
    Serial.print(t);          Serial.print(",");
    Serial.print(theta1, 4);  Serial.print(",");
    Serial.print(theta2, 4);  Serial.print(",");
    Serial.print(T1,     2);  Serial.print(",");
    Serial.print(T2,     2);  Serial.print(",");
    Serial.print(T3,     2);  Serial.print(",");
    Serial.println(T4,   2);

    // Sigfox送信（1分ごと）
    if (millis() - lastSigfoxMs >= SIGFOX_INTERVAL_MS) {
        lastSigfoxMs = millis();
        sigfoxSend(theta1, theta2, T1, T2, T3, T4);
    }

    delay(SAMPLE_INTERVAL_MS);
}
