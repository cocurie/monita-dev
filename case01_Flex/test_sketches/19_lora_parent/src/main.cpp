/**
 * Monita LoRa 検証 Step19: 親機（受信側）— Gateway ver1.1 基板での電波強度連続受信テスト
 *
 * 【目的】
 *   Gateway ver1.1 基板（E220-900T22S(JP) 直結・D0/D1/D2割当）を使って、
 *   Flex子機から届くLoRaフレームを連続受信し、RSSI（電波強度）だけをひたすら
 *   表示する。Gateway本番ファーム（gateway_v1.1/src/main.cpp）のLoRa受信部分
 *   （フレーム同期・チェックサム・RSSIバイト読取・ストール監視ロジック）を
 *   そのまま流用し、BLE/LTE-M/SD/GAS送信など受信テストに不要な機能は省いた
 *   最小構成にしている。
 *
 * 【対象ハード】Gateway ver1.1 基板（ブレッドボード直結ではない）
 *   XIAO D0（RX）  ← E220 TXD（net UART_RX_2）
 *   XIAO D1（TX）  → E220 RXD（net UART_TX_2）
 *   XIAO D2        → E220 M0・M1 共通駆動（基板側でM0/M1短絡済み、net LORA_SETTING）
 *   ※ ブレッドボード版（M0/M1をGND直結でMode0固定）とはピン・配線が異なるので注意。
 *
 * 【E220設定】
 *   起動時に gateway_v1.1 と同じレジスタ（UART9600bps/エア速度SF7,BW125kHz・
 *   送信出力13dBm・チャンネル0・RSSIバイト有効化）を読み取り、想定値と違えば
 *   書き込む。Flex子機側（v3.10_lora）と設定が揃っていないと受信できない。
 *
 * 【動作】
 *   フレームを1つ受信するたびに、DeviceID・RSSI(dBm)・累計受信数を1行表示する。
 *   さらに10秒ごとに簡易統計（受信数・チェックサムNG数・直近RSSI）をまとめて表示する。
 *   E220はREG3(RSSIバイト有効化)の設定により、受信データの直後にRSSIバイトを
 *   自動付加する。dBm = RSSIバイト（0〜255の生値）－256（公式データシート記載の式）。
 *
 * 【フレーム形式】（Flex側 v3.10_lora/main.cpp が送る形式と同一）
 *   [0]SYNC=0xAA [1]LEN [2..LEN+1]MSDペイロード [LEN+2]チェックサム(単純和) [+1]RSSI
 *   本テストではDeviceIDのフィルタは行わず、チェックサムが合ったフレームは
 *   すべて表示する（電波強度確認が目的のため、想定外Device IDでも捨てない）。
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>  // USB CDC（Serial）。USE_TINYUSBビルド時に必要

// ── ピン割当（Gateway ver1.1 基板）─────────────────
static int const LORA_RX_PIN   = 0;  // D0: E220 TXD → XIAO RX（net UART_RX_2）
static int const LORA_TX_PIN   = 1;  // D1: XIAO TX → E220 RXD（net UART_TX_2）
static int const LORA_M0M1_PIN = 2;  // D2: E220 M0・M1 共通駆動

#define LORA_MODE_SWITCH_DELAY_MS 100U
#define MAX_PAYLOAD 24

// ★2026-08-07追加（切り分け用）: 1 にすると受信と並行して定期的にPINGを送信する。
//   Flex基板が1台しかなく「Gatewayの送信」と「Flexの受信」の検証が循環していたため、
//   受信が実証済みのこのスケッチに送信だけを足して双方向テストできるようにした。
//   相手は 18_lora_child（RX_DUMP_ENABLED=1 で受信バイトをダンプするようにしたもの）。
//   Flex側に [RX raw] が出ればGatewayの送信は電波に乗っている＝Flexの受信も生きている。
//   切り分けが済んだら 0 に戻してよい。
#define TX_PING_ENABLED   1
#define TX_PING_INTERVAL_MS 3000UL

static const uint8_t TARGET_DEVICE_ID = 0x0E;  // このDeviceIDのフレームのみ表示する

// UARTE1（第2ハードウェアUART）をRX専用として使う。gateway_v1.1と同じ構成。
static Uart loraSerial(NRF_UARTE1, UARTE1_IRQn, LORA_RX_PIN, LORA_TX_PIN);

// ★UARTE1を自前で使う場合、割り込みハンドラをこのように手動で転送しないと
//   send/receive の完了通知が届かず、write()が2バイト目以降で永久にブロックする
//   （gateway_v1.1で実機確認済みの既知の罠）。
extern "C" void UARTE1_IRQHandler(void) {
  loraSerial.IrqHandler();
}

static bool loraSetMode(bool high) {
  digitalWrite(LORA_M0M1_PIN, high ? HIGH : LOW);
  delay(LORA_MODE_SWITCH_DELAY_MS);
  return true;
}
static inline bool loraModeNormal() { return loraSetMode(false); }
static inline bool loraModeConfig() { return loraSetMode(true); }

// 設定コマンド（gateway_v1.1・Flex側 v3.10_lora/main.cpp と同一値。全台共通）。
#define LORA_CFG_REG_START 0x00
#define LORA_CFG_REG_LEN   6
static const uint8_t LORA_CFG_ADDH = 0x00;
static const uint8_t LORA_CFG_ADDL = 0x00;
static const uint8_t LORA_CFG_REG0 = 0x68;  // UART9600bps + エア速度(SF7/BW125kHz)
static const uint8_t LORA_CFG_REG1 = 0x01;  // 送信出力13dBm
static const uint8_t LORA_CFG_REG2 = 0x00;  // チャンネル0
static const uint8_t LORA_CFG_REG3 = 0x80;  // RSSIバイト有効化ON/透過送信モード

static bool loraReadConfig(uint8_t *out6) {
  unsigned long drainStart = millis();
  while (loraSerial.available()) {
    loraSerial.read();
    if (millis() - drainStart > 300UL) break;
  }
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

static void loraPrintRegs(const char* label, const uint8_t regs[LORA_CFG_REG_LEN]) {
  Serial.print(F("[LORA] ")); Serial.print(label); Serial.print(F(": "));
  for (int i = 0; i < LORA_CFG_REG_LEN; i++) {
    if (regs[i] < 0x10) Serial.print('0');
    Serial.print(regs[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
}

static bool loraCheckAndConfigure() {
  if (!loraModeConfig()) return false;

  uint8_t cur[LORA_CFG_REG_LEN] = {0};
  bool readOk = loraReadConfig(cur);
  bool matches = readOk &&
      cur[0] == LORA_CFG_ADDH && cur[1] == LORA_CFG_ADDL &&
      cur[2] == LORA_CFG_REG0 && cur[3] == LORA_CFG_REG1 &&
      cur[4] == LORA_CFG_REG2 && cur[5] == LORA_CFG_REG3;

  Serial.print(F("[LORA] config read "));
  Serial.println(!readOk ? F("失敗") : (matches ? F("一致") : F("不一致→書込")));
  if (readOk) {
    loraPrintRegs("実測値(読込)", cur);
    uint8_t expected[LORA_CFG_REG_LEN] = {LORA_CFG_ADDH, LORA_CFG_ADDL, LORA_CFG_REG0,
                                           LORA_CFG_REG1, LORA_CFG_REG2, LORA_CFG_REG3};
    loraPrintRegs("期待値      ", expected);
  }

  if (!readOk) { loraModeNormal(); return false; }

  if (!matches) {
    bool verifyOk = false;
    for (int attempt = 1; attempt <= 2 && !verifyOk; attempt++) {
      loraWriteConfig();
      uint8_t verify[LORA_CFG_REG_LEN] = {0};
      bool verifyReadOk = loraReadConfig(verify);
      verifyOk = verifyReadOk &&
          verify[0] == LORA_CFG_ADDH && verify[1] == LORA_CFG_ADDL &&
          verify[2] == LORA_CFG_REG0 && verify[3] == LORA_CFG_REG1 &&
          verify[4] == LORA_CFG_REG2 && verify[5] == LORA_CFG_REG3;

      Serial.print(F("[LORA] config write 確認("));
      Serial.print(attempt); Serial.print(F("/2): "));
      Serial.println(verifyOk ? F("OK") : F("NG"));
      if (verifyReadOk) loraPrintRegs("書込後の実測値", verify);
      else              Serial.println(F("[LORA] 書込後の読込自体に失敗（応答なし）"));
    }
    if (!verifyOk) {
      Serial.println(F("[LORA] 2回とも書込確認NG（配線・電源を確認）"));
      loraModeNormal();
      return false;
    }
  }

  return loraModeNormal();
}

// ── 受信フレーム組み立て（状態機械） ─────────────────
#define LORA_FIELD_TIMEOUT_MS 500UL

enum LoraRxState { LORA_WAIT_SYNC, LORA_WAIT_LEN, LORA_WAIT_BODY, LORA_WAIT_CKSUM, LORA_WAIT_RSSI };
static LoraRxState s_loraState = LORA_WAIT_SYNC;
static uint8_t     s_loraLen = 0;
static uint8_t     s_loraBody[MAX_PAYLOAD];
static uint8_t     s_loraBodyIdx = 0;
static uint8_t     s_loraSum = 0;
static uint8_t     s_loraRssiRaw = 0;
static uint32_t    s_loraFieldStartMs = 0;

static uint32_t s_loraRxBytes  = 0;  // UARTE1から読んだ生バイトの累計
static uint32_t s_loraCksumNg  = 0;  // チェックサム不一致で破棄したフレーム数
static uint32_t s_loraFramesOk = 0;  // 受信成功フレーム数
static int      s_lastRssiDbm  = 0;  // 直近のRSSI
static int      s_minRssiDbm   = 0;  // 起動後の最小RSSI（弱い方）
static int      s_maxRssiDbm   = -9999;  // 起動後の最大RSSI（強い方）

#define LORA_RX_STALL_MS 30000UL
static uint32_t s_loraLastRxMs = 0;
static uint32_t s_loraRekicks  = 0;

static void loraKickTx() {
  const uint8_t dummy[3] = {0x00, 0x00, 0x00};
  loraSerial.write(dummy, sizeof(dummy));
  loraSerial.flush();
  delay(200);
  while (loraSerial.available()) loraSerial.read();
}

// UARTE1のエラー要因を回収し、受信が止まっていれば受信を再起動する
// （バッテリー給電時等にE220が受信不能ラッチする既知の症状への対策。
//   gateway_v1.1で実機確認済みのロジックをそのまま流用）
static void loraRxWatchdog() {
  uint32_t errsrc = NRF_UARTE1->ERRORSRC;
  if (errsrc) NRF_UARTE1->ERRORSRC = errsrc;
  if (NRF_UARTE1->EVENTS_ERROR) NRF_UARTE1->EVENTS_ERROR = 0;

  if (millis() - s_loraLastRxMs >= LORA_RX_STALL_MS) {
    s_loraLastRxMs = millis();
    s_loraRekicks++;
    NRF_UARTE1->TASKS_STARTRX = 1;
    Serial.print(F("[LORA] 受信ストール検出 → RX再起動 #"));
    Serial.println(s_loraRekicks);
    loraModeNormal();
    loraKickTx();
  }
}

static bool loraFeedByte(uint8_t b) {
  switch (s_loraState) {
    case LORA_WAIT_SYNC:
      if (b == 0xAA) { s_loraSum = b; s_loraState = LORA_WAIT_LEN; s_loraFieldStartMs = millis(); }
      return false;
    case LORA_WAIT_LEN:
      s_loraLen = b;
      s_loraSum = (uint8_t)(s_loraSum + b);
      s_loraBodyIdx = 0;
      if (s_loraLen == 0 || s_loraLen > MAX_PAYLOAD) { s_loraState = LORA_WAIT_SYNC; return false; }
      s_loraState = LORA_WAIT_BODY;
      return false;
    case LORA_WAIT_BODY:
      s_loraBody[s_loraBodyIdx++] = b;
      s_loraSum = (uint8_t)(s_loraSum + b);
      if (s_loraBodyIdx >= s_loraLen) s_loraState = LORA_WAIT_CKSUM;
      return false;
    case LORA_WAIT_CKSUM:
      if (b != s_loraSum) { s_loraCksumNg++; s_loraState = LORA_WAIT_SYNC; return false; }
      s_loraState = LORA_WAIT_RSSI;
      return false;
    case LORA_WAIT_RSSI:
      s_loraRssiRaw = b;
      s_loraState = LORA_WAIT_SYNC;
      return true;
    default:
      s_loraState = LORA_WAIT_SYNC;
      return false;
  }
}

static void loraPoll() {
  while (loraSerial.available()) {
    uint8_t b = (uint8_t)loraSerial.read();
    s_loraRxBytes++;
    s_loraLastRxMs = millis();
    if (loraFeedByte(b)) {
      uint8_t deviceId = s_loraBody[1];  // MSDペイロード [0]PktType [1]DeviceID ...
      if (deviceId != TARGET_DEVICE_ID) continue;  // 対象外のDeviceIDは棄却（統計にも含めない）
      s_loraFramesOk++;
      int rssiDbm = (int)s_loraRssiRaw - 256;
      s_lastRssiDbm = rssiDbm;
      if (rssiDbm < s_minRssiDbm) s_minRssiDbm = rssiDbm;
      if (rssiDbm > s_maxRssiDbm) s_maxRssiDbm = rssiDbm;

      Serial.print(F("[RX] #"));
      Serial.print(s_loraFramesOk);
      Serial.print(F("  DeviceID=0x"));
      if (deviceId < 0x10) Serial.print('0');
      Serial.print(deviceId, HEX);
      Serial.print(F("  RSSI="));
      Serial.print(rssiDbm);
      Serial.println(F("dBm"));
    }
  }

  if (s_loraState != LORA_WAIT_SYNC && millis() - s_loraFieldStartMs > LORA_FIELD_TIMEOUT_MS) {
    Serial.println(F("[LORA] フレーム途中でタイムアウト。再同期します"));
    s_loraState = LORA_WAIT_SYNC;
  }

  loraRxWatchdog();
}

static uint32_t s_lastStatsMs = 0;
#define STATS_INTERVAL_MS 10000UL

static void printStats() {
  Serial.print(F("[STATS] 受信フレーム数=")); Serial.print(s_loraFramesOk);
  Serial.print(F(" CKSUM_NG=")); Serial.print(s_loraCksumNg);
  Serial.print(F(" 直近RSSI="));
  if (s_loraFramesOk == 0) {
    Serial.print(F("(未受信)"));
  } else {
    Serial.print(s_lastRssiDbm); Serial.print(F("dBm"));
    Serial.print(F(" 最良=")); Serial.print(s_maxRssiDbm); Serial.print(F("dBm"));
    Serial.print(F(" 最弱=")); Serial.print(s_minRssiDbm); Serial.print(F("dBm"));
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Serial.println(F("\n[Step19] LoRa親機（受信側）— Gateway ver1.1 電波強度連続受信テスト"));

  pinMode(LORA_M0M1_PIN, OUTPUT);
  loraSerial.begin(9600);  // E220-900T22S(JP) デフォルト
  delay(500);              // E220 起動待ち

  if (loraCheckAndConfigure()) {
    Serial.println(F("[LORA] 設定確認OK。受信待機を開始します"));
  } else {
    Serial.println(F("[LORA] ✗ 設定確認に失敗（配線・電源を確認してください）。"
                      "それでも受信待機は継続します"));
  }

  s_loraLastRxMs = millis();
  s_lastStatsMs = millis();
}

#if TX_PING_ENABLED
// Flex側(18_lora_child, RX_DUMP_ENABLED=1)で [RX raw] として見えるはずの
// 分かりやすいマーカー列を送る。フレーム形式は問わない（届いたかどうかだけ見る）。
static uint32_t s_lastPingMs = 0;
static uint32_t s_pingCount  = 0;

static void sendPing() {
  s_pingCount++;
  const uint8_t ping[8] = {0xAA, 0x06, 'P', 'I', 'N', 'G', 0x00, 0x55};

  loraModeNormal();  // 念のためNormal（透過送受信）モードを確定させる
  loraSerial.write(ping, sizeof(ping));
  loraSerial.flush();

  Serial.print(F("[TX PING] #"));
  Serial.print(s_pingCount);
  Serial.println(F("  (Flex側に [RX raw] が出れば送信成功)"));
}
#endif

void loop() {
  loraPoll();

  uint32_t now = millis();

#if TX_PING_ENABLED
  if (now - s_lastPingMs >= TX_PING_INTERVAL_MS) {
    s_lastPingMs = now;
    sendPing();
  }
#endif

  if (now - s_lastStatsMs >= STATS_INTERVAL_MS) {
    s_lastStatsMs = now;
    printStats();
  }
  yield();
}
