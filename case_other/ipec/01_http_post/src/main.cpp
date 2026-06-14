/**
 * case_other/ipec — iPEC サーバー HTTPS POST 検証（5回送信テスト）
 *
 * 配線（ブレッドボード / NEXCO テストと同じ）:
 *   XIAO D6 (TX) → SIM7080G RX
 *   XIAO D7 (RX) ← SIM7080G TX
 *   XIAO 5V      → SIM7080G 5V
 *   XIAO GND     → SIM7080G GND
 *
 * iPEC エンドポイント:
 *   URL        : https://jg9v8fcum8.execute-api.ap-northeast-1.amazonaws.com/dev
 *   Method     : POST
 *   Content-Type: application/json
 *   x-api-key  : Z5lcAxUM8CaJwUOpQW1YW8th8MVkSB7l7BoKgyM6
 *
 * JSON フォーマット（iPEC 仕様）:
 *   {
 *     "device": "monita-flex-001",
 *     "time"  : 1716000000,    // Unix タイムスタンプ（本番は RTC から）
 *     "ch1"   : 245,           // CH1 計測値
 *     "ch2"   : 252,           // CH2 計測値
 *     "ch3"   : 255,           // CH3 計測値
 *     "ch4"   : 0,             // CH4 計測値
 *     "temp"  : 185,           // 基板温度（×10、185 = 18.5℃）
 *     "batt"  : 3000           // 電池電圧（mV）
 *   }
 *
 * 動作:
 *   LTE-M 接続後、5回 POST して結果を表示 → 終了
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

// ══════════════════════════════════════════════
// ▼ ピン定義（ブレッドボード: D6/D7）
// ══════════════════════════════════════════════
static const uint8_t LTE_TX_PIN = 6;   // D6 → SIM7080G RX
static const uint8_t LTE_RX_PIN = 7;   // D7 ← SIM7080G TX

// ══════════════════════════════════════════════
// ▼ iPEC 設定
// ══════════════════════════════════════════════
const char* IPEC_HOST    = "jg9v8fcum8.execute-api.ap-northeast-1.amazonaws.com";
const char* IPEC_PATH    = "/dev";
const char* IPEC_API_KEY = "Z5lcAxUM8CaJwUOpQW1YW8th8MVkSB7l7BoKgyM6";
const char* DEVICE_ID    = "monita-flex-001";

// SIM / APN
const char* APN = "iot.1nce.net";

// 送信回数
static const int TOTAL_SEND = 5;

// ══════════════════════════════════════════════
// AT コマンド送受信
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
// AT 疎通確認（最大20回リトライ）
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
      Serial.println(F("✓ ネットワーク登録 OK"));
      registered = true;
      break;
    }
    String att = sendAT("AT+CGATT?", 2000);
    if (att.indexOf("+CGATT: 1") >= 0) {
      Serial.println(F("✓ Attach 済み"));
      registered = true;
      break;
    }
    delay(5000);
  }
  if (!registered) { Serial.println(F("✗ 登録失敗")); return false; }

  sendAT("AT+CNACT=0,1", 15000);
  delay(3000);
  String ip = sendAT("AT+CNACT?", 3000);
  if (ip.indexOf("0,1") < 0) { Serial.println(F("✗ IP 取得失敗")); return false; }

  // 接続確認用に電波強度表示
  String csq = sendAT("AT+CSQ", 3000);
  String ops = sendAT("AT+COPS?", 3000);
  Serial.println(F("✓ LTE-M 接続完了"));
  return true;
}

// ══════════════════════════════════════════════
// JSON ボディ生成
// ══════════════════════════════════════════════
static String buildJson(int count) {
  // ダミー計測値（本番は HX711/DS3231 等から取得）
  int ch1  = 245 + count;
  int ch2  = 252 + count;
  int ch3  = 255;
  int ch4  = 0;
  int temp = 185;          // 18.5℃（×10）
  int batt = 3000;         // 3000 mV

  // time: Unix タイムスタンプ（本番は DS3231 から。今はカウンタで代用）
  unsigned long t = 1716000000UL + (unsigned long)count * 60;

  String json = "{";
  json += "\"device\":\""; json += DEVICE_ID;  json += "\",";
  json += "\"time\":";     json += t;           json += ",";
  json += "\"ch1\":";      json += ch1;         json += ",";
  json += "\"ch2\":";      json += ch2;         json += ",";
  json += "\"ch3\":";      json += ch3;         json += ",";
  json += "\"ch4\":";      json += ch4;         json += ",";
  json += "\"temp\":";     json += temp;        json += ",";
  json += "\"batt\":";     json += batt;
  json += "}";
  return json;
}

// ══════════════════════════════════════════════
// iPEC サーバーへ HTTPS POST（1回）
// ══════════════════════════════════════════════
static bool postToIpec(const String& json) {
  Serial.println(F("  JSON: ") );
  Serial.println("  " + json);

  int bodyLen = json.length();

  // SSL / 接続設定
  sendAT("AT+SHDISC", 2000);
  delay(300);
  sendAT("AT+CSSLCFG=\"ignorertctime\",1,1", 2000);   delay(200);
  sendAT("AT+CSSLCFG=\"sslversion\",1,3", 2000);       delay(200);
  sendAT("AT+CSSLCFG=\"sni\",1,\"" + String(IPEC_HOST) + "\"", 2000); delay(200);
  sendAT("AT+SHSSL=1,\"\"", 2000);                     delay(200);
  sendAT("AT+SHCONF=\"BODYLEN\",1024", 2000);           delay(200);
  sendAT("AT+SHCONF=\"HEADERLEN\",512", 2000);          delay(200);
  sendAT("AT+SHCONF=\"URL\",\"https://" + String(IPEC_HOST) + "\"", 2000); delay(200);

  // 接続
  Serial.println(F("  サーバー接続中..."));
  String conn = sendAT("AT+SHCONN", 20000);
  if (conn.indexOf("OK") < 0) {
    Serial.println(F("  ✗ 接続失敗"));
    return false;
  }

  // リクエストヘッダ設定
  sendAT("AT+SHCHEAD", 2000);  // ヘッダクリア
  delay(200);
  sendAT("AT+SHAHEAD=\"Content-Type\",\"application/json\"", 2000);
  delay(200);
  sendAT("AT+SHAHEAD=\"x-api-key\",\"" + String(IPEC_API_KEY) + "\"", 2000);
  delay(200);

  // POST ボディ設定
  String bodCmd = "AT+SHBOD=" + String(bodyLen);
  sendAT(bodCmd, 3000);
  delay(300);
  Serial1.print(json);  // ボディ送信
  delay(500);
  // モジュールの応答を読み捨て
  String bodResp = "";
  unsigned long bs = millis();
  while (millis() - bs < 1000) {
    while (Serial1.available()) bodResp += (char)Serial1.read();
    yield();
  }
  if (bodResp.length() > 0) { Serial.print(F("  bod>")); Serial.println(bodResp); }

  // POST 実行
  Serial.println(F("  POST 送信中..."));
  String result = sendAT("AT+SHREQ=\"" + String(IPEC_PATH) + "\",3", 20000);

  // ステータスコード取得（+SHREQ: 3,<code>,<len>）
  int statusCode = 0;
  int idx = result.indexOf("+SHREQ: ");
  if (idx >= 0) {
    String s = result.substring(idx + 8);
    int c1 = s.indexOf(",");
    int c2 = s.indexOf(",", c1 + 1);
    if (c1 >= 0 && c2 > c1) statusCode = s.substring(c1 + 1, c2).toInt();
  }

  // レスポンスボディ読み込み
  String body = sendAT("AT+SHREAD=0,512", 5000);

  sendAT("AT+SHDISC", 3000);

  Serial.print(F("  レスポンスコード: ")); Serial.println(statusCode);
  if (body.length() > 0) {
    Serial.print(F("  レスポンスボディ: ")); Serial.println(body);
  }

  return (statusCode >= 200 && statusCode < 300);
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
  Serial.println();

  // Serial1 初期化（D6/D7 ブレッドボード配線）
  Serial1.setPins(LTE_RX_PIN, LTE_TX_PIN);
  Serial1.begin(115200);

  // SIM7080G 起動待ち
  Serial.print(F("SIM7080G 起動待ち（15秒）"));
  for (int i = 0; i < 15; i++) {
    delay(1000); Serial.print('.');
    while (Serial1.available()) Serial.write(Serial1.read());
  }
  Serial.println();

  // AT 疎通確認
  if (!probeAT()) {
    Serial.println(F("[ERROR] SIM7080G が応答しません。配線を確認してください。"));
    return;
  }

  // LTE-M 接続
  Serial.println(F("\n--- LTE-M 接続 ---"));
  if (!lteConnect()) {
    Serial.println(F("[ERROR] LTE-M 接続失敗。"));
    return;
  }

  // ═══ 5回 POST ═══
  Serial.println(F("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
  Serial.println(F("  POST 開始（5回）"));
  Serial.println(F("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));

  int okCount = 0;
  for (int i = 1; i <= TOTAL_SEND; i++) {
    Serial.print(F("\n【送信 ")); Serial.print(i); Serial.print(F("/")); Serial.print(TOTAL_SEND); Serial.println(F("】"));
    String json = buildJson(i);
    bool ok = postToIpec(json);

    if (ok) {
      okCount++;
      Serial.println(F("  ★ 成功 ✓"));
    } else {
      Serial.println(F("  ✗ 失敗"));
    }

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
  if (okCount == TOTAL_SEND) {
    Serial.println(F("  ✅ 全送信成功！iPEC サーバーへの POST を確認"));
  } else if (okCount > 0) {
    Serial.println(F("  ⚠ 一部成功。失敗した送信を確認してください。"));
  } else {
    Serial.println(F("  ❌ 全送信失敗。エンドポイント/APIキーを確認してください。"));
  }
  Serial.println(F("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
  Serial.println(F("\n[完了] テスト終了"));
}

void loop() {
  // テスト終了後はパススルーモード（AT コマンドを手動送信可能）
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) sendAT(line);
  }
  while (Serial1.available()) Serial.write(Serial1.read());
  yield();
}
