/************************************************************
 *  Multi-Channel Strain Measurement System ver 7.0
 *  Arduino Mega 2560 + HX711 x20 + M5Stamp SIM7080G (SORACOM)
 *
 *  v6 からの主な変更点:
 *    - 通信モジュール: SARA-R410M → SIM7080G
 *    - SIM: SORACOM (APN: soracom.io)
 *    - ATコマンド: SARA系 → SIM7080G系 (CAOPEN/CASEND/CACLOSE)
 *    - AT応答チェックを追加（送達確認あり）
 *    - QuickStats 依存を廃止（median を内部実装）
 *    - SHT40 (I2C) / DS18B20 (OneWire D30) 温度センサ追加
 *    - リセットボタン (RESET-GND 直結) / ゼロ点補正ボタン (D31, 4秒長押し)
 *
 *  HX711 ピン配置は v6 から変更なし。
 *  SoftwareSerial (D10/D11) を継続使用 ― ハードウェアUART (Serial1-3) は
 *  HX711 CH14/15/16 と競合するため使用不可。
 *
 ************************************************************
 *  Arduino Mega 2560 ピンアサイン
 * ----------------------------------------------------------
 *  [HX711  20ch]           DATA       CLK
 *    CH1                    A0         A1
 *    CH2                    A2         A3
 *    CH3                    A4         A5
 *    CH4                    A6         A7
 *    CH5                    A8         A9
 *    CH6                   A10        A11
 *    CH7                   A12        A13
 *    CH8                   A14        A15
 *    CH9                    D2         D3
 *    CH10                   D4         D5
 *    CH11                   D6         D7
 *    CH12                   D8         D9
 *    CH13                  D12        D13
 *    CH14                  D14        D15   ※ Serial3(TX3/RX3) と共用ピン
 *    CH15                  D16        D17   ※ Serial2(TX2/RX2) と共用ピン
 *    CH16                  D18        D19   ※ Serial1(TX1/RX1) と共用ピン
 *    CH17                  D22        D23
 *    CH18                  D24        D25
 *    CH19                  D26        D27
 *    CH20                  D28        D29
 *
 *  [SIM7080G  SoftwareSerial]
 *    SW-RX  (← SIM7080G TXD)   D10
 *    SW-TX  (→ SIM7080G RXD)   D11
 *    PWR KEY                    D36
 *    VCC                        5V / GND
 *
 *  [温度センサ]
 *    SHT40  SDA                D20   (USE_SHT40=true 時)
 *    SHT40  SCL                D21   (USE_SHT40=true 時)
 *    DS18B20 DQ                D30   (USE_DS18B20=true 時)
 *
 *  [ボタン]
 *    リセット                   RESET ─ GND (ハードウェア直結、GPIO不要)
 *    ゼロ点補正 (4秒長押し)      D31  ─ GND (内部プルアップ使用)
 *
 *  [USB シリアル / デバッグ]
 *    Serial TX                  D1   (シリアルモニタ出力)
 *
 *  [空きピン]
 *    D32–D35, D37–D53
 * ----------------------------------------------------------
 *  ※ HX711 は GPIO としてのみ使用。Serial1-3 のUART機能は
 *    CH14-16 のピンと物理的に競合するため使用不可。
 ************************************************************/

/* ======================== 通信設定 ======================== */
#define INTERVAL_MS     (600000UL * 10)   // 100分
#define ENDPOINT        "uni.soracom.io"
#define ENDPOINT_PORT   23080
#define LTE_BAUD        9600
#define SIM_PWRKEY_PIN  36
#define SW_RX           10
#define SW_TX           11

/* ======================== EEPROM設定 ======================== */
#include <EEPROM.h>

/* ======================== SoftwareSerial ======================== */
#include <SoftwareSerial.h>
SoftwareSerial simSerial(SW_RX, SW_TX);

/* ======================== HX711 設定 ======================== */
#include <HX711.h>

#define CHANNEL_NUM 20

/* ----------- ★ ユーザー設定項目 ----------- */

bool channelEnabled[CHANNEL_NUM] = {
  true, true, true, true, true,
  true, true, true, true, true,
  true, true, true, true, true,
  true, true, true, true, true,
};

float gaugeFactor[CHANNEL_NUM] = {
  1065, 1065, 1065, 1065, 1065,
  1065, 1065, 1065, 1065, 1065,
  1065, 1065, 1065, 1065, 1065,
  1065, 1065, 1065, 1065, 1065,
};

#define MEDIAN_SAMPLE_NUM   5
#define REPEAT_MEASURE_NUM  5
#define HX711_READ_DELAY_MS 1000

/* ----------- ★ ゼロ点補正ボタン設定 ----------- */
// D31 と GND の間にボタンを接続（内部プルアップ使用）
#define CALIB_BTN_PIN  31
#define CALIB_HOLD_MS  4000UL   // 長押し判定時間 (ms)

/* ----------- ★ ゼロ点補正フラグ (コンパイル時強制実行用) ----------- */
// ボタンが使えない場合の代替手段。通常は false のまま
bool zeroModification = false;

/* ----------- ★ 温度センサ ON/OFF ----------- */
#define USE_SHT40   true    // SHT40: I2C (D20=SDA, D21=SCL)
#define USE_DS18B20 false    // DS18B20: OneWire (D30)
#define DS18B20_PIN 30

/* ----------- ★ ここまでがユーザー設定項目 ----------- */

/* ======================== 温度センサ ライブラリ ======================== */
#if USE_SHT40
  #include <Wire.h>
  #include <Adafruit_SHT4x.h>
  Adafruit_SHT4x sht4;
  float sht_temp = 0.0;
  float sht_humi = 0.0;
  bool  sht40_ok = false;
#endif

#if USE_DS18B20
  #include <OneWire.h>
  #include <DallasTemperature.h>
  OneWire           oneWire(DS18B20_PIN);
  DallasTemperature ds18b20(&oneWire);
  float ds_temp    = 0.0;
  bool  ds18b20_ok = false;
#endif

/* ======================== HX711 ピン設定 ======================== */

HX711 channels[CHANNEL_NUM];

const uint8_t hxPins[CHANNEL_NUM][2] = {
  {A0, A1},   {A2, A3},   {A4, A5},   {A6, A7},   {A8, A9},
  {A10, A11}, {A12, A13}, {A14, A15}, {2, 3},     {4, 5},
  {6, 7},     {8, 9},     {12, 13},   {14, 15},   {16, 17},
  {18, 19},   {22, 23},   {24, 25},   {26, 27},   {28, 29},
};

/* ======================== 変数定義 ======================== */

int   initial_val[CHANNEL_NUM];
float max_strain[CHANNEL_NUM];
float min_strain[CHANNEL_NUM];
float mean_val[CHANNEL_NUM];
float range_val[CHANNEL_NUM];
float val_buf[MEDIAN_SAMPLE_NUM];
char  payload[1200];
bool  calibRequested = false;   // ボタン長押しで true になる

/* ======================== 中央値計算 ======================== */
float calcMedian(float* arr, int n) {
  float tmp[MEDIAN_SAMPLE_NUM];
  memcpy(tmp, arr, n * sizeof(float));
  for (int i = 0; i < n - 1; i++) {
    for (int j = 0; j < n - i - 1; j++) {
      if (tmp[j] > tmp[j + 1]) {
        float t = tmp[j]; tmp[j] = tmp[j + 1]; tmp[j + 1] = t;
      }
    }
  }
  return tmp[n / 2];
}

/* ======================== ATコマンド送信 ======================== */
String sendAT(const String& cmd, int timeoutMs = 5000) {
  Serial.print(F(">> "));
  Serial.println(cmd);
  simSerial.print(cmd);
  simSerial.print(F("\r\n"));

  long start = millis();
  String resp = "";
  while (millis() - start < timeoutMs) {
    while (simSerial.available()) {
      resp += (char)simSerial.read();
    }
  }
  Serial.print(resp);
  return resp;
}

/* ======================== LTE 初期化 ======================== */
bool initLTE() {
  Serial.println(F("--- LTE初期化 ---"));

  bool atOk = false;
  for (int i = 0; i < 10; i++) {
    if (sendAT("AT", 1000).indexOf("OK") >= 0) { atOk = true; break; }
    delay(1000);
  }
  if (!atOk) { Serial.println(F("✗ ATコマンド応答なし")); return false; }

  sendAT("AT+CNMP=38", 2000);
  sendAT("AT+CMNB=3", 2000);
  sendAT("AT+CGDCONT=1,\"IP\",\"soracom.io\"", 2000);

  // LTE登録待ち (AT+CREG は 2G/3G用, LTE-M は AT+CEREG を使う)
  Serial.println(F("ネットワーク登録待ち..."));
  bool registered = false;
  for (int i = 0; i < 12; i++) {
    String r = sendAT("AT+CEREG?", 3000);
    if (r.indexOf("0,1") >= 0 || r.indexOf("0,5") >= 0) {
      Serial.println(F("✓ ネットワーク登録成功")); registered = true; break;
    }
    delay(5000);
  }
  if (!registered) { Serial.println(F("✗ 登録タイムアウト")); return false; }

  sendAT("AT+CNACT=0,1", 15000);
  delay(2000);
  String cnact = sendAT("AT+CNACT?", 3000);
  if (cnact.indexOf("0,1") < 0) {
    Serial.println(F("✗ IPアドレス取得失敗")); return false;
  }

  Serial.println(F("✓ LTE初期化完了"));
  return true;
}

/* ======================== SORACOM uni TCP送信 ======================== */
bool sendPayload(const char* payload) {
  Serial.println(F("--- TCP送信 ---"));

  sendAT("AT+CACLOSE=0", 3000);
  delay(300);

  String openCmd = String(F("AT+CAOPEN=0,0,\"TCP\",\"")) + ENDPOINT + "\"," + ENDPOINT_PORT;
  String res = sendAT(openCmd, 15000);
  if (res.indexOf("+CAOPEN: 0,0") < 0) {
    Serial.println(F("✗ TCP接続失敗"));
    return false;
  }
  Serial.println(F("✓ TCP接続成功"));
  delay(300);

  int len = strlen(payload);
  simSerial.print("AT+CASEND=0,");
  simSerial.print(len);
  simSerial.print("\r\n");
  delay(1000);
  simSerial.print(payload);

  long t = millis();
  String sendResp = "";
  while (millis() - t < 3000) {
    while (simSerial.available()) sendResp += (char)simSerial.read();
  }
  Serial.print(sendResp);

  sendAT("AT+CACLOSE=0", 3000);

  if (sendResp.indexOf("OK") >= 0) {
    Serial.println(F("✓ 送信完了"));
    return true;
  }
  Serial.println(F("✗ 送信失敗"));
  return false;
}

/* ======================== ゼロ点補正 ======================== */
void runZeroCalibration() {
  Serial.println(F("===== Zero Calibration Start ====="));

  // 補正前の EEPROM 値を表示
  Serial.println(F("--- 現在の EEPROM 値 ---"));
  for (int ch = 0; ch < CHANNEL_NUM; ch++) {
    int v; EEPROM.get(ch * sizeof(int), v);
    Serial.print(F("  CH")); Serial.print(ch + 1);
    Serial.print(F(": ")); Serial.println(v);
  }

  // 各チャンネルのゼロ点を測定して EEPROM に保存
  Serial.println(F("--- ゼロ点測定中 ---"));
  for (int ch = 0; ch < CHANNEL_NUM; ch++) {
    if (!channelEnabled[ch]) continue;
    initial_val[ch] = (int)(channels[ch].read() / gaugeFactor[ch]);
    EEPROM.put(ch * sizeof(int), initial_val[ch]);
    Serial.print(F("  CH")); Serial.print(ch + 1);
    Serial.print(F(" -> ")); Serial.println(initial_val[ch]);
  }

  Serial.println(F("===== Zero Calibration Done ====="));
}

/* ======================== 100分待機（ボタン監視付き） ======================== */
// delay(INTERVAL_MS) の代わりに使用。
// 待機中に D31 を 4秒長押しすると calibRequested = true になり待機を終了する。
void waitInterval() {
  Serial.println(F("--- 待機中 (ゼロ点補正: D31 を 4秒長押し) ---"));

  unsigned long waitStart = millis();
  unsigned long btnStart  = 0;
  bool btnDown = false;

  while (millis() - waitStart < INTERVAL_MS) {
    delay(50);
    bool pressed = (digitalRead(CALIB_BTN_PIN) == LOW);

    if (pressed && !btnDown) {
      // 押し始め
      btnDown  = true;
      btnStart = millis();
      Serial.println(F("  [BTN] 押下 — 4秒長押しでゼロ点補正"));

    } else if (pressed && btnDown) {
      // 押し続け中: 4秒経過で確定
      if (millis() - btnStart >= CALIB_HOLD_MS) {
        Serial.println(F("  [BTN] 4秒長押し確認 → ゼロ点補正を実行します"));
        // ボタンが離されるまで待つ
        while (digitalRead(CALIB_BTN_PIN) == LOW) delay(10);
        calibRequested = true;
        return;   // 待機を終了して次サイクルへ
      }

    } else if (!pressed && btnDown) {
      // 4秒未満で離した
      Serial.println(F("  [BTN] 離れ（4秒未満 — キャンセル）"));
      btnDown = false;
    }
  }
}

/* ======================== セットアップ ======================== */
void setup() {
  Serial.begin(115200);
  delay(1000);
  simSerial.begin(LTE_BAUD);

  Serial.println(F("===== System Boot (v7) ====="));

  // ゼロ点補正ボタン
  pinMode(CALIB_BTN_PIN, INPUT_PULLUP);

  // SIM7080G 起動
  Serial.println(F("[1/5] SIM7080G power on..."));
  pinMode(SIM_PWRKEY_PIN, OUTPUT);
  digitalWrite(SIM_PWRKEY_PIN, HIGH);
  delay(100);
  digitalWrite(SIM_PWRKEY_PIN, LOW);
  delay(1000);
  digitalWrite(SIM_PWRKEY_PIN, HIGH);
  delay(5000);

  // HX711 + 温度センサ初期化
  Serial.println(F("[2/5] HX711 + sensor initialization..."));
  for (int i = 0; i < CHANNEL_NUM; i++) {
    channels[i].begin(hxPins[i][0], hxPins[i][1]);
  }

#if USE_SHT40
  Wire.begin();
  if (sht4.begin()) {
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    sht40_ok = true;
    Serial.println(F("  SHT40: OK"));
  } else {
    Serial.println(F("  SHT40: not found"));
  }
#endif

#if USE_DS18B20
  ds18b20.begin();
  if (ds18b20.getDeviceCount() > 0) {
    ds18b20_ok = true;
    Serial.print(F("  DS18B20: OK ("));
    Serial.print(ds18b20.getDeviceCount());
    Serial.println(F(" device(s))"));
  } else {
    Serial.println(F("  DS18B20: not found"));
  }
#endif

  // EEPROM からゼロ点読み込み
  Serial.println(F("[3/5] Loading calibration from EEPROM..."));
  for (int ch = 0; ch < CHANNEL_NUM; ch++) {
    EEPROM.get(ch * sizeof(int), initial_val[ch]);
  }

  // LTE 初期化
  Serial.println(F("[4/5] LTE initialization..."));
  if (!initLTE()) {
    Serial.println(F("✗ LTE初期化失敗 — 送信なしで測定続行"));
  }

  Serial.println(F("[5/5] Setup done!"));
  Serial.println(F("============================"));
}

/* ======================== メインループ ======================== */
void loop() {

  // ゼロ点補正: ボタン長押し or コンパイル時フラグ
  if (calibRequested || zeroModification) {
    calibRequested = false;
    runZeroCalibration();
  }

  Serial.println(F("===== Measurement cycle start ====="));

  // 1/5 バッファ初期化
  Serial.println(F("[1/5] Initializing measurement buffers..."));
  for (int ch = 0; ch < CHANNEL_NUM; ch++) {
    max_strain[ch] = -999999.0;
    min_strain[ch] =  999999.0;
  }

  // 2/5 HX711 センサ読み取り
  Serial.println(F("[2/5] Reading HX711 sensors..."));
  for (int ch = 0; ch < CHANNEL_NUM; ch++) {
    if (!channelEnabled[ch]) {
      Serial.print(F("  -> CH")); Serial.print(ch + 1);
      Serial.println(F(" skipped (disabled)"));
      mean_val[ch] = range_val[ch] = 0;
      continue;
    }

    Serial.print(F("  -> CH")); Serial.print(ch + 1);
    Serial.println(F(" measurement start"));

    for (int repeat = 0; repeat < REPEAT_MEASURE_NUM; repeat++) {
      for (int i = 0; i < MEDIAN_SAMPLE_NUM; i++) {
        val_buf[i] = (channels[ch].read() / gaugeFactor[ch]) - initial_val[ch];
      }
      float smoothed = calcMedian(val_buf, MEDIAN_SAMPLE_NUM);
      max_strain[ch] = max(max_strain[ch], smoothed);
      min_strain[ch] = min(min_strain[ch], smoothed);
    }

    range_val[ch] = max_strain[ch] - min_strain[ch];
    mean_val[ch]  = (max_strain[ch] + min_strain[ch]) / 2.0;

    Serial.print(F("     CH")); Serial.print(ch + 1);
    Serial.println(F(" done, waiting..."));
    delay(HX711_READ_DELAY_MS);
  }

  // 3/5 温度センサ読み取り
#if USE_SHT40
  if (sht40_ok) {
    sensors_event_t hum_ev, tmp_ev;
    sht4.getEvent(&hum_ev, &tmp_ev);
    sht_temp = tmp_ev.temperature;
    sht_humi = hum_ev.relative_humidity;
    Serial.print(F("  SHT40: temp=")); Serial.print(sht_temp, 1);
    Serial.print(F(" humi="));         Serial.println(sht_humi, 1);
  }
#endif
#if USE_DS18B20
  if (ds18b20_ok) {
    ds18b20.requestTemperatures();
    ds_temp = ds18b20.getTempCByIndex(0);
    Serial.print(F("  DS18B20: temp=")); Serial.println(ds_temp, 1);
  }
#endif

  // 4/5 JSONペイロード生成
  Serial.println(F("[4/5] Creating payload..."));
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

#if USE_SHT40
  if (sht40_ok) {
    char st[8], sh[8];
    dtostrf(sht_temp, 4, 1, st);
    dtostrf(sht_humi, 4, 1, sh);
    if (!firstItem) pos += sprintf(payload + pos, ",");
    firstItem = false;
    pos += sprintf(payload + pos, "\"temp_sht\":%s,\"humi\":%s", st, sh);
  }
#endif
#if USE_DS18B20
  if (ds18b20_ok) {
    char sd[8];
    dtostrf(ds_temp, 4, 1, sd);
    if (!firstItem) pos += sprintf(payload + pos, ",");
    firstItem = false;
    pos += sprintf(payload + pos, "\"temp_ds\":%s", sd);
  }
#endif

  pos += sprintf(payload + pos, "}");
  Serial.print(F("Payload: ")); Serial.println(payload);
  Serial.print(F("Length: ")); Serial.println(pos);

  // 5/5 LTE送信
  Serial.println(F("[5/5] Sending data via LTE..."));
  sendPayload(payload);

  Serial.println(F("===== Measurement cycle complete =====\n"));

  // 100分待機（ゼロ点補正ボタンを監視）
  waitInterval();
}
