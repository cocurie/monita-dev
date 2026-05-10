/*@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
 * マルチセンサ観測ノード 完全版スケッチ ver1.04
 *
 * 【ver1.04 変更点】
 * ・DS3231温度を氷点下でも正しく読めるよう修正
 * ・温度レジスタを符号付き16bitとして処理
 * ・0.1℃単位でint化
 * ・N回に1回のみ測定
 * ・循環カウンタ方式（0～N-1）
 * ・CH5追加（DS3231温度）
 * ・全体を5CH対応へ拡張
 * ・送信データ10byte化（2byte × 5CH）
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@*/

#define DEBUG_MODE 0 // 0:実運用, 1:デバッグモード

/**************** N回に1回測定設定 ****************/
#define MEASURE_INTERVAL 1
#define BOOT_COUNTER_ADDR 100

/************** CH1〜CH5 センサ割当 *****************
 0:SENSOR_NONE
 1:SENSOR_HX711
 2:SENSOR_MPU6050_PITCH
 3:SENSOR_DS3231_TEMP
********************************************************/
const uint8_t CH_ASSIGN[5] = {
  1, // CH1 ひずみ
  1, // CH2 ひずみ
  1, // CH3 ひずみ
  1, // CH4 ひずみ
  3  // CH5 温度
};

/******************** 測定パラメータ ****************/
#define DATA_NUM    5
#define REPEAT_NUM  5

#define GAUGE_FACTOR_1 1110
#define GAUGE_FACTOR_2 1110
#define GAUGE_FACTOR_3 1110
#define GAUGE_FACTOR_4 1110

#define ERR_A -0.0252f
#define ERR_B  0.53755f

/******************** ピン定義 **********************/
#define BUTTON_PIN 12
#define DONE_PIN   13

#include <Wire.h>
#include <HX711.h>
#include <EEPROM.h>
#include <QuickStats.h>
#include <MPU6050.h>

/******************** enum **************************/
enum SensorType {
  SENSOR_NONE = 0,
  SENSOR_HX711 = 1,
  SENSOR_MPU6050_PITCH = 2,
  SENSOR_DS3231_TEMP = 3
};

/******************** 抽象クラス ********************/
class SensorModule {
public:
  virtual void begin() {}
  virtual int measure() = 0;
  virtual void saveOffset(int addr) {}
  virtual void loadOffset(int addr) {}
};

/******************** DS3231 温度モジュール ************/
class DS3231TempModule : public SensorModule {

public:
  void begin() override {
    Wire.begin();
  }

  int measure() override {

    Wire.beginTransmission(0x68);
    Wire.write(0x11);
    Wire.endTransmission();

    Wire.requestFrom(0x68, 2);

    if (Wire.available() < 2) {
      return 0;
    }

    uint8_t msb = Wire.read();
    uint8_t lsb = Wire.read();

    int16_t rawTemp = ((int16_t)msb << 8) | lsb;

    rawTemp >>= 6;

    float temperature = rawTemp * 0.25;

    int temp_x10 = (int)(temperature * 10.0);

#if DEBUG_MODE
    Serial.print("DS3231 Temp: ");
    Serial.println(temperature);
#endif

    return temp_x10;
  }
};

/******************** HX711 *************************/
class HX711Module : public SensorModule {
  HX711 hx;
  QuickStats stats;
  int factor;
  int offset = 0;

public:
  HX711Module(int dout, int sck, int f) : factor(f) {
    hx.begin(dout, sck);
  }

  void saveOffset(int addr) override {
    offset = hx.read() / factor;
    EEPROM.put(addr, offset);
  }

  void loadOffset(int addr) override {
    EEPROM.get(addr, offset);
  }

  int measure() override {
    int maxv = -999999, minv = 999999;
    float buf[DATA_NUM];

    for (int r = 0; r < REPEAT_NUM; r++) {
      for (int i = 0; i < DATA_NUM; i++)
        buf[i] = (hx.read() / factor) - offset;

      int v = stats.median(buf, DATA_NUM);
      maxv = max(maxv, v);
      minv = min(minv, v);
    }
    return (maxv + minv) / 2;
  }
};

/******************** MPU6050 ***********************/
class MPU6050PitchModule : public SensorModule {
  MPU6050 mpu;

  float correct(float x) {
    return x - (ERR_A * x + ERR_B);
  }

public:
  void begin() override {
    mpu.initialize();
  }

  int measure() override {
    float buf[DATA_NUM];
    QuickStats s;

    for (int i = 0; i < DATA_NUM; i++) {
      int16_t ax, ay, az;
      mpu.getAcceleration(&ax, &ay, &az);
      float axg = ax / 16384.0;
      float ayg = ay / 16384.0;
      float azg = az / 16384.0;
      float pitch = atan2(axg, sqrt(ayg * ayg + azg * azg)) * 180 / PI;
      buf[i] = correct(pitch);
      delay(50);
    }
    return (int)(s.median(buf, DATA_NUM) * 10);
  }
};

/******************** チャンネル *********************/
SensorModule* channels[5] = { nullptr };

void setupChannels() {
  for (int i = 0; i < 5; i++) {

    switch (CH_ASSIGN[i]) {

      case SENSOR_HX711:
        if (i == 0) channels[i] = new HX711Module(A0, A1, GAUGE_FACTOR_1);
        if (i == 1) channels[i] = new HX711Module(A2, A3, GAUGE_FACTOR_2);
        if (i == 2) channels[i] = new HX711Module(2, 3, GAUGE_FACTOR_3);
        if (i == 3) channels[i] = new HX711Module(6, 7, GAUGE_FACTOR_4);
        break;

      case SENSOR_MPU6050_PITCH:
        channels[i] = new MPU6050PitchModule();
        break;

      case SENSOR_DS3231_TEMP:
        channels[i] = new DS3231TempModule();
        break;
    }

    if (channels[i]) channels[i]->begin();
  }
}

/******************** HEX変換 ************************/
String toHex(int v) {
  byte* b = (byte*)&v;
  String s;
  for (int i = 0; i < 2; i++) {
    if (b[i] < 0x10) s += "0";
    s += String(b[i], HEX);
  }
  return s;
}

/******************** SETUP *************************/
void setup() {

  pinMode(DONE_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(9600);
  Wire.begin();

  uint8_t bootCount = 0;
  EEPROM.get(BOOT_COUNTER_ADDR, bootCount);

  bootCount++;
  if (bootCount >= MEASURE_INTERVAL)
    bootCount = 0;

  EEPROM.put(BOOT_COUNTER_ADDR, bootCount);

  bool doMeasure = (bootCount == 0);

  if (!doMeasure) {
    digitalWrite(DONE_PIN, LOW);
    delay(200);
    digitalWrite(DONE_PIN, HIGH);
    delay(200);
    digitalWrite(DONE_PIN, LOW);
    while (true);
  }

  setupChannels();
}

/******************** LOOP **************************/
void loop() {

  int value[5] = {0};

  for (int i = 0; i < 5; i++)
    if (channels[i]) value[i] = channels[i]->measure();

  String msg = "AT$SF=";
  for (int i = 0; i < 5; i++) msg += toHex(value[i]);
  msg += "\r";

#if DEBUG_MODE
  Serial.println(msg);
#else
  Serial.print(msg);
#endif

  delay(8000);

  digitalWrite(DONE_PIN, LOW);
  delay(200);
  digitalWrite(DONE_PIN, HIGH);
  delay(200);
  digitalWrite(DONE_PIN, LOW);

  while (true);
}