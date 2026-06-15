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
 * XIAO nRF52840 Sense — MONITA Phase1 本番コード（省電力版）
 *
 * ・内蔵 LSM6DS3TR-C  (0x6A) → θ1, T1
 * ・外付け LSM6DSO32  (0x6B) → θ2, T2
 * ・DS18B20           (D2)   → T3（筐体壁側温度）
 * ・SHT40             (0x44) → T4（筐体内部温度）
 * ・Sigfox            (Serial1: D6=TX, D7=RX, 9600bps)
 *
 * 計測仕様:
 *   加速度100回平均 → θ1, θ2 算出
 *   20分ごとに計測＋Sigfox送信
 *
 * 省電力対策:
 *   送信後 Sigfox を AT$P=1 でスリープ
 *   次回送信前に AT で起床
 *
 * Sigfoxペイロード 12バイト（little-endian）:
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
OneWire           oneWire(ONE_WIRE_PIN);
DallasTemperature ds18b20(&oneWire);

// =====================================================
// 設定
// =====================================================

const int      READ_N            = 100;
const uint32_t MEASURE_INTERVAL  = 20UL * 60UL * 1000UL;  // 20分（ms）

// =====================================================
// Sigfox
// =====================================================

#define SIGFOX_SERIAL  Serial1
#define SIGFOX_BAUD    9600

bool sigfoxSendAT(const char* cmd, uint32_t timeoutMs = 10000UL)
{
    uint32_t flushEnd = millis() + 500;
    while (millis() < flushEnd) {
        while (SIGFOX_SERIAL.available()) SIGFOX_SERIAL.read();
        delay(10);
    }

    SIGFOX_SERIAL.println(cmd);
    Serial.print("# [SF] >> "); Serial.println(cmd);

    char resp[512] = {0};
    uint16_t idx = 0;
    uint32_t deadline = millis() + timeoutMs;
    uint32_t lastReceiveMs = millis();

    while (millis() < deadline) {
        while (SIGFOX_SERIAL.available() && idx < 511) {
            char c = SIGFOX_SERIAL.read();
            resp[idx++] = c;
            resp[idx]   = '\0';
            lastReceiveMs = millis();
        }

        if (idx > 0 && millis() - lastReceiveMs > 200) {
            Serial.print("# [SF] << "); Serial.println(resp);
            if (strstr(resp, "OK"))  return true;
            if (strstr(resp, "ERR")) return false;
            idx = 0;
            memset(resp, 0, sizeof(resp));
        }
        delay(10);
    }
    Serial.println("# [SF] タイムアウト");
    return false;
}

void sigfoxInit()
{
    sigfoxSendAT("AT", 5000);
    delay(200);
    sigfoxSendAT("AT$RC=3", 5000);
    delay(500);
    sigfoxSendAT("AT$IF=923200000", 5000);
    delay(500);
}

void sigfoxWakeup()
{
    SIGFOX_SERIAL.println("AT");
    delay(2000);
    sigfoxSendAT("AT", 5000);
}

void sigfoxSleep()
{
    // 送信後残データをフラッシュしてからスリープ
    uint32_t t = millis() + 3000UL;
    while (millis() < t) {
        while (SIGFOX_SERIAL.available()) SIGFOX_SERIAL.read();
        delay(50);
    }
    Serial.println("# [SF] フラッシュ完了");
    sigfoxSendAT("AT$P=1", 5000);
}

void sigfoxSend(float theta1, float theta2,
                float T1, float T2, float T3, float T4)
{
    int16_t v[6];
    v[0] = (int16_t)(theta1 * 1000);
    v[1] = (int16_t)(theta2 * 1000);
    v[2] = (int16_t)(T1 * 10);
    v[3] = (int16_t)(T2 * 10);
    v[4] = (int16_t)(T3 * 10);
    v[5] = (int16_t)(T4 * 10);

    // little-endian（LSByte先）
    char cmd[40];
    sprintf(cmd, "AT$SF=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
            (uint8_t)(v[0] & 0xFF), (uint8_t)((uint16_t)v[0] >> 8),
            (uint8_t)(v[1] & 0xFF), (uint8_t)((uint16_t)v[1] >> 8),
            (uint8_t)(v[2] & 0xFF), (uint8_t)((uint16_t)v[2] >> 8),
            (uint8_t)(v[3] & 0xFF), (uint8_t)((uint16_t)v[3] >> 8),
            (uint8_t)(v[4] & 0xFF), (uint8_t)((uint16_t)v[4] >> 8),
            (uint8_t)(v[5] & 0xFF), (uint8_t)((uint16_t)v[5] >> 8));

    sigfoxSendAT(cmd, 60000UL);
}

// =====================================================
// ユーティリティ
// =====================================================

float calcTheta(float ax, float az)
{
    return atan2(ax, az) * 180.0f / PI;
}

float readDSO32Temp()
{
    Wire.beginTransmission(0x6B);
    Wire.write(0x20);
    Wire.endTransmission(false);
    Wire.requestFrom(0x6B, 2);
    if (Wire.available() < 2) return NAN;
    uint8_t l = Wire.read();
    uint8_t h = Wire.read();
    int16_t raw = (int16_t)((h << 8) | l);
    return (raw / 256.0f) + 25.0f;
}

// =====================================================
// 計測（100回平均）
// =====================================================

void measure(float &theta1, float &theta2,
             float &T1, float &T2, float &T3, float &T4)
{
    ds18b20.requestTemperatures();

    float ax1_sum = 0, az1_sum = 0;
    float ax2_sum = 0, az2_sum = 0;

    for (int i = 0; i < READ_N; i++) {
        ax1_sum += imu1.readFloatAccelX();
        az1_sum += imu1.readFloatAccelZ();

        int32_t acc[3], gyr[3];
        imu2.Get_X_Axes(acc);
        imu2.Get_G_Axes(gyr);
        ax2_sum += acc[0] / 1000.0f;
        az2_sum += acc[2] / 1000.0f;

        delay(2);
    }

    theta1 = calcTheta(ax1_sum / READ_N, az1_sum / READ_N);
    theta2 = calcTheta(ax2_sum / READ_N, az2_sum / READ_N);
    T1     = imu1.readTempC();
    T2     = readDSO32Temp();
    T3     = ds18b20.getTempCByIndex(0);

    sensors_event_t humidity, temp;
    sht40.getEvent(&humidity, &temp);
    T4 = temp.temperature;

    Serial.printf("# θ1=%.4f° θ2=%.4f° T1=%.1f T2=%.1f T3=%.1f T4=%.1f\n",
                  theta1, theta2, T1, T2, T3, T4);
}

// =====================================================
// setup
// =====================================================

void setup()
{
    delay(1000);

    Wire.begin();

    // Sigfox初期化
    SIGFOX_SERIAL.begin(SIGFOX_BAUD);
    delay(2000);
    sigfoxInit();

    // センサー初期化
    if (imu1.begin() != 0) {
        Serial.println("# [ERROR] LSM6DS3 初期化失敗");
    } else {
        Serial.println("# [OK] LSM6DS3 初期化完了");
    }

    imu2.begin();
    imu2.Enable_X();
    imu2.Enable_G();
    Serial.println("# [OK] LSM6DSO32 初期化完了");

    sht40.begin();
    sht40.setPrecision(SHT4X_HIGH_PRECISION);
    sht40.setHeater(SHT4X_NO_HEATER);
    Serial.println("# [OK] SHT40 初期化完了");

    ds18b20.begin();
    ds18b20.setWaitForConversion(true);
    Serial.printf("# [OK] DS18B20 検出数: %d\n", ds18b20.getDeviceCount());

    Serial.println("# 計測開始（N=100平均、20分間隔）");
}

// =====================================================
// loop
// =====================================================

void loop()
{
    // Sigfox起床
    sigfoxWakeup();

    // 計測
    float theta1, theta2, T1, T2, T3, T4;
    measure(theta1, theta2, T1, T2, T3, T4);

    // Sigfox送信
    sigfoxSend(theta1, theta2, T1, T2, T3, T4);

    // Sigfoxスリープ
    sigfoxSleep();

    // 20分待機
    delay(MEASURE_INTERVAL);
}
