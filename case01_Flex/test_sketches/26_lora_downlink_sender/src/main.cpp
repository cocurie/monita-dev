/**
 * Monita LoRa 検証 Step26: ダウンリンク送信テストツール（ブレッドボード版）
 *
 * 【目的】
 *   test_sketches/25_lora_downlink_child が正しくダウンリンクを受信・検証・適用
 *   できるかを、Gateway実機に触れずに確認するための送信専用ツール。
 *   XIAO nRF52840 Sense + E220-900T22S(JP) をブレッドボードに直結し、
 *   D0ボタンを押すたびに1回、TARGET_DEVICE_ID宛てのダウンリンクフレームを送信する。
 *
 * 【配線】25_lora_downlink_childと同一
 *   XIAO D8 (TX) → E220 RXD / XIAO D9 (RX) ← E220 TXD
 *   XIAO D1 → E220 M0 / XIAO D2 → E220 M1
 *   XIAO 3V3 → E220 VCC（常時給電）/ XIAO GND → E220 GND
 *   XIAO D0 → タクトスイッチ（もう片方はGND、INPUT_PULLUPで使用）
 *
 * 【送信内容】
 *   TEST_SEND_TIME_FLAG / TEST_SEND_SLEEP_FLAG / TEST_SEND_AVGMED_FLAG の
 *   define で、どのフラグを立てて送るか切り替えられる（テストのたびに
 *   コメントアウトを変えて個別に確認できるようにしている）。
 *   時刻はビルド時刻（__DATE__/__TIME__）を使う。
 *
 * フレーム仕様は25_lora_downlink_childのコメント参照（同一仕様）。
 */

#include <Arduino.h>
#include <nrf.h>
#include <Adafruit_TinyUSB.h>

// ============================================================
// ▼ 設定
// ============================================================
#define DEBUG_MODE            1

// 送信先（25_lora_downlink_child側のDEVICE_IDと合わせる）
static const uint8_t TARGET_DEVICE_ID = 0x08;

// どのフラグを立てて送るか（テストのたびに1/0を切り替える）
#define TEST_SEND_TIME_FLAG     1  // 時刻補正（ビルド時刻を送る）
#define TEST_SEND_SLEEP_FLAG    1  // 送信頻度変更
#define TEST_SEND_AVGMED_FLAG   1  // 平均/メジアン回数変更

// 送信頻度変更の値
#define TEST_NEW_SLEEP_MINUTES  7
// 平均/メジアン回数変更の値
#define TEST_NEW_SAMPLES_PER_AVG 8
#define TEST_NEW_MEASURE_COUNT   8

#define BUTTON_PIN  0   // D0
#define BOOT_BLUE_MS 500

// ============================================================
// ピン割当（ブレッドボード配線）
// ============================================================
#define LORA_TX_PIN 8
#define LORA_RX_PIN 9
#define LORA_M0_PIN 1
#define LORA_M1_PIN 2
#define LORA_UART_BAUD 9600

// ============================================================
// ステータスLED
// ============================================================
static void rgbHwBegin() {
#ifdef LED_RED
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
#endif
}
static void rgbHwShow(uint8_t r, uint8_t g, uint8_t b) {
#ifdef LED_RED
  analogWrite(LED_RED,   255 - r);
  analogWrite(LED_GREEN, 255 - g);
  analogWrite(LED_BLUE,  255 - b);
#endif
}
static void rgbOff()          { rgbHwShow(0, 0, 0); }
static void statusBootBlue()  { rgbHwShow(0, 0, 255); }
static void statusIdle()      { rgbHwShow(0, 0, 32); }   // 常時ダウンライトブルー: 待機中
static void statusSendGreen() { rgbHwShow(0, 255, 0); }

// ============================================================
// LoRa（E220-900T22S(JP)）— 子機・Gatewayと同一設定値
// ============================================================
#define LORA_MODE_SWITCH_DELAY_MS 100U
#define LORA_CFG_REG_START 0x00
#define LORA_CFG_REG_LEN   6

static const uint8_t LORA_CFG_ADDH = 0x00;
static const uint8_t LORA_CFG_ADDL = 0x00;
static const uint8_t LORA_CFG_REG0 = 0x68;
static const uint8_t LORA_CFG_REG1 = 0x01;
static const uint8_t LORA_CFG_REG2 = 0x00;
static const uint8_t LORA_CFG_REG3 = 0x80;

static void loraSetMode(bool m0High, bool m1High) {
  digitalWrite(LORA_M0_PIN, m0High ? HIGH : LOW);
  digitalWrite(LORA_M1_PIN, m1High ? HIGH : LOW);
  delay(LORA_MODE_SWITCH_DELAY_MS);
}
static inline void loraModeNormal() { loraSetMode(false, false); }
static inline void loraModeConfig() { loraSetMode(true, true); }

static bool loraReadConfig(uint8_t *out6) {
  while (Serial1.available()) Serial1.read();
  Serial1.write((uint8_t)0xC1);
  Serial1.write((uint8_t)LORA_CFG_REG_START);
  Serial1.write((uint8_t)LORA_CFG_REG_LEN);

  const int respLen = 3 + LORA_CFG_REG_LEN;
  uint8_t resp[3 + LORA_CFG_REG_LEN];
  int idx = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 500UL && idx < respLen) {
    if (Serial1.available()) resp[idx++] = (uint8_t)Serial1.read();
  }
  if (idx < respLen) return false;
  if (resp[0] != 0xC1) return false;
  memcpy(out6, &resp[3], LORA_CFG_REG_LEN);
  return true;
}

static void loraWriteConfig() {
  Serial1.write((uint8_t)0xC0);
  Serial1.write((uint8_t)LORA_CFG_REG_START);
  Serial1.write((uint8_t)LORA_CFG_REG_LEN);
  Serial1.write(LORA_CFG_ADDH);
  Serial1.write(LORA_CFG_ADDL);
  Serial1.write(LORA_CFG_REG0);
  Serial1.write(LORA_CFG_REG1);
  Serial1.write(LORA_CFG_REG2);
  Serial1.write(LORA_CFG_REG3);
  delay(200);
  unsigned long t0 = millis();
  while (millis() - t0 < 300UL) { while (Serial1.available()) Serial1.read(); }
}

static bool loraCheckAndConfigureOnce() {
  loraModeConfig();

  uint8_t cur[LORA_CFG_REG_LEN] = {0};
  bool readOk = loraReadConfig(cur);
  bool matches = readOk &&
      cur[0] == LORA_CFG_ADDH && cur[1] == LORA_CFG_ADDL &&
      cur[2] == LORA_CFG_REG0 && cur[3] == LORA_CFG_REG1 &&
      cur[4] == LORA_CFG_REG2 && cur[5] == LORA_CFG_REG3;

#if DEBUG_MODE
  Serial.print("[LORA] config read ");
  Serial.println(!readOk ? "失敗" : (matches ? "一致" : "不一致→書込"));
#endif

  if (!readOk) { loraModeNormal(); return false; }

  if (!matches) {
    loraWriteConfig();
    uint8_t verify[LORA_CFG_REG_LEN] = {0};
    bool verifyOk = loraReadConfig(verify) &&
        verify[0] == LORA_CFG_ADDH && verify[1] == LORA_CFG_ADDL &&
        verify[2] == LORA_CFG_REG0 && verify[3] == LORA_CFG_REG1 &&
        verify[4] == LORA_CFG_REG2 && verify[5] == LORA_CFG_REG3;
#if DEBUG_MODE
    Serial.println(verifyOk ? "[LORA] config write 確認OK" : "[LORA] config write 確認NG");
#endif
    if (!verifyOk) { loraModeNormal(); return false; }
  }

  loraModeNormal();
  return true;
}

#define LORA_CONFIG_RETRY 5
#define LORA_CONFIG_RETRY_DELAY_MS 500U

static bool loraCheckAndConfigure() {
  for (uint8_t attempt = 0; attempt < LORA_CONFIG_RETRY; attempt++) {
    if (loraCheckAndConfigureOnce()) return true;
#if DEBUG_MODE
    Serial.print("[LORA] config check 失敗、リトライ ");
    Serial.print(attempt + 1);
    Serial.print("/");
    Serial.println(LORA_CONFIG_RETRY);
#endif
    delay(LORA_CONFIG_RETRY_DELAY_MS);
  }
  return false;
}

static void loraPrintFrameHex(const uint8_t *msd, uint8_t msdLen, uint8_t sum) {
  Serial.print("[LORA] TXフレーム(HEX): AA ");
  if (msdLen < 0x10) Serial.print('0');
  Serial.print(msdLen, HEX);
  Serial.print(' ');
  for (uint8_t i = 0; i < msdLen; i++) {
    if (msd[i] < 0x10) Serial.print('0');
    Serial.print(msd[i], HEX);
    Serial.print(' ');
  }
  if (sum < 0x10) Serial.print('0');
  Serial.println(sum, HEX);
}

static void loraSendFrame(const uint8_t *msd, uint8_t msdLen) {
  uint8_t sum = (uint8_t)(0xAAU + msdLen);
  for (uint8_t i = 0; i < msdLen; i++) sum = (uint8_t)(sum + msd[i]);

#if DEBUG_MODE
  loraPrintFrameHex(msd, msdLen, sum);
#endif

  Serial1.write((uint8_t)0xAA);
  Serial1.write(msdLen);
  for (uint8_t i = 0; i < msdLen; i++) Serial1.write(msd[i]);
  Serial1.write(sum);
  Serial1.flush();
}

// ============================================================
// ビルド時刻の解析（__DATE__ "Mmm dd yyyy" / __TIME__ "hh:mm:ss"）
// v3.10_loraの初回時刻設定と同じロジック
// ============================================================
static void getBuildTime(uint8_t *year2000, uint8_t *month, uint8_t *day,
                          uint8_t *hour, uint8_t *minute, uint8_t *sec) {
  static const char monthNames[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char monStr[4] = {0};
  int d = 0, y = 0, h = 0, mi = 0, s = 0;
  sscanf(__DATE__, "%3s %d %d", monStr, &d, &y);
  sscanf(__TIME__, "%d:%d:%d", &h, &mi, &s);
  int m = (strstr(monthNames, monStr) - monthNames) / 3 + 1;

  *year2000 = (uint8_t)(y - 2000);
  *month = (uint8_t)m;
  *day = (uint8_t)d;
  *hour = (uint8_t)h;
  *minute = (uint8_t)mi;
  *sec = (uint8_t)s;
}

// ============================================================
// ダウンリンクフレーム送信（25_lora_downlink_childと同一仕様）
// ============================================================
static const uint16_t DOWNLINK_COMPANY_ID = 0xC0DE;
static const uint8_t  DOWNLINK_PKT_TYPE   = 0x81;

enum DownlinkFlag {
  DL_FLAG_TIME       = 1u << 0,
  DL_FLAG_SLEEP_MIN  = 1u << 1,
  DL_FLAG_AVG_MEDIAN = 1u << 2,
};

static void sendDownlinkCommand() {
  if (!loraCheckAndConfigure()) {
#if DEBUG_MODE
    Serial.println("[LORA] config check failed, TX skipped");
#endif
    return;
  }

  uint8_t flags = 0;
#if TEST_SEND_TIME_FLAG
  flags |= DL_FLAG_TIME;
#endif
#if TEST_SEND_SLEEP_FLAG
  flags |= DL_FLAG_SLEEP_MIN;
#endif
#if TEST_SEND_AVGMED_FLAG
  flags |= DL_FLAG_AVG_MEDIAN;
#endif

  uint8_t y, mo, da, h, mi, se;
  getBuildTime(&y, &mo, &da, &h, &mi, &se);

  uint8_t payload[15];
  payload[0] = (uint8_t)(DOWNLINK_COMPANY_ID >> 8);
  payload[1] = (uint8_t)(DOWNLINK_COMPANY_ID & 0xFF);
  payload[2] = DOWNLINK_PKT_TYPE;
  payload[3] = TARGET_DEVICE_ID;
  payload[4] = flags;
  payload[5] = y;
  payload[6] = mo;
  payload[7] = da;
  payload[8] = h;
  payload[9] = mi;
  payload[10] = se;
  payload[11] = (uint8_t)(TEST_NEW_SLEEP_MINUTES >> 8);
  payload[12] = (uint8_t)(TEST_NEW_SLEEP_MINUTES & 0xFF);
  payload[13] = TEST_NEW_SAMPLES_PER_AVG;
  payload[14] = TEST_NEW_MEASURE_COUNT;

  statusSendGreen();
#if DEBUG_MODE
  Serial.print("[DOWNLINK] 送信: 宛先=0x"); Serial.print(TARGET_DEVICE_ID, HEX);
  Serial.print(" flags=0b"); Serial.println(flags, BIN);
#endif
  loraSendFrame(payload, sizeof(payload));
  delay(300);
  statusIdle();
}

// ============================================================
// Arduino エントリ
// ============================================================
static void loraUartBegin() {
  Serial1.setPins(LORA_RX_PIN, LORA_TX_PIN);
  Serial1.begin(LORA_UART_BAUD);
  delay(500);
}

static bool s_lastButtonState = HIGH;

void setup() {
  rgbHwBegin();
  statusBootBlue();
  delay(BOOT_BLUE_MS);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LORA_M0_PIN, OUTPUT);
  pinMode(LORA_M1_PIN, OUTPUT);
  digitalWrite(LORA_M0_PIN, LOW);
  digitalWrite(LORA_M1_PIN, LOW);

#if DEBUG_MODE
  Serial.begin(115200);
  for (int rep = 0; rep < 3; rep++) {
    delay(1000);
    Serial.println("\n[Step26] LoRaダウンリンク送信テストツール起動");
    Serial.print("送信先DeviceID=0x"); Serial.println(TARGET_DEVICE_ID, HEX);
    Serial.println("D0ボタンを押すとダウンリンクを1回送信します");
  }
#endif

  loraUartBegin();
  statusIdle();
}

void loop() {
  bool btn = digitalRead(BUTTON_PIN);
  if (s_lastButtonState == HIGH && btn == LOW) {
    delay(20);  // チャタリング除去
    if (digitalRead(BUTTON_PIN) == LOW) {
      sendDownlinkCommand();
    }
  }
  s_lastButtonState = btn;
  delay(10);
}
