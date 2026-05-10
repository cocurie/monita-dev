#include <HX711.h>
#include <EEPROM.h>
#include <Wire.h>
#include <DS3232RTC.h>
#include <QuickStats.h>

#define BAUDRATE 115200
#define DATA_NUM 5
#define REPEAT_NUM 5

#define factor1 1065
#define factor2 1065
#define factor3 1065
#define factor4 1065
#define factor5 1065
#define factor6 1065

#define SARA_RESET 9
#define button 12
#define donepin 13
#define INTERVAL_MS (60000UL * 120) // 電源が切れなかった場合の間隔（120分）

HX711 channel1, channel2, channel3, channel4, channel5, channel6;
DS3232RTC myRTC;
QuickStats stats;

int max_strain1, min_strain1, val1_smoothed;
int max_strain2, min_strain2, val2_smoothed;
int max_strain3, min_strain3, val3_smoothed;
int max_strain4, min_strain4, val4_smoothed;
int max_strain5, min_strain5, val5_smoothed;
int max_strain6, min_strain6, val6_smoothed;

float val1[DATA_NUM], val2[DATA_NUM], val3[DATA_NUM], val4[DATA_NUM], val5[DATA_NUM], val6[DATA_NUM];
int range1, range2, range3, range4, range5, range6;
int mean1, mean2, mean3, mean4, mean5, mean6;

int initial_val1, initial_val2, initial_val3, initial_val4, initial_val5, initial_val6;
int NUTPL;

void setup() {
  NUTPL = EEPROM.read(0xA00);

  pinMode(donepin, OUTPUT);
  pinMode(button, INPUT_PULLUP);
  pinMode(SARA_RESET, OUTPUT);
  digitalWrite(SARA_RESET, HIGH);

  if (NUTPL >= 2) {  // 指定回数で起動して送信
    EEPROM.write(0xA00, 0);

    Serial.begin(BAUDRATE);
    delay(200);
    
    // --- SARA-R410Mを確実にリセット ---
    digitalWrite(SARA_RESET, HIGH);
    delay(100);
    digitalWrite(SARA_RESET, LOW);
    delay(150);
    digitalWrite(SARA_RESET, HIGH);
    delay(7000); // 起動待ち（重要）

    // --- HX711初期化 ---
    channel1.begin(A0, A1);
    channel2.begin(A2, A3);
    channel3.begin(2, 3);
    channel4.begin(6, 7);
    channel5.begin(10, 11);
    channel6.begin(4, 5);
    Wire.begin();

  } else {
    // 起動回数を加算して電源を切る
    NUTPL++;
    EEPROM.write(0xA00, NUTPL);
    delay(200);
    digitalWrite(donepin, LOW);
    digitalWrite(donepin, HIGH);
  }
}

void loop() {

  // --- 初期値ボタン押下時 ---
  if (digitalRead(button) == LOW) {
    initial_val1 = int(channel1.read() / factor1);
    initial_val2 = int(channel2.read() / factor2);
    initial_val3 = int(channel3.read() / factor3);
    initial_val4 = int(channel4.read() / factor4);
    initial_val5 = int(channel5.read() / factor5);
    initial_val6 = int(channel6.read() / factor6);

    EEPROM.put(0x000, initial_val1);
    EEPROM.put(0x002, initial_val2);
    EEPROM.put(0x004, initial_val3);
    EEPROM.put(0x006, initial_val4);
    EEPROM.put(0x008, initial_val5);
    EEPROM.put(0x010, initial_val6);
  } else {
    EEPROM.get(0x000, initial_val1);
    EEPROM.get(0x002, initial_val2);
    EEPROM.get(0x004, initial_val3);
    EEPROM.get(0x006, initial_val4);
    EEPROM.get(0x008, initial_val5);
    EEPROM.get(0x010, initial_val6);
  }

  // --- ひずみ測定 ---
  measureChannel(channel1, val1, max_strain1, min_strain1, val1_smoothed, initial_val1, factor1);
  measureChannel(channel2, val2, max_strain2, min_strain2, val2_smoothed, initial_val2, factor2);
  measureChannel(channel3, val3, max_strain3, min_strain3, val3_smoothed, initial_val3, factor3);
  measureChannel(channel4, val4, max_strain4, min_strain4, val4_smoothed, initial_val4, factor4);
  measureChannel(channel5, val5, max_strain5, min_strain5, val5_smoothed, initial_val5, factor5);
  measureChannel(channel6, val6, max_strain6, min_strain6, val6_smoothed, initial_val6, factor6);

  // --- 範囲と中間値算出 ---
  range1 = max_strain1 - min_strain1; mean1 = (max_strain1 + min_strain1) / 2;
  range2 = max_strain2 - min_strain2; mean2 = (max_strain2 + min_strain2) / 2;
  range3 = max_strain3 - min_strain3; mean3 = (max_strain3 + min_strain3) / 2;
  range4 = max_strain4 - min_strain4; mean4 = (max_strain4 + min_strain4) / 2;
  range5 = max_strain5 - min_strain5; mean5 = (max_strain5 + min_strain5) / 2;
  range6 = max_strain6 - min_strain6; mean6 = (max_strain6 + min_strain6) / 2;

  // --- JSONデータ生成 ---
  char payload[300];
  sprintf_P(payload, PSTR("{\"mean1\":%d,\"mean2\":%d,\"mean3\":%d,\"mean4\":%d,\"mean5\":%d,\"mean6\":%d,"
                          "\"range1\":%d,\"range2\":%d,\"range3\":%d,\"range4\":%d,\"range5\":%d,\"range6\":%d}"),
            mean1, mean2, mean3, mean4, mean5, mean6, range1, range2, range3, range4, range5, range6);

  // --- SORACOM送信 ---
  sendCommand("AT", 3000);
  sendCommand("AT+CFUN=1", 3000);
  sendCommand("AT+UMNOPROF=20", 3000); // LTE-Mプロファイル
  sendCommand("AT+USOCR=6", 3000);
  sendCommand("AT+USOCO=0,\"uni.soracom.io\",23080", 5000);

  char hdr_buf[10];
  sprintf(hdr_buf, "%d", strlen(payload));
  sendCommand("AT+USOWR=0," + String(hdr_buf), 2000);
  sendCommand(payload, 3000);
  sendCommand("AT+USOCL=0", 1000);

  // --- 測定完了・電源オフ指示 ---
  digitalWrite(donepin, LOW);
  digitalWrite(donepin, HIGH);
  delay(200);
  digitalWrite(donepin, LOW);

  delay(INTERVAL_MS);  // 電源が切れなかった場合の安全用
}

/* ===========================================================
   関数群
   =========================================================== */

// HX711計測処理（中央値・最大・最小更新）
void measureChannel(HX711 &ch, float *vals, int &maxv, int &minv, int &median, int init_val, float factor) {
  maxv = -999999;
  minv = 999999;
  for (int j = 0; j < REPEAT_NUM; j++) {
    for (int i = 0; i < DATA_NUM; i++) {
      vals[i] = int(ch.read() / factor - init_val);
    }
    median = (int)stats.median(vals, DATA_NUM);
    if (median > maxv) maxv = median;
    if (median < minv) minv = median;
  }
}

// ATコマンド送信＋応答待機
void sendCommand(String command, int timeout) {
  Serial.print(command);
  Serial.print("\r\n");
  unsigned long start = millis();
  while (millis() - start < timeout) {
    while (Serial.available()) {
      char c = Serial.read();
      // 応答確認したい場合はここに Serial.print(c); を有効化
    }
  }
}
