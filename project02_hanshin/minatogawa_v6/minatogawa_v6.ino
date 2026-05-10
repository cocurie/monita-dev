/************************************************************
 *  Multi-Channel Strain Measurement System ver 6.1
 *  Arduino Mega + HX711 + LTE-M (SORACOM)
 *  
 *  - ゼロ点補正は zeroModification フラグで制御
 *  - zeroModification が true の場合のみ EEPROM 値表示
 ************************************************************/

/* ======================== 通信設定 ======================== */
#define INTERVAL_MS (600000UL * 10)
#define ENDPOINT "uni.soracom.io"

/* LTE-M Shield (SARA-R410M) */
#define RX 10
#define TX 11
#define BAUDRATE 115200
#define SARA_RESET 36

#include <SoftwareSerial.h>
SoftwareSerial LTE_M_shieldUART(RX, TX);

/* ======================== EEPROM設定 ======================== */
#include <EEPROM.h>

/* ======================== HX711 / 計測設定 ======================== */
#include <QuickStats.h>
#include <HX711.h>

#define CHANNEL_NUM 20

/* ----------- ★ ユーザー設定項目 ----------- */

bool channelEnabled[CHANNEL_NUM] = {
  true,true,true,true,true,
  true,true,true,true,true,
  true,true,true,true,true,
  true,true,true,true,true,
};

float gaugeFactor[CHANNEL_NUM] = {
  1065,1065,1065,1065,1065,
  1065,1065,1065,1065,1065,
  1065,1065,1065,1065,1065,
  1065,1065,1065,1065,1065
};

#define MEDIAN_SAMPLE_NUM 5
#define REPEAT_MEASURE_NUM 5
#define HX711_READ_DELAY_MS 1000

/* ----------- ★ ゼロ点補正モードフラグ ----------- */
bool zeroModification = false;  // trueの場合はゼロ点補正モード。 false は通常測定モード

/* ----------- ★ ここまでがユーザー設定項目 ----------- */

/* ======================== HX711設定 ======================== */

HX711 channels[CHANNEL_NUM];

const uint8_t hxPins[CHANNEL_NUM][2] = {
  {A0, A1}, {A2, A3}, {A4, A5}, {A6, A7}, {A8, A9},
  {A10, A11}, {A12, A13}, {A14, A15}, {2, 3}, {4, 5},
  {6, 7}, {8, 9}, {12, 13}, {14, 15}, {16, 17},
  {18, 19}, {22, 23}, {24, 25}, {26, 27}, {28, 29}
};

QuickStats stats;

/* ======================== 変数定義 ======================== */

int initial_val[CHANNEL_NUM];

float max_strain[CHANNEL_NUM];
float min_strain[CHANNEL_NUM];
float mean_val[CHANNEL_NUM];
float range_val[CHANNEL_NUM];
float val_buf[CHANNEL_NUM][MEDIAN_SAMPLE_NUM];

/* ======================== ATコマンド送信関数 ======================== */
void sendCommand(String command, int timeout) {
  LTE_M_shieldUART.print(command);
  LTE_M_shieldUART.print("\r\n");

  long start = millis();
  while (millis() - start < timeout) {
    while (LTE_M_shieldUART.available()) {
      LTE_M_shieldUART.read();
    }
  }
}

/* ======================== セットアップ ======================== */
void setup() {

  Serial.begin(BAUDRATE);
  delay(1000);
  LTE_M_shieldUART.begin(BAUDRATE);

  Serial.println(F("===== System Boot ====="));

  Serial.println(F("[1/5] LTE module reset..."));
  delay(100);
  pinMode(SARA_RESET, OUTPUT);
  digitalWrite(SARA_RESET, LOW);
  delay(500);
  digitalWrite(SARA_RESET, HIGH);
  delay(500);

  Serial.println(F("[2/5] HX711 initialization..."));
  delay(100);
  for (int i = 0; i < CHANNEL_NUM; i++) {
    channels[i].begin(hxPins[i][0], hxPins[i][1]);
  }

  Serial.println(F("[3/5] Loading calibration values from EEPROM..."));
  delay(100);
  for (int ch = 0; ch < CHANNEL_NUM; ch++) {
    EEPROM.get(ch * sizeof(int), initial_val[ch]);
  }

  Serial.println(F("[4/5] Communication interface ready."));
  delay(100);
  Serial.println(F("[5/5] Setup done!"));
  Serial.println(F("========================"));
}

/* ======================== メインループ ======================== */
void loop() {

  /********** 0/5 EEPROM表示（ゼロ点補正モード時のみ） **********/
  if (zeroModification) {
    Serial.println(F("===== EEPROM Zero Values ====="));
    for (int ch = 0; ch < CHANNEL_NUM; ch++) {
      int eeprom_val;
      EEPROM.get(ch * sizeof(int), eeprom_val);
      Serial.print(F("CH"));
      Serial.print(ch + 1);
      Serial.print(F(": "));
      Serial.println(eeprom_val);
    }
    Serial.println(F("=============================="));
  }

  Serial.println(F("===== Measurement cycle start ====="));

  /********** 1/5 ゼロ点補正モードチェック **********/
  if (zeroModification) {
    Serial.println(F("[1/5] Zero modification mode active. Updating initial values..."));

    for (int ch = 0; ch < CHANNEL_NUM; ch++) {
      if (!channelEnabled[ch]) continue;

      initial_val[ch] = (int)(channels[ch].read() / gaugeFactor[ch]);
      EEPROM.put(ch * sizeof(int), initial_val[ch]);

      Serial.print(F("  CH"));
      Serial.print(ch + 1);
      Serial.print(F(" initial = "));
      Serial.println(initial_val[ch]);
    }

    Serial.println(F("Zero calibration values updated in EEPROM."));
  }

  /********** 2/5 測定値初期化 **********/
  Serial.println(F("[2/5] Initializing measurement buffers..."));
  for (int ch = 0; ch < CHANNEL_NUM; ch++) {
    max_strain[ch] = -999999.0;
    min_strain[ch] =  999999.0;
  }

  /********** 3/5 センサ読み取り **********/
  Serial.println(F("[3/5] Reading HX711 sensors..."));
  for (int ch = 0; ch < CHANNEL_NUM; ch++) {

    if (!channelEnabled[ch]) {
      Serial.print(F("  -> CH"));
      Serial.print(ch + 1);
      Serial.println(F(" skipped (disabled)"));
      mean_val[ch]  = 0;
      range_val[ch] = 0;
      continue;
    }

    Serial.print(F("  -> CH"));
    Serial.print(ch + 1);
    Serial.println(F(" measurement start"));

    for (int repeat = 0; repeat < REPEAT_MEASURE_NUM; repeat++) {
      for (int i = 0; i < MEDIAN_SAMPLE_NUM; i++) {
        val_buf[ch][i] = (channels[ch].read() / gaugeFactor[ch]) - initial_val[ch];
      }

      float smoothed = stats.median(val_buf[ch], MEDIAN_SAMPLE_NUM);

      max_strain[ch] = max(max_strain[ch], smoothed);
      min_strain[ch] = min(min_strain[ch], smoothed);
    }

    range_val[ch] = max_strain[ch] - min_strain[ch];
    mean_val[ch]  = (max_strain[ch] + min_strain[ch]) / 2.0;

    Serial.print(F("     CH"));
    Serial.print(ch + 1);
    Serial.println(F(" done, waiting..."));
    delay(HX711_READ_DELAY_MS);
  }

  /********** 4/5 JSONペイロード生成 **********/
  Serial.println(F("[4/5] Creating payload..."));

  char payload[1000];
  int pos = 0;
  bool firstItem = true;

  pos += sprintf(payload + pos, "{");

  for (int ch = 0; ch < CHANNEL_NUM; ch++) {
    if (!channelEnabled[ch]) continue;

    if (!firstItem) pos += sprintf(payload + pos, ",");
    firstItem = false;

    pos += sprintf(payload + pos,
                   "\"mean%d\":%d,\"range%d\":%d",
                   ch + 1, (int)mean_val[ch],
                   ch + 1, (int)range_val[ch]);
  }

  pos += sprintf(payload + pos, "}");

  Serial.print(F("Payload: "));
  Serial.println(payload);

  /********** 5/5 LTE通信 **********/
  Serial.println(F("[5/5] Sending data via LTE..."));

  char hdr_buf[10];
  sprintf(hdr_buf, "%d", strlen(payload));

  sendCommand("AT", 1000);
  sendCommand("AT+UMNOPROF=20", 10000);
  sendCommand("AT+USOCR=6", 1000);
  sendCommand("AT+USOCO=0,\"" ENDPOINT "\",23080", 3000);

  sendCommand(String("AT+USOWR=0,") + hdr_buf, 3000);
  sendCommand(payload, 20000);

  sendCommand("AT+USORD=0,100", 3000);
  sendCommand("AT+USOCL=0", 1000);

  Serial.println(F("[5/5] Transmission done!"));
  Serial.println(F("===== Measurement cycle complete =====\n"));

  delay(INTERVAL_MS);
}
