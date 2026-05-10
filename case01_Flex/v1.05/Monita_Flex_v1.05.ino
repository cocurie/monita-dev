
/*@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
* マルチセンサ観測ノード 完全版スケッチ ver1.05
*
* 【ver1.05 変更点】
* ・SENSOR_HX711_RANGE追加（センサ割当の選択肢に追加）

* ・RANGE_SOURCE_CH定義でRangeのソースCHを指定

* ・CH6追加（指定CHのひずみRange）

* ・6CH対応・送信データ12byte化（2byte × 6CH）
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@*/



#define DEBUG_MODE 0



#define MEASURE_INTERVAL 1
#define BOOT_COUNTER_ADDR 100



#define RANGE_SOURCE_CH 1



const uint8_t CH_ASSIGN[6] = {
  1, // CH1
  1, // CH2
  1, // CH3
  1, // CH4
  3, // CH5
  4  // CH6 (Range)
};



#define DATA_NUM    5
#define REPEAT_NUM  5



#define GAUGE_FACTOR_1 1110
#define GAUGE_FACTOR_2 1110
#define GAUGE_FACTOR_3 1110
#define GAUGE_FACTOR_4 1110



#define ERR_A -0.0252f
#define ERR_B  0.53755f



#define BUTTON_PIN 12
#define DONE_PIN   13



#include <Wire.h>
#include <HX711.h>
#include <EEPROM.h>
#include <QuickStats.h>
#include <MPU6050.h>



enum SensorType {
  SENSOR_NONE = 0,
  SENSOR_HX711 = 1,
  SENSOR_MPU6050_PITCH = 2,
  SENSOR_DS3231_TEMP = 3,
  SENSOR_HX711_RANGE = 4
};



class SensorModule {
public:
  virtual void begin() {}
  virtual int measure() = 0;
  virtual void saveOffset(int addr) {}
  virtual void loadOffset(int addr) {}
};



/******************** DS3231 ********************/
class DS3231TempModule : public SensorModule {
public:
  void begin() override {}



  int measure() override {
    Wire.beginTransmission(0x68);
    Wire.write(0x11);
    Wire.endTransmission();



    Wire.requestFrom(0x68, 2);
    if (Wire.available() < 2) return 0;



    uint8_t msb = Wire.read();
    uint8_t lsb = Wire.read();



    int16_t rawTemp = ((int16_t)msb << 8) | lsb;
    rawTemp >>= 6;



    float temperature = rawTemp * 0.25;
    return (int)(temperature * 10.0);
  }
};



/******************** HX711 ********************/
class HX711Module : public SensorModule {
  HX711 hx;
  QuickStats stats;
  int factor;
  int offset = 0;
  int lastRange = 0;



  int dout, sck;



public:
  HX711Module(int d, int s, int f)
    : factor(f), dout(d), sck(s) {}



  void begin() override {
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



    lastRange = maxv - minv;
    return (maxv + minv) / 2;
  }



  int getRange() { return lastRange; }
};



/******************** MPU6050 ********************/
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



/******************** HX711 Range ********************/
class HX711RangeModule : public SensorModule {
  HX711Module* src;



public:
  HX711RangeModule(HX711Module* s) : src(s) {}



  int measure() override {
    return src ? src->getRange() : 0;
  }
};



/******************** チャンネル ********************/
SensorModule* channels[6] = { nullptr };



void setupChannels() {



  // ① 通常センサ生成
  for (int i = 0; i < 6; i++) {



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
  }



  // ② Range後付け（順序依存回避）
  for (int i = 0; i < 6; i++) {
    if (CH_ASSIGN[i] == SENSOR_HX711_RANGE) {



      SensorModule* src = channels[RANGE_SOURCE_CH - 1];



      if (CH_ASSIGN[RANGE_SOURCE_CH - 1] == SENSOR_HX711) {
        channels[i] = new HX711RangeModule((HX711Module*)src);
      } else {
        channels[i] = new HX711RangeModule(nullptr);
      }
    }
  }



  // ③ begin
  for (int i = 0; i < 6; i++) {
    if (channels[i]) channels[i]->begin();
  }
}



/******************** HEX変換（エンディアン明示） ********************/
String toHex(int v) {
  uint8_t hi = (v >> 8) & 0xFF;
  uint8_t lo = v & 0xFF;



  String s;
  if (hi < 0x10) s += "0";
  s += String(hi, HEX);
  if (lo < 0x10) s += "0";
  s += String(lo, HEX);



  return s;
}



/******************** SETUP ********************/
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



/******************** LOOP ********************/
void loop() {



  int value[6] = {0};



  for (int i = 0; i < 6; i++)
    if (channels[i]) value[i] = channels[i]->measure();



  String msg = "AT$SF=";
  for (int i = 0; i < 6; i++) msg += toHex(value[i]);
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
