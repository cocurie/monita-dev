/**
 * case_other/ipec — iPEC サーバー HTTPS POST 検証（5回送信テスト）
 *
 * 配線（ブレッドボード）:
 *   XIAO D6 (TX) → SIM7080G RX
 *   XIAO D7 (RX) ← SIM7080G TX
 *   XIAO 5V      → SIM7080G 5V
 *   XIAO GND     → SIM7080G GND
 *
 * 方式: raw TCP + SSL（AT+CAOPEN / AT+CASEND）
 *   → SHHTTP クライアント（AT+SHBOD）はJSON内のダブルクォートに
 *     対応できないため、生 HTTP リクエストを直接送信する方式を採用
 *
 * iPEC エンドポイント:
 *   URL        : https://jg9v8fcum8.execute-api.ap-northeast-1.amazonaws.com/dev
 *   Method     : POST
 *   Content-Type: application/json
 *   x-api-key  : Z5lcAxUM8CaJwUOpQW1YW8th8MVkSB7l7BoKgyM6
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

// ══════════════════════════════════════════════
// ▼ ピン定義（ブレッドボード D6/D7）
// ══════════════════════════════════════════════
static const uint8_t LTE_TX_PIN = 6;
static const uint8_t LTE_RX_PIN = 7;

// ══════════════════════════════════════════════
// ▼ iPEC 設定
// ══════════════════════════════════════════════
const char* IPEC_HOST    = "jg9v8fcum8.execute-api.ap-northeast-1.amazonaws.com";
const char* IPEC_PATH    = "/dev";
const char* IPEC_API_KEY = "Z5lcAxUM8CaJwUOpQW1YW8th8MVkSB7l7BoKgyM6";
const char* DEVICE_ID    = "monita-flex-001";
const char* APN          = "iot.1nce.net";

static const int TOTAL_SEND = 5;

// ══════════════════════════════════════════════
// AT コマンド送受信（yield付き）
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

// ══════════════════════════════════════════════
// AT 疎通確認
// ══════════════════════════════════════════════
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

// ══════════════════════════════════════════════
// LTE-M 接続
// ══════════════════════════════════════════════
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
    if (r.indexOf("0,1") >= 0 || r.indexOf("0,5") >= 0) {
      registered = true; break;
    }
    String att = sendAT("AT+CGATT?", 2000);
    if (att.indexOf("+CGATT: 1") >= 0) {
      registered = true; break;
    }
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
// JSON ボディ生成（ダミー計測値）
// ══════════════════════════════════════════════
static String buildJson(int count) {
  unsigned long t = 1716000000UL + (unsigned long)count * 60;
  String json = "{";
  json += "\"device\":\""; json += DEVICE_ID;       json += "\",";
  json += "\"time\":";     json += t;               json += ",";
  json += "\"ch1\":";      json += (245 + count);   json += ",";
  json += "\"ch2\":";      json += (252 + count);   json += ",";
  json += "\"ch3\":255,";
  json += "\"ch4\":0,";
  json += "\"temp\":185,";
  json += "\"batt\":3000";
  json += "}";
  return json;
}

// ══════════════════════════════════════════════
// iPEC HTTPS POST（raw TCP + SSL 方式）
//
// AT+SHBOD（SHHTTP クライアント）はJSON内の
// ダブルクォートを扱えないため、CAOPEN で
// 生 HTTP リクエストを直接送信する
// ══════════════════════════════════════════════
static bool postToIpec(const String& json) {
  Serial.println("  JSON: " + json);

  // ── 既存接続クローズ ──
  sendAT("AT+CACLOSE=0", 2000);
  delay(300);

  // ── SSL 設定（コンテキスト 0）──
  sendAT("AT+CSSLCFG=\"sslversion\",0,3", 2000);       // TLS 1.2
  delay(200);
  sendAT("AT+CSSLCFG=\"ignorertctime\",0,1", 2000);     // 証明書時刻無視
  delay(200);
  sendAT("AT+CASSLCFG=0,\"ssl\",0", 2000);              // 接続0 ← SSLコンテキスト0
  delay(200);

  // ── SSL TCP 接続（port 443）──
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

  // ── raw HTTP POST リクエスト組み立て ──
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

  // ── AT+CASEND で > プロンプトを待ってから送信 ──
  String casendCmd = "AT+CASEND=0," + String(totalLen);
  Serial.print(F(">> ")); Serial.println(casendCmd);
  Serial1.print(casendCmd + "\r\n");

  // '>' プロンプトを待つ（最大5秒）
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

  // ── SEND OK + レスポンス受信 ──
  String rawResp = "";
  s = millis();
  while (millis() - s < 15000) {
    while (Serial1.available()) rawResp += (char)Serial1.read();
    if (rawResp.indexOf("SEND OK") >= 0) break;
    yield();
  }
  Serial.print(F("  CASEND 応答: ")); Serial.println(rawResp);

  // +CARECV 通知を待つ
  String recvNotif = "";
  s = millis();
  while (millis() - s < 10000) {
    while (Serial1.available()) recvNotif += (char)Serial1.read();
    if (recvNotif.indexOf("+CARECV") >= 0) break;
    yield();
  }

  // HTTP レスポンス読み取り
  String carecv = sendAT("AT+CARECV=0,1024", 5000);
  String allResp = rawResp + recvNotif + carecv;
  Serial.print(F("  レスポンス全文: ")); Serial.println(allResp);

  // 接続切断
  sendAT("AT+CACLOSE=0", 3000);

  // HTTP ステータスコード解析
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
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) yield();
  delay(500);

  Serial.println(F("\n╔══════════════════════════════════════╗"));
  Serial.println(F("║  iPEC HTTPS POST 検証（5回送信）     ║"));
  Serial.println(F("╚══════════════════════════════════════╝"));
  Serial.print(F("エンドポイント: https://"));
  Serial.print(IPEC_HOST); Serial.println(IPEC_PATH);
  Serial.println(F("方式: raw TCP + SSL（AT+CAOPEN）\n"));

  Serial1.setPins(LTE_RX_PIN, LTE_TX_PIN);
  Serial1.begin(115200);

  // SIM7080G 起動待ち
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

  // ═══ 5回 POST ═══
  Serial.println(F("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
  Serial.println(F("  POST 開始（5回）"));
  Serial.println(F("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));

  int okCount = 0;
  for (int i = 1; i <= TOTAL_SEND; i++) {
    Serial.print(F("\n【送信 ")); Serial.print(i);
    Serial.print(F("/")); Serial.print(TOTAL_SEND); Serial.println(F("】"));

    bool ok = postToIpec(buildJson(i));
    if (ok) { okCount++; Serial.println(F("  ★ 成功 ✓")); }
    else     { Serial.println(F("  ✗ 失敗")); }

    if (i < TOTAL_SEND) {
      Serial.println(F("  10秒待機..."));
      for (int w = 0; w < 10; w++) { delay(1000); yield(); }
    }
  }

  // ═══ 結果サマリー ═══
  Serial.println(F("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
  Serial.println(F("  【結果サマリー】"));
  Serial.print(F("  成功: ")); Serial.print(okCount);
  Serial.print(F(" / ")); Serial.print(TOTAL_SEND); Serial.println(F(" 回"));
  if      (okCount == TOTAL_SEND) Serial.println(F("  ✅ 全送信成功！"));
  else if (okCount > 0)           Serial.println(F("  ⚠ 一部成功"));
  else                            Serial.println(F("  ❌ 全送信失敗"));
  Serial.println(F("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
  Serial.println(F("\n[完了] テスト終了（ATコマンド手動送信可）"));
}

void loop() {
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) sendAT(line);
  }
  while (Serial1.available()) Serial.write(Serial1.read());
  yield();
}
