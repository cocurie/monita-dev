/**
 * Monita LoRa 検証 Step03（Gateway側）: ダウンリンク送信テストツール（Gateway v1.1実機版）
 *
 * 【目的】
 *   case01_Flex/test_sketches/25_lora_downlink_child（Flex実機）が正しく
 *   ダウンリンクを受信・検証・適用できるかを確認するための送信専用ツール。
 *   Gateway v1.1実機（XIAO nRF52840 + E220-900T22S(JP)）にそのまま書き込んで使う。
 *
 * 【★2026-08-07: case01_Flex/test_sketches/26_lora_downlink_sender との違い】
 *   - 26番はXIAO+E220ブレッドボード単体・D0ボタン押下で1回送信、という設計だったが、
 *     Gateway v1.1実機で進めることになったため新規作成した。
 *   - Gateway v1.1はD0がLoRa RX（UARTE1）として使用済みでボタンが無い
 *     （物理スイッチ自体、他の理由で撤去済み。gateway_v1.00_to_v1.10_diff.md 補足参照）。
 *     そのため「一定間隔で自動的に連続送信」する方式にした。
 *   - LoRa UARTもGateway v1.1と同じ第2ハードウェアUART（UARTE1、D0/D1）を使う。
 *     FlexのSerial1(D8/D9)とは配線・実装方式が異なる点に注意。
 *   - UARTE1を自前で使う場合、割り込みハンドラを手動転送しないと write() が
 *     2回目の呼び出しで無期限にブロックする不具合がある（gateway_v1.1本番ファームで
 *     実機デバッグ済みの既知の罠）。同じ回避策をそのまま踏襲している。
 *
 * 【配線】Gateway v1.1実機そのまま（platformio.iniのコメント参照）
 *   XIAO D0 ← E220 TXD / XIAO D1 → E220 RXD / XIAO D2 → E220 M0・M1共通駆動
 *
 * 【送信内容】
 *   TEST_SEND_TIME_FLAG / TEST_SEND_SLEEP_FLAG / TEST_SEND_AVGMED_FLAG の
 *   define で、どのフラグを立てて送るか切り替えられる。
 *   時刻はビルド時刻（__DATE__/__TIME__）を使う。
 *   SEND_INTERVAL_MS 間隔で自動的に送信を繰り返す。
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

// 自動連続送信の間隔（ms）。Gateway v1.1にはD0ボタンが無いため、
// ボタン押下の代わりにこの間隔で自動的にダウンリンクを送信し続ける。
#define SEND_INTERVAL_MS  15000UL

// ============================================================
// ピン割当（Gateway v1.1実機。gateway_v1.1/platformio.iniのコメント参照）
// ============================================================
static int const LORA_RX_PIN   = 0;  // D0: E220 TXD → XIAO RX
static int const LORA_TX_PIN   = 1;  // D1: XIAO TX → E220 RXD
static int const LORA_M0M1_PIN = 2;  // D2: E220 M0・M1 共通駆動（基板でM0/M1短絡済み）
#define LORA_UART_BAUD 9600

// ============================================================
// LoRa用UART（UARTE1、第2ハードウェアUART）
//
// gateway_v1.1/src/main.cpp と同じ理由で、割り込みハンドラを手動転送しないと
// write() が2回目の呼び出しで無期限にブロックする（実機デバッグ済みの既知の罠）。
// ============================================================
static Uart loraSerial(NRF_UARTE1, UARTE1_IRQn, LORA_RX_PIN, LORA_TX_PIN);

extern "C" void UARTE1_IRQHandler(void) {
  loraSerial.IrqHandler();
}

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
// LoRa（E220-900T22S(JP)）— 子機・Gateway本番ファームと同一設定値
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

static bool loraSetMode(bool high) {
  digitalWrite(LORA_M0M1_PIN, high ? HIGH : LOW);
  delay(LORA_MODE_SWITCH_DELAY_MS);
  return true;
}
static inline bool loraModeNormal() { return loraSetMode(false); }
static inline bool loraModeConfig() { return loraSetMode(true); }

static bool loraReadConfig(uint8_t *out6) {
  while (loraSerial.available()) loraSerial.read();
  loraSerial.write((uint8_t)0xC1);
  loraSerial.write((uint8_t)LORA_CFG_REG_START);
  loraSerial.write((uint8_t)LORA_CFG_REG_LEN);

  const int respLen = 3 + LORA_CFG_REG_LEN;
  uint8_t resp[3 + LORA_CFG_REG_LEN];
  int idx = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 500UL && idx < respLen) {
    if (loraSerial.available()) resp[idx++] = (uint8_t)loraSerial.read();
  }
  if (idx < respLen) return false;
  if (resp[0] != 0xC1) return false;
  memcpy(out6, &resp[3], LORA_CFG_REG_LEN);
  return true;
}

static void loraWriteConfig() {
  loraSerial.write((uint8_t)0xC0);
  loraSerial.write((uint8_t)LORA_CFG_REG_START);
  loraSerial.write((uint8_t)LORA_CFG_REG_LEN);
  loraSerial.write(LORA_CFG_ADDH);
  loraSerial.write(LORA_CFG_ADDL);
  loraSerial.write(LORA_CFG_REG0);
  loraSerial.write(LORA_CFG_REG1);
  loraSerial.write(LORA_CFG_REG2);
  loraSerial.write(LORA_CFG_REG3);
  delay(200);
  unsigned long t0 = millis();
  while (millis() - t0 < 300UL) { while (loraSerial.available()) loraSerial.read(); }
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

// ★2026-08-07修正: フレーム全体をRAM上に組み立ててから1回のブロック書き込みで送る。
//
// 【経緯】
//   当初は 0xAA / LEN / payload / checksum をバイト単位で write() していたが、
//   実機テストでFlex側に一切届かなかった。一方、同じGateway基板・同じモジュールで
//   19_lora_parent に足したPING送信（uint8_t配列を write(buf,len) で1回送信）は
//   確実に届いた。Adafruitコアの write(uint8_t) は内部で write(&data,1) を呼ぶため
//   1バイトごとに個別のDMA転送＋ENDTX待ちが発生し、E220の透過モードでの
//   パケット境界判定に影響した可能性がある。実績のあるブロック書き込みに揃える。
static void loraSendFrame(const uint8_t *msd, uint8_t msdLen) {
  uint8_t sum = (uint8_t)(0xAAU + msdLen);
  for (uint8_t i = 0; i < msdLen; i++) sum = (uint8_t)(sum + msd[i]);

#if DEBUG_MODE
  loraPrintFrameHex(msd, msdLen, sum);
#endif

  uint8_t frame[3 + 32];  // [SYNC][LEN][payload...][checksum]
  uint8_t n = 0;
  frame[n++] = 0xAA;
  frame[n++] = msdLen;
  for (uint8_t i = 0; i < msdLen; i++) frame[n++] = msd[i];
  frame[n++] = sum;

  loraSerial.write(frame, n);
  loraSerial.flush();
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
  // ★2026-08-07修正: 送信のたびの設定確認（Configモードへの出入り）をやめ、
  //   Normalモードを再確定するだけにした。
  //
  // 【経緯】
  //   当初は送信のたびに loraCheckAndConfigure() を呼んでいた（Config→read→Normal）。
  //   E220はConfigモード中は電波を送受信できず、Normalへ戻すのに固定100msの待ちしか
  //   入れていない（AUX未接続のため完了を検知できない）。実機テストではこの構成で
  //   Flex側に一切届かず、一方 19_lora_parent のPING送信（Normalのまま送信）は
  //   確実に届いた。設定確認は起動時に1回だけ行う方式（19と同じ）に揃える。
  loraModeNormal();

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
  loraSerial.begin(LORA_UART_BAUD);
  delay(500);
}

void setup() {
  rgbHwBegin();
  statusBootBlue();
  delay(500);

  pinMode(LORA_M0M1_PIN, OUTPUT);
  digitalWrite(LORA_M0M1_PIN, LOW);  // Normalモードで起動

#if DEBUG_MODE
  Serial.begin(115200);
  for (int rep = 0; rep < 3; rep++) {
    delay(1000);
    Serial.println("\n[Step03] LoRaダウンリンク送信テストツール起動（Gateway v1.1実機版）");
    Serial.print("送信先DeviceID=0x"); Serial.println(TARGET_DEVICE_ID, HEX);
    Serial.print(SEND_INTERVAL_MS / 1000UL); Serial.println("秒間隔で自動送信します（D0ボタンは無し）");
  }
#endif

  loraUartBegin();

  // 設定確認は起動時に1回だけ行う（19_lora_parentと同じ方式）。
  // 以降の送信ではConfigモードへ入らず、Normalモードのまま送信する。
  if (!loraCheckAndConfigure()) {
#if DEBUG_MODE
    Serial.println("[LORA] ★起動時の設定確認に失敗しました（配線・給電を確認してください）");
#endif
  }
  loraModeNormal();

  statusIdle();
}

static unsigned long s_lastSendMs = 0;

void loop() {
  if (millis() - s_lastSendMs >= SEND_INTERVAL_MS) {
    s_lastSendMs = millis();
    sendDownlinkCommand();
  }
}
