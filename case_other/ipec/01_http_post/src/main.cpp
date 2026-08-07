/**
 * case_other/ipec/01_http_post — LoRa受信値 → iPECサーバー HTTPS POST 実機テスト
 *
 * 【目的】
 *   実運用に近い構成での疎通テスト:
 *     Flex ver3.10（子機、LoRa送信）→ Gateway ver1.1（親機、本コード）
 *       → SIM7080G(LTE-M) → iPECサーバーへ HTTPS POST
 *   これまでのダミーJSON連続送信テストに代えて、LoRaで実際に受信した
 *   フレーム（DeviceID・CH1-4・電池電圧）をJSONへ詰めて送信する。
 *
 * 【対象ハード】Gateway ver1.1 基板
 *   LoRa (E220-900T22S(JP))  : XIAO D0(RX)/D1(TX)/D2(M0M1)、UARTE1使用
 *                              → 19_lora_parent と同一ロジックを流用
 *   LTE-M (SIM7080G)         : XIAO D6(TX)/D7(RX)、Serial1(UARTE0)使用
 *                              → 既存 01_http_post のロジックをそのまま流用
 *
 * 【対の子機】case01_Flex/test_sketches/18_lora_child（Flex ver3.10実機）
 *   DEVICE_ID を本コードの TARGET_DEVICE_ID と一致させておくこと。
 *
 * 【動作】
 *   1. 起動時にLoRa設定確認・LTE-M接続を行う
 *   2. TARGET_DEVICE_ID のLoRaフレームを受信するたびに、そのCH1-4・電池電圧を
 *      JSONに詰めてiPECへPOSTする（最大 TOTAL_SEND 回。以降は受信のみ継続表示）
 *
 * iPEC エンドポイント:
 *   URL        : https://jg9v8fcum8.execute-api.ap-northeast-1.amazonaws.com/dev
 *   Method     : POST
 *   Content-Type: application/json
 *   x-api-key  : Z5lcAxUM8CaJwUOpQW1YW8th8MVkSB7l7BoKgyM6
 *   方式       : raw TCP + SSL（AT+CAOPEN / AT+CASEND）
 *     → SHHTTPクライアント（AT+SHBOD）はJSON内のダブルクォートに対応できないため、
 *       生HTTPリクエストを直接送信する方式を採用（既存実装から変更なし）
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>  // USB CDC（Serial）。USE_TINYUSBビルド時に必要

// ══════════════════════════════════════════════
// ▼ LoRa（E220）ピン割当（Gateway ver1.1 基板。19_lora_parentと同一）
// ══════════════════════════════════════════════
static int const LORA_RX_PIN   = 0;  // D0: E220 TXD → XIAO RX（net UART_RX_2）
static int const LORA_TX_PIN   = 1;  // D1: XIAO TX → E220 RXD（net UART_TX_2）
static int const LORA_M0M1_PIN = 2;  // D2: E220 M0・M1 共通駆動

#define LORA_MODE_SWITCH_DELAY_MS 100U
#define MAX_PAYLOAD 24

// このDeviceIDのフレームのみ処理する（対の子機 18_lora_child の DEVICE_ID と一致させること）
static const uint8_t TARGET_DEVICE_ID = 0x0E;

// UARTE1（第2ハードウェアUART）をLoRa RX専用として使う。gateway_v1.1と同じ構成。
static Uart loraSerial(NRF_UARTE1, UARTE1_IRQn, LORA_RX_PIN, LORA_TX_PIN);

// ★UARTE1を自前で使う場合、割り込みハンドラをこのように手動で転送しないと
//   send/receive の完了通知が届かず、write()が2バイト目以降で永久にブロックする
//   （gateway_v1.1で実機確認済みの既知の罠）。
extern "C" void UARTE1_IRQHandler(void) {
  loraSerial.IrqHandler();
}

// ══════════════════════════════════════════════
// ▼ LTE-M（SIM7080G）ピン割当（Gateway ver1.1 基板。既存01_http_postと同一）
//   Serial1（UARTE0）を使用。LoRa用のloraSerial(UARTE1)とは別系統。
// ══════════════════════════════════════════════
static const uint8_t LTE_TX_PIN = 6;
static const uint8_t LTE_RX_PIN = 7;

// ══════════════════════════════════════════════
// ▼ iPEC 設定
// ══════════════════════════════════════════════
const char* IPEC_HOST    = "jg9v8fcum8.execute-api.ap-northeast-1.amazonaws.com";
const char* IPEC_PATH    = "/dev";
const char* IPEC_API_KEY = "Z5lcAxUM8CaJwUOpQW1YW8th8MVkSB7l7BoKgyM6";
const char* DEVICE_ID    = "monita-flex-001";  // iPEC側で事前登録済みの固定デバイスID
const char* APN          = "iot.1nce.net";

static const int TOTAL_SEND = 5;  // この回数だけLoRa受信フレームをPOSTしたら試験終了

// ══════════════════════════════════════════════
// LoRa設定コマンド（gateway_v1.1・Flex側 v3.10_lora/main.cpp と同一値。全台共通）
// ══════════════════════════════════════════════
#define LORA_CFG_REG_START 0x00
#define LORA_CFG_REG_LEN   6
static const uint8_t LORA_CFG_ADDH = 0x00;
static const uint8_t LORA_CFG_ADDL = 0x00;
static const uint8_t LORA_CFG_REG0 = 0x68;  // UART9600bps + エア速度(SF7/BW125kHz)
static const uint8_t LORA_CFG_REG1 = 0x01;  // 送信出力13dBm
static const uint8_t LORA_CFG_REG2 = 0x00;  // チャンネル0
static const uint8_t LORA_CFG_REG3 = 0x80;  // RSSIバイト有効化ON/透過送信モード

static bool loraSetMode(bool high) {
  digitalWrite(LORA_M0M1_PIN, high ? HIGH : LOW);
  delay(LORA_MODE_SWITCH_DELAY_MS);
  return true;
}
static inline bool loraModeNormal() { return loraSetMode(false); }
static inline bool loraModeConfig() { return loraSetMode(true); }

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

  if (!readOk) { loraModeNormal(); return false; }

  if (!matches) {
    bool verifyOk = false;
    for (int attempt = 1; attempt <= 2 && !verifyOk; attempt++) {
      loraWriteConfig();
      uint8_t verify[LORA_CFG_REG_LEN] = {0};
      verifyOk = loraReadConfig(verify) &&
          verify[0] == LORA_CFG_ADDH && verify[1] == LORA_CFG_ADDL &&
          verify[2] == LORA_CFG_REG0 && verify[3] == LORA_CFG_REG1 &&
          verify[4] == LORA_CFG_REG2 && verify[5] == LORA_CFG_REG3;
      Serial.print(F("[LORA] config write 確認("));
      Serial.print(attempt); Serial.print(F("/2): "));
      Serial.println(verifyOk ? F("OK") : F("NG"));
    }
    if (!verifyOk) {
      Serial.println(F("[LORA] 2回とも書込確認NG（配線・電源を確認）"));
      loraModeNormal();
      return false;
    }
  }

  return loraModeNormal();
}

// ══════════════════════════════════════════════
// LoRa受信フレーム組み立て（状態機械。19_lora_parentと同一）
//
// フレーム形式: [0]SYNC=0xAA [1]LEN [2..LEN+1]MSDペイロード [LEN+2]チェックサム [+1]RSSI
// MSDペイロード（18_lora_childが送る19バイト固定レイアウト）:
//   [0]PktType [1]DeviceID [2]FWVersion [3-10]CH1-4(int16 LE)
//   [11-12]BATT(mV) [13]Hour [14]Min [15-18]CH1-4 Range
// ══════════════════════════════════════════════
enum LoraRxState { LORA_WAIT_SYNC, LORA_WAIT_LEN, LORA_WAIT_BODY, LORA_WAIT_CKSUM, LORA_WAIT_RSSI };
static LoraRxState s_loraState = LORA_WAIT_SYNC;
static uint8_t     s_loraLen = 0;
static uint8_t     s_loraBody[MAX_PAYLOAD];
static uint8_t     s_loraBodyIdx = 0;
static uint8_t     s_loraSum = 0;
static uint8_t     s_loraRssiRaw = 0;
static uint32_t    s_loraFieldStartMs = 0;

#define LORA_FIELD_TIMEOUT_MS 500UL
#define LORA_RX_STALL_MS 30000UL
static uint32_t s_loraLastRxMs = 0;
static uint32_t s_loraCksumNg  = 0;
static uint32_t s_loraFramesOk = 0;

// 直近受信フレームの内容（loop側でPOSTするために保持）
static bool     s_newFrameReady = false;
static int16_t  s_rxCh[4];
static uint16_t s_rxBatt;
static uint8_t  s_rxHour, s_rxMin;
static int      s_rxRssiDbm;

static void loraKickTx() {
  const uint8_t dummy[3] = {0x00, 0x00, 0x00};
  loraSerial.write(dummy, sizeof(dummy));
  loraSerial.flush();
  delay(200);
  while (loraSerial.available()) loraSerial.read();
}

// UARTE1のエラー要因を回収し、受信が止まっていれば受信を再起動する
// （gateway_v1.1で実機確認済みのロジックをそのまま流用）
static void loraRxWatchdog() {
  uint32_t errsrc = NRF_UARTE1->ERRORSRC;
  if (errsrc) NRF_UARTE1->ERRORSRC = errsrc;
  if (NRF_UARTE1->EVENTS_ERROR) NRF_UARTE1->EVENTS_ERROR = 0;

  if (millis() - s_loraLastRxMs >= LORA_RX_STALL_MS) {
    s_loraLastRxMs = millis();
    NRF_UARTE1->TASKS_STARTRX = 1;
    Serial.println(F("[LORA] 受信ストール検出 → RX再起動"));
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

// 受信バイトを処理し、TARGET_DEVICE_IDのフレームが1つ完成したら
// s_rxCh等へ展開してs_newFrameReadyを立てる（既にPOST待ちのフレームがあれば上書きしない）
static void loraPoll() {
  while (loraSerial.available()) {
    uint8_t b = (uint8_t)loraSerial.read();
    s_loraLastRxMs = millis();
    if (loraFeedByte(b)) {
      uint8_t deviceId = s_loraBody[1];
      if (deviceId != TARGET_DEVICE_ID) continue;

      s_loraFramesOk++;
      int rssiDbm = (int)s_loraRssiRaw - 256;

      Serial.print(F("[LORA RX] #")); Serial.print(s_loraFramesOk);
      Serial.print(F(" DeviceID=0x")); Serial.print(deviceId, HEX);
      Serial.print(F(" RSSI=")); Serial.print(rssiDbm); Serial.println(F("dBm"));

      if (s_newFrameReady) {
        Serial.println(F("[LORA RX] 前回フレームがPOST待ちのため今回分は破棄"));
        continue;
      }

      for (int i = 0; i < 4; i++) {
        s_rxCh[i] = (int16_t)((uint16_t)s_loraBody[3 + i * 2] |
                               ((uint16_t)s_loraBody[3 + i * 2 + 1] << 8));
      }
      s_rxBatt = (uint16_t)s_loraBody[11] | ((uint16_t)s_loraBody[12] << 8);
      s_rxHour = s_loraBody[13];
      s_rxMin  = s_loraBody[14];
      s_rxRssiDbm = rssiDbm;
      s_newFrameReady = true;
    }
  }

  if (s_loraState != LORA_WAIT_SYNC && millis() - s_loraFieldStartMs > LORA_FIELD_TIMEOUT_MS) {
    Serial.println(F("[LORA] フレーム途中でタイムアウト。再同期します"));
    s_loraState = LORA_WAIT_SYNC;
  }

  loraRxWatchdog();
}

// ══════════════════════════════════════════════
// LTE-M（SIM7080G）AT コマンド送受信（既存01_http_postから変更なし）
// ══════════════════════════════════════════════
static String sendAT(const String& cmd, int waitMs = 5000) {
  Serial.print(F(">> ")); Serial.println(cmd);
  Serial1.print(cmd + "\r\n");
  long start = millis();
  String res = "";
  while (millis() - start < waitMs) {
    while (Serial1.available()) res += (char)Serial1.read();
    yield();
  }
  if (res.length() > 0) Serial.println(res);
  else                   Serial.println(F("(応答なし)"));
  return res;
}

static bool probeAT() {
  Serial.print(F("AT 疎通確認"));
  for (int t = 0; t < 20; t++) {
    Serial.print('.');
    Serial1.print("AT\r\n");
    String r = "";
    unsigned long s = millis();
    while (millis() - s < 500) {
      while (Serial1.available()) r += (char)Serial1.read();
      yield();
    }
    if (r.indexOf("OK") >= 0) {
      Serial.println(F(" OK ✓"));
      while (Serial1.available()) Serial1.read();
      return true;
    }
    while (Serial1.available()) Serial1.read();
    delay(500);
  }
  Serial.println(F(" 失敗 ✗"));
  return false;
}

static bool lteConnect() {
  sendAT("ATE0", 2000);
  sendAT("AT+CPIN?", 3000);
  sendAT("AT+CNMP=38", 2000);
  sendAT("AT+CMNB=1",  2000);
  sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\"", 2000);

  Serial.println(F("ネットワーク登録待ち（最大60秒）"));
  bool registered = false;
  for (int i = 0; i < 12; i++) {
    String r = sendAT("AT+CREG?", 3000);
    if (r.indexOf("0,1") >= 0 || r.indexOf("0,5") >= 0) { registered = true; break; }
    String att = sendAT("AT+CGATT?", 2000);
    if (att.indexOf("+CGATT: 1") >= 0) { registered = true; break; }
    delay(5000);
  }
  if (!registered) { Serial.println(F("✗ 登録失敗")); return false; }
  Serial.println(F("✓ ネットワーク登録 OK"));

  sendAT("AT+CNACT=0,1", 15000);
  delay(3000);
  String ip = sendAT("AT+CNACT?", 3000);
  if (ip.indexOf("0,1") < 0) { Serial.println(F("✗ IP 取得失敗")); return false; }

  sendAT("AT+CSQ", 3000);
  sendAT("AT+COPS?", 3000);
  Serial.println(F("✓ LTE-M 接続完了"));
  return true;
}

// ══════════════════════════════════════════════
// JSON ボディ生成（LoRaで受信した実測値を使用）
//
// キー名はiPEC側スキーマに合わせて自由に調整可（README参照）。
// ══════════════════════════════════════════════
static String buildJsonFromLora(int count) {
  unsigned long t = 1716000000UL + (unsigned long)count * 60;

  String json = "{";
  json += "\"device\":\""; json += DEVICE_ID;          json += "\",";
  json += "\"time\":";     json += t;                  json += ",";
  json += "\"ch1\":";             json += s_rxCh[0];        json += ",";
  json += "\"ch2\":";             json += s_rxCh[1];        json += ",";
  json += "\"ch3\":";             json += s_rxCh[2];        json += ",";
  json += "\"ch4\":";             json += s_rxCh[3];        json += ",";
  json += "\"batt\":";            json += s_rxBatt;         json += ",";
  json += "\"rssi\":";            json += s_rxRssiDbm;
  json += "}";
  return json;
}

// ══════════════════════════════════════════════
// iPEC HTTPS POST（raw TCP + SSL 方式。既存01_http_postから変更なし）
// ══════════════════════════════════════════════
static bool postToIpec(const String& json) {
  Serial.println("  JSON: " + json);

  sendAT("AT+CACLOSE=0", 2000);
  delay(300);

  sendAT("AT+CSSLCFG=\"sslversion\",0,3", 2000);       // TLS 1.2
  delay(200);
  sendAT("AT+CSSLCFG=\"ignorertctime\",0,1", 2000);     // 証明書時刻無視
  delay(200);
  sendAT("AT+CASSLCFG=0,\"ssl\",1", 2000);              // 接続0: SSL 有効
  delay(200);

  String openCmd = "AT+CAOPEN=0,0,\"TCP\",\"";
  openCmd += IPEC_HOST;
  openCmd += "\",443";
  Serial.println(F("  SSL TCP 接続中..."));
  String openResp = sendAT(openCmd, 30000);
  if (openResp.indexOf("OK") < 0) {
    Serial.println(F("  ✗ 接続失敗"));
    sendAT("AT+CACLOSE=0", 2000);
    return false;
  }
  Serial.println(F("  接続 OK ✓"));
  delay(500);

  int bodyLen = json.length();
  String httpReq = "POST ";
  httpReq += IPEC_PATH;
  httpReq += " HTTP/1.1\r\n";
  httpReq += "Host: ";        httpReq += IPEC_HOST;   httpReq += "\r\n";
  httpReq += "Content-Type: application/json\r\n";
  httpReq += "x-api-key: ";  httpReq += IPEC_API_KEY; httpReq += "\r\n";
  httpReq += "Content-Length: "; httpReq += bodyLen;  httpReq += "\r\n";
  httpReq += "Connection: close\r\n";
  httpReq += "\r\n";
  httpReq += json;

  int totalLen = httpReq.length();
  Serial.print(F("  送信バイト数: ")); Serial.println(totalLen);

  String casendCmd = "AT+CASEND=0," + String(totalLen);
  Serial.print(F(">> ")); Serial.println(casendCmd);
  Serial1.print(casendCmd + "\r\n");

  bool gotPrompt = false;
  unsigned long s = millis();
  while (millis() - s < 5000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      if (c == '>') { gotPrompt = true; break; }
    }
    if (gotPrompt) break;
    yield();
  }

  if (!gotPrompt) {
    Serial.println(F("  ✗ > プロンプト待ちタイムアウト"));
    sendAT("AT+CACLOSE=0", 2000);
    return false;
  }
  Serial.println(F("  > 受信 → HTTP リクエスト送信"));
  delay(50);
  Serial1.print(httpReq);

  String rawResp = "";
  s = millis();
  while (millis() - s < 15000) {
    while (Serial1.available()) rawResp += (char)Serial1.read();
    if (rawResp.indexOf("SEND OK") >= 0) break;
    yield();
  }
  Serial.print(F("  CASEND 応答: ")); Serial.println(rawResp);

  String recvNotif = "";
  s = millis();
  while (millis() - s < 10000) {
    while (Serial1.available()) recvNotif += (char)Serial1.read();
    if (recvNotif.indexOf("+CARECV") >= 0) break;
    yield();
  }

  String carecv = sendAT("AT+CARECV=0,1024", 5000);
  String allResp = rawResp + recvNotif + carecv;
  Serial.print(F("  レスポンス全文: ")); Serial.println(allResp);

  sendAT("AT+CACLOSE=0", 3000);

  int httpIdx = allResp.indexOf("HTTP/1.");
  if (httpIdx >= 0) {
    String statusStr = allResp.substring(httpIdx + 9, httpIdx + 12);
    int statusCode = statusStr.toInt();
    Serial.print(F("  HTTP ステータス: ")); Serial.println(statusCode);
    return (statusCode >= 200 && statusCode < 300);
  }
  return false;
}

// ══════════════════════════════════════════════
// Arduino エントリ
// ══════════════════════════════════════════════
static int s_okCount = 0;
static int s_sendCount = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) yield();
  delay(500);

  Serial.println(F("\n╔══════════════════════════════════════════════╗"));
  Serial.println(F("║  LoRa受信値 → iPEC HTTPS POST 実機テスト      ║"));
  Serial.println(F("║  Flex v3.10 → Gateway v1.1 → iPEC             ║"));
  Serial.println(F("╚══════════════════════════════════════════════╝"));
  Serial.print(F("エンドポイント: https://"));
  Serial.print(IPEC_HOST); Serial.println(IPEC_PATH);
  Serial.print(F("対象DeviceID: 0x")); Serial.println(TARGET_DEVICE_ID, HEX);
  Serial.print(F("最大送信回数: ")); Serial.println(TOTAL_SEND);

  // ── LoRa初期化 ──
  Serial.println(F("\n--- LoRa 初期化 ---"));
  pinMode(LORA_M0M1_PIN, OUTPUT);
  loraSerial.begin(9600);
  delay(500);
  if (loraCheckAndConfigure()) {
    Serial.println(F("[LORA] 設定確認OK。受信待機を開始します"));
  } else {
    Serial.println(F("[LORA] ✗ 設定確認に失敗（配線・電源を確認してください）。"
                      "それでも受信待機は継続します"));
  }
  s_loraLastRxMs = millis();

  // ── LTE-M初期化 ──
  Serial.println(F("\n--- LTE-M 初期化 ---"));
  Serial1.setPins(LTE_RX_PIN, LTE_TX_PIN);
  Serial1.begin(115200);

  Serial.print(F("SIM7080G 起動待ち（15秒）"));
  for (int i = 0; i < 15; i++) {
    delay(1000); Serial.print('.');
    while (Serial1.available()) Serial.write(Serial1.read());
  }
  Serial.println();

  if (!probeAT()) {
    Serial.println(F("[ERROR] SIM7080G が応答しません"));
    return;
  }

  Serial.println(F("\n--- LTE-M 接続 ---"));
  if (!lteConnect()) {
    Serial.println(F("[ERROR] LTE-M 接続失敗"));
    return;
  }

  Serial.println(F("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
  Serial.println(F("  LoRaフレーム受信待機中...（受信次第POST開始）"));
  Serial.println(F("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
}

void loop() {
  loraPoll();

  if (s_newFrameReady && s_sendCount < TOTAL_SEND) {
    s_newFrameReady = false;
    s_sendCount++;

    Serial.print(F("\n【送信 ")); Serial.print(s_sendCount);
    Serial.print(F("/")); Serial.print(TOTAL_SEND); Serial.println(F("】"));

    bool ok = postToIpec(buildJsonFromLora(s_sendCount));
    if (ok) { s_okCount++; Serial.println(F("  ★ 成功 ✓")); }
    else     { Serial.println(F("  ✗ 失敗")); }

    if (s_sendCount == TOTAL_SEND) {
      Serial.println(F("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
      Serial.println(F("  【結果サマリー】"));
      Serial.print(F("  成功: ")); Serial.print(s_okCount);
      Serial.print(F(" / ")); Serial.print(TOTAL_SEND); Serial.println(F(" 回"));
      if      (s_okCount == TOTAL_SEND) Serial.println(F("  ✅ 全送信成功！"));
      else if (s_okCount > 0)           Serial.println(F("  ⚠ 一部成功"));
      else                              Serial.println(F("  ❌ 全送信失敗"));
      Serial.println(F("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
      Serial.println(F("\n[完了] 試験終了。LoRa受信ログとATコマンド手動送信は継続可"));
    }
  }

  // AT コマンド手動送信パススルー（デバッグ用。既存01_http_postから変更なし）
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) sendAT(line);
  }
  while (Serial1.available()) Serial.write(Serial1.read());

  yield();
}
