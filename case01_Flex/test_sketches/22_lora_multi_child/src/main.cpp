/**
 * Monita LoRa 検証 Step22: 複数台テスト用 子機（ブレッドボード版、ダミーデータ送信）
 *
 * 【目的】
 *   LoRa複数台（13台想定）を同時運用したときの受信・RSSI・衝突の様子を、
 *   実際のセンサ値ではなくダミーデータで手軽に検証する。
 *   Flex v3.10基板は使わず、XIAO nRF52840 に E220-900T22S(JP) を直結したブレッドボードで動かす。
 *   受信側は Gateway基板 ver1.1（COMM_MODE_LORAビルド）の実機をそのまま使う。
 *
 * 【v3.10_loraとの違い（ブレッドボード簡易版のため）】
 *   - TCA9534（I2C GPIO拡張）は使わない。M0/M1はXIAOの直結ピン2本で独立制御
 *     （実基板ver3.10はM0/M1が基板上で短絡されP2の1本共通駆動だが、
 *       ブレッドボードではその制約が無いため、より素直な2本独立制御にしている）
 *   - HX711・TCA9546A・DS18B20・MPU6050は無し。CH1〜4はダミー値を送信する
 *   - DS3231（RTC）は無し。Hour/Minはダミー固定値（0:00）を送信する
 *   - 3V3_SWによる電源ゲーティングは無し。E220はXIAOの3V3から常時給電する
 *     （ベンチテストのため省電力は考慮しない）
 *   - D0ボタン・タレ機能は無し
 *
 * 【配線】
 *   XIAO D8 (TX)        → E220 RXD
 *   XIAO D9 (RX)         ← E220 TXD
 *   XIAO D1              → E220 M0（独立制御）
 *   XIAO D2              → E220 M1（独立制御）
 *   XIAO 3V3             → E220 VCC（常時給電）
 *   XIAO GND             → E220 GND
 *   E220 AUX             → 未接続（固定ディレイで代替）
 *
 * 【★書き込み前に必ず変更すること】
 *   DEVICE_ID を台ごとに重複しないユニークな値にする（下記「設定」参照）。
 *   Gateway側（gateway_v1.1）の ALLOWED_DEVICE_IDS にも同じ値を登録しておくこと。
 *
 * 【送信間隔】
 *   TEST_SLEEP_MINUTES で変更可能（初期値5分）。複数台の衝突を短時間で
 *   多く観察したい場合は短く、電池持ちを気にせず長時間放置したい場合は長くする。
 *
 * 【E220設定値】
 *   Gateway実機（gateway_v1.1）と同じ値を使う（チャンネル・アドレス・送信出力等）。
 *   起動毎にREADで確認し、不一致ならWRITEする（v3.10_loraと同じ選択肢A方式）。
 */

#include <Arduino.h>
#include <nrf.h>
#include <Adafruit_TinyUSB.h>  // USB CDC（Serial）。USE_TINYUSBビルド時に必要

// ============================================================
// ▼ 設定（台ごとに変更するのは基本的に DEVICE_ID のみ）
// ============================================================
// ★★★ 書き込み前に必ず変更: 13台テストなら 0x01〜0x0D のように重複しない値にする ★★★
static const uint8_t DEVICE_ID = 0x08;
static const uint8_t FW_VERSION = 2;

#define DEBUG_MODE          1     // 1: USB Serialデバッグログ有効
#define TEST_SLEEP_MINUTES  4     // 送信間隔（分）。初期値5分。衝突をたくさん見たい場合は短くする
#define BOOT_BLUE_MS         500

// ============================================================
// ピン割当（ブレッドボード配線。上記コメント参照）
// ============================================================
#define LORA_TX_PIN 8
#define LORA_RX_PIN 9
#define LORA_M0_PIN 1
#define LORA_M1_PIN 2
#define LORA_UART_BAUD 9600

// AUXピン（任意配線・デバッグ用）。E220のAUXはモジュール内部処理中（送信中含む）LOWになり、
// 準備完了でHIGHに戻る。配線した場合のみ -1 以外にすると、送信の瞬間に本当にモジュールが
// 動作しているか（=UARTに書いただけでなく実際にBUSYになったか）をログで確認できる。
// 未配線のまま有効にすると浮動ピンの不定値を拾うだけなので、必ず配線してから有効にすること。
#define LORA_AUX_PIN 3  // 例: 配線したら 3 などの空きピン番号に変更する

// ============================================================
// ステータスLED（XIAO nRF52840 Sense 内蔵の離散RGB。アクティブLOW）
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
static void statusSendGreen() { rgbHwShow(0, 255, 0); }
static void statusErrorRed()  { rgbHwShow(255, 0, 0); }

// ============================================================
// ウォッチドッグタイマー（nRF52840 内蔵 WDT）
// ============================================================
static uint32_t const WDT_TIMEOUT_MS = 10UL * 60UL * 1000UL;  // 10分（TEST_SLEEP_MINUTES=5分に余裕を持たせた値）

static void wdtInit(uint32_t timeoutMs) {
  NRF_WDT->CONFIG  = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);
  NRF_WDT->CRV     = (uint32_t)((uint64_t)timeoutMs * 32768ULL / 1000ULL);
  NRF_WDT->RREN    = WDT_RREN_RR0_Msk;
  NRF_WDT->TASKS_START = 1;
}
static inline void wdtFeed() { NRF_WDT->RR[0] = WDT_RR_RR_Reload; }

// ============================================================
// スリープ（RTC2 + WFI）。v3.10_loraと同じ方式だが、ボタン処理は省略した簡易版
// ============================================================
#define RTC2_PRESCALER 4095U
#define RTC2_TICKS_PER_SECOND 8U
#define RTC2_COUNTER_MASK 0x00FFFFFFU

static volatile bool s_rtc2Compare0Wake;

extern "C" void RTC2_IRQHandler(void) {
  if (NRF_RTC2->EVENTS_COMPARE[0]) {
    NRF_RTC2->EVENTS_COMPARE[0] = 0;
    (void)NRF_RTC2->EVENTS_COMPARE[0];
    NRF_RTC2->INTENCLR = RTC_INTENCLR_COMPARE0_Msk;
    NRF_RTC2->TASKS_STOP = 1;
    s_rtc2Compare0Wake = true;
  }
}

static void deepSleep(uint32_t minutes) {
  wdtFeed();

#if DEBUG_MODE
  Serial.println("[Sleep before wait]");
  delay(3000);  // USB接続でログを見る時間を確保
#endif

  rgbOff();
  Serial1.end();  // E220は常時給電のままだが、XIAO側のUARTペリフェラルは閉じておく

#if DEBUG_MODE
  Serial.print("[RTC2 sleep] "); Serial.print(minutes); Serial.println(" min");
  Serial.flush();
#endif

  if (minutes == 0U) minutes = 1U;
  uint64_t ticks64 = (uint64_t)minutes * 60ULL * (uint64_t)RTC2_TICKS_PER_SECOND;
  if (ticks64 > RTC2_COUNTER_MASK) ticks64 = RTC2_COUNTER_MASK;
  if (ticks64 < 1ULL) ticks64 = 1ULL;
  const uint32_t ticks = (uint32_t)ticks64;

  s_rtc2Compare0Wake = false;

  NRF_RTC2->TASKS_STOP = 1;
  NRF_RTC2->TASKS_CLEAR = 1;
  NRF_RTC2->PRESCALER = RTC2_PRESCALER;
  NRF_RTC2->EVTENCLR = 0xFFFFFFFFU;
  NRF_RTC2->EVENTS_COMPARE[0] = 0;
  (void)NRF_RTC2->EVENTS_COMPARE[0];
  NRF_RTC2->CC[0] = ticks;
  NRF_RTC2->INTENCLR = 0xFFFFFFFFU;
  NRF_RTC2->INTENSET = RTC_INTENSET_COMPARE0_Msk;

  NVIC_SetPriority(RTC2_IRQn, 7);
  NVIC_ClearPendingIRQ(RTC2_IRQn);
  NVIC_EnableIRQ(RTC2_IRQn);
  NRF_RTC2->TASKS_START = 1;

  while (!s_rtc2Compare0Wake) {
    __DSB();
    __WFI();
  }

  NVIC_DisableIRQ(RTC2_IRQn);
  NRF_RTC2->INTENCLR = RTC_INTENCLR_COMPARE0_Msk;
  NRF_RTC2->TASKS_STOP = 1;
}

// ============================================================
// LoRa（E220-900T22S(JP)）— v3.10_loraと同じ設定値・フレーム形式
//
// M0/M1はXIAO直結の2本（LORA_M0_PIN/LORA_M1_PIN）で独立制御する
// （実基板ver3.10と違い、ブレッドボードでは短絡の制約が無いため素直に2本使う）。
// 設定コマンド・レジスタ配置は E220-900T22S(JP) 公式データシート準拠
// （詳細はメモリ`e220_register_map_and_bug`、Gateway実機と同一値を使うこと）。
// ============================================================
#define LORA_MODE_SWITCH_DELAY_MS 100U
#define LORA_CFG_REG_START 0x00
#define LORA_CFG_REG_LEN   6

// Gateway実機（gateway_v1.1）と同一値。ここを変える場合はGateway側も合わせて変更すること
static const uint8_t LORA_CFG_ADDH = 0x00;
static const uint8_t LORA_CFG_ADDL = 0x00;
static const uint8_t LORA_CFG_REG0 = 0x68;  // UART9600bps + エア速度(SF7/BW125kHz)
static const uint8_t LORA_CFG_REG1 = 0x01;  // ペイロード長200B/RSSIノイズ無効/送信出力13dBm
static const uint8_t LORA_CFG_REG2 = 0x00;  // チャンネル0
static const uint8_t LORA_CFG_REG3 = 0x80;  // RSSIバイト有効化ON/透過送信モード

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

// 1回分の確認・書込処理（下のloraCheckAndConfigure()からリトライ付きで呼ばれる）
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

// ブレッドボードは接触が不安定になりやすく、READ/WRITEのどこか1箇所が
// たまたま失敗しただけで毎回送信をスキップしてしまうのはもったいないため、
// 数回リトライする（2026-07-20追加。config read/write確認の間欠的な失敗を吸収する）。
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

#if LORA_AUX_PIN >= 0
// AUXレベルをログに出す（配線している場合のみ意味を持つ）。
// LOW=モジュール内部処理中（送信中含む）、HIGH=待機中/準備完了。
static void loraLogAux(const char *label) {
  Serial.print("[LORA] AUX(");
  Serial.print(label);
  Serial.print(")=");
  Serial.println(digitalRead(LORA_AUX_PIN) == HIGH ? "HIGH" : "LOW");
}
#endif

// デバッグ用: 送信する生バイト列をそのままHEXダンプする
// （「Serial1.write()に何を渡したか」を目視で追えるようにする。実際に電波に乗ったかまでは
//   分からないが、ソフト側のフレーム組み立てミスの切り分けには使える）。
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

// 透過モードでフレームを送信する（[SYNC][LEN][payload...][checksum]）。v3.10_loraと同一形式
static void loraSendFrame(const uint8_t *msd, uint8_t msdLen) {
  uint8_t sum = (uint8_t)(0xAAU + msdLen);
  for (uint8_t i = 0; i < msdLen; i++) sum = (uint8_t)(sum + msd[i]);

#if DEBUG_MODE
  loraPrintFrameHex(msd, msdLen, sum);
#endif
#if LORA_AUX_PIN >= 0
  loraLogAux("write前");
#endif

  Serial1.write((uint8_t)0xAA);
  Serial1.write(msdLen);
  for (uint8_t i = 0; i < msdLen; i++) Serial1.write(msd[i]);
  Serial1.write(sum);
  Serial1.flush();  // UARTバッファ送出完了を待つ（write()はバッファに積むだけなので明示的にflush）

#if LORA_AUX_PIN >= 0
  loraLogAux("write直後");
  // AUXがLOWに落ちて戻るまでを最大500msポーリングし、実際にモジュールがBUSYになったか確認する
  unsigned long auxT0 = millis();
  bool sawBusy = false;
  while (millis() - auxT0 < 500UL) {
    if (digitalRead(LORA_AUX_PIN) == LOW) { sawBusy = true; break; }
  }
  Serial.println(sawBusy ? "[LORA] AUX BUSY検出（モジュールが処理を開始した）"
                          : "[LORA] AUX BUSY未検出（モジュールが反応していない疑い）");
  if (sawBusy) {
    unsigned long readyT0 = millis();
    while (millis() - readyT0 < 1000UL && digitalRead(LORA_AUX_PIN) == LOW) { /* wait */ }
    loraLogAux("処理完了後");
  }
#endif
}

// v3.10_loraと同じ理由（AUX未接続で送信完了を検知できないため）。冗長送信も同様に行う
#define LORA_TX_COMPLETE_DELAY_MS 300U
#define LORA_TX_REPEAT 2
#define LORA_TX_REPEAT_GAP_MS 100U

// ============================================================
// ダミーデータ送信
//
// MSDペイロード（19バイト、v3.10_loraの本番フォーマットと同一。Gateway側の
// パーサーをそのまま使い回せる）:
//   [0]PktType=0x04 [1]DeviceID [2]FWVersion [3-10]CH1-4(int16 LE、ダミー値)
//   [11-12]BATT(ダミー固定値) [13]Hour=0 [14]Min=0（RTC無しのため固定） [15-18]Range=0
// ============================================================
static uint32_t s_dummyCounter = 0;
static const uint16_t DUMMY_BATT_MV = 3700;  // ダミーの電池電圧表示（実測ではない）

static void sendLoRaDummy() {
  if (!loraCheckAndConfigure()) {
    statusErrorRed();
#if DEBUG_MODE
    Serial.println("[LORA] config check failed, TX skipped");
#endif
    return;
  }

  s_dummyCounter++;
  int16_t ch[4] = {
    (int16_t)(s_dummyCounter),
    (int16_t)(s_dummyCounter + 100),
    (int16_t)(s_dummyCounter + 200),
    (int16_t)(s_dummyCounter + 300),
  };

  uint8_t msd[19];
  msd[0] = 0x04;  // Pkt type: Monita Flex v3.10 LoRa（本番と同一。Gateway側パーサー流用のため）
  msd[1] = DEVICE_ID;
  msd[2] = FW_VERSION;
  for (int i = 0; i < 4; i++) {
    msd[3 + i * 2]     = (uint8_t)(ch[i] & 0xFF);
    msd[3 + i * 2 + 1] = (uint8_t)((ch[i] >> 8) & 0xFF);
  }
  msd[11] = (uint8_t)(DUMMY_BATT_MV & 0xFF);
  msd[12] = (uint8_t)((DUMMY_BATT_MV >> 8) & 0xFF);
  msd[13] = 0;  // Hour（RTC無しのためダミー固定）
  msd[14] = 0;  // Min（同上）
  for (int i = 0; i < 4; i++) msd[15 + i] = 0;  // Range（同上）

  statusSendGreen();
  for (uint8_t rep = 0; rep < LORA_TX_REPEAT; rep++) {
    loraSendFrame(msd, sizeof(msd));
    delay(LORA_TX_COMPLETE_DELAY_MS);
    if (rep + 1 < LORA_TX_REPEAT) delay(LORA_TX_REPEAT_GAP_MS);
  }

#if DEBUG_MODE
  // ★送信直後にもう一度Configモードへ入って読み出せるか確認する。
  // TX時の電流バーストでブラウンアウト・内部リセット・ハングが起きていれば、
  // ここでread失敗 or 期待値と不一致になって現れるはず（電源不足の直接証拠になる）。
  {
    Serial.println("[LORA] 送信直後の生存確認（再度Config読み出し）...");
    bool postOk = loraCheckAndConfigureOnce();
    Serial.println(postOk ? "[LORA] 送信後も生存確認OK（設定一致 or 再書込成功）"
                           : "[LORA] ★送信後の生存確認NG（TX時に電源断/ハングした疑い）");
  }
#endif

#if DEBUG_MODE
  Serial.print("[LORA] TX DeviceID=0x"); Serial.print(DEVICE_ID, HEX);
  Serial.print(" counter="); Serial.print(s_dummyCounter);
  Serial.print(" CH="); for (int i = 0; i < 4; i++) { Serial.print(ch[i]); Serial.print(" "); }
  Serial.println();
#endif
}

// ============================================================
// Arduino エントリ
// ============================================================
static void loraUartBegin() {
  Serial1.setPins(LORA_RX_PIN, LORA_TX_PIN);
  Serial1.begin(LORA_UART_BAUD);
  delay(500);  // E220 起動待ち（暫定値）
}

void setup() {
  wdtInit(WDT_TIMEOUT_MS);

  rgbHwBegin();
  statusBootBlue();
  delay(BOOT_BLUE_MS);
  rgbOff();

  pinMode(LORA_M0_PIN, OUTPUT);
  pinMode(LORA_M1_PIN, OUTPUT);
  digitalWrite(LORA_M0_PIN, LOW);
  digitalWrite(LORA_M1_PIN, LOW);
#if LORA_AUX_PIN >= 0
  pinMode(LORA_AUX_PIN, INPUT);
#endif

#if DEBUG_MODE
  Serial.begin(115200);
  for (int rep = 0; rep < 3; rep++) {
    delay(1000);
    Serial.println("\n[Step22] LoRa複数台テスト 子機（ダミーデータ）起動");
    Serial.print("DeviceID=0x"); Serial.println(DEVICE_ID, HEX);
    Serial.print("送信間隔="); Serial.print(TEST_SLEEP_MINUTES); Serial.println("分");
  }
#endif

  loraUartBegin();

  sendLoRaDummy();
  deepSleep(TEST_SLEEP_MINUTES);
}

void loop() {
  wdtFeed();
  loraUartBegin();  // スリープ中にSerial1.end()しているため再初期化

#if DEBUG_MODE
  Serial.println("[WAKE]");
#endif

  sendLoRaDummy();
  deepSleep(TEST_SLEEP_MINUTES);
}
