/**
 * case_other/ipec — 検証 Step1: iPEC サーバーへ HTTP POST（LTE-M / SIM7080G）
 *
 * 目的:
 *   コクリエ Monita Flex から iPEC 社のクラウドサーバーに
 *   JSON データを HTTP POST できるか確認する。
 *
 * 確認内容:
 *   1. LTE-M ネットワーク接続
 *   2. iPEC エンドポイントへ HTTPS POST（JSON ボディ）
 *   3. レスポンスコード（200 / 201）でサーバー側受信を確認
 *   4. 60 秒ごとに定期送信
 *
 * 配線（Flex v3.02 基板）:
 *   XIAO D8 (TX) → SIM7080G RX（基板上で接続済み）
 *   XIAO D9 (RX) ← SIM7080G TX（基板上で接続済み）
 *   D10 HIGH で 3V3_SW ON
 *
 * SIM: 1NCE IoT SIM（APN: iot.1nce.net）
 *
 * iPEC エンドポイント仕様（要確認）:
 *   URL  : https://22uzcg15xg.execute-api.ap-northeast-1.amazonaws.com/dev
 *   Method: POST
 *   Content-Type: application/json
 *   x-api-key: <iPEC から取得したキー>
 *
 * JSON ボディ例（iPEC 側スキーマに合わせて調整）:
 *   {
 *     "device_id": "monita-flex-001",
 *     "device_time": "2026-06-14T10:00:00",
 *     "channel_1": 1234,
 *     "channel_2": 5678
 *   }
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

// ══════════════════════════════════════════════
// ▼ ピン定義
// ══════════════════════════════════════════════
static const uint8_t  SW_POWER_PIN = 10;
static const uint8_t  LTE_TX_PIN   = 8;
static const uint8_t  LTE_RX_PIN   = 9;
static const uint32_t LTE_BAUD     = 115200;

// ══════════════════════════════════════════════
// ▼ 設定（ここを書き換える）
// ══════════════════════════════════════════════

// SIM / APN
const char* APN = "iot.1nce.net";

// iPEC サーバー設定
const char* IPEC_HOST     = "22uzcg15xg.execute-api.ap-northeast-1.amazonaws.com";
const char* IPEC_PATH     = "/dev";
const char* IPEC_API_KEY  = "YOUR_API_KEY_HERE";  // ← iPEC から取得した API キーを入れる
const char* DEVICE_ID     = "monita-flex-001";     // ← デバイス識別子

// 送信間隔
static const uint32_t SEND_INTERVAL_MS = 60000;  // 60 秒

// ══════════════════════════════════════════════
// AT コマンド送受信
// ══════════════════════════════════════════════
static String sendAT(const String& cmd, int waitMs = 5000) {
  Serial.print(F(">> "));
  Serial.println(cmd);
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
    delay(500);
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
  }
  Serial.println(F(" 失敗"));
  return false;
}

// ══════════════════════════════════════════════
// LTE-M 接続（ネットワーク登録 + IP 取得）
// ══════════════════════════════════════════════
static bool lteConnect() {
  sendAT("ATE0", 2000);
  sendAT("AT+CNMP=38", 2000);  // LTE only
  sendAT("AT+CMNB=1",  2000);  // LTE-M
  sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\"", 2000);

  Serial.println(F("ネットワーク登録待ち（最大60秒）"));
  bool registered = false;
  for (int i = 0; i < 12; i++) {
    String r = sendAT("AT+CREG?", 3000);
    if (r.indexOf("0,1") >= 0 || r.indexOf("0,5") >= 0) {
      Serial.println(F("ネットワーク登録 OK ✓"));
      registered = true;
      break;
    }
    String att = sendAT("AT+CGATT?", 2000);
    if (att.indexOf("+CGATT: 1") >= 0) {
      Serial.println(F("Attach 済み ✓"));
      registered = true;
      break;
    }
    delay(5000);
  }
  if (!registered) return false;

  // IP 取得
  sendAT("AT+CNACT=0,1", 15000);
  delay(3000);
  String ip = sendAT("AT+CNACT?", 3000);
  if (ip.indexOf("0,1") < 0) {
    Serial.println(F("IP 取得失敗"));
    return false;
  }
  Serial.println(F("IP 取得 OK ✓"));
  return true;
}

// ══════════════════════════════════════════════
// 現在時刻文字列（ISO 8601）
//   RTC 未実装のため、起動からのカウントで仮生成
//   本番は DS3231 から取得すること
// ══════════════════════════════════════════════
static String getTimestamp() {
  // TODO: DS3231 からの実時刻に差し替える
  unsigned long sec = millis() / 1000;
  char buf[24];
  snprintf(buf, sizeof(buf), "2026-06-14T00:%02lu:%02lu",
           (sec / 60) % 60, sec % 60);
  return String(buf);
}

// ══════════════════════════════════════════════
// JSON ボディ生成
//   ch1_raw, ch2_raw: HX711 生値（テストはダミー値）
// ══════════════════════════════════════════════
static String buildJson(int ch1_raw, int ch2_raw) {
  String ts = getTimestamp();
  String json = "{";
  json += "\"device_id\":\"";   json += DEVICE_ID;    json += "\",";
  json += "\"device_time\":\""; json += ts;           json += "\",";
  json += "\"channel_1\":";     json += ch1_raw;      json += ",";
  json += "\"channel_2\":";     json += ch2_raw;
  json += "}";
  return json;
}

// ══════════════════════════════════════════════
// iPEC サーバーへ HTTPS POST
// ══════════════════════════════════════════════
static bool postToIpec(const String& jsonBody) {
  Serial.println(F("\n--- iPEC POST 開始 ---"));
  Serial.print(F("ボディ: ")); Serial.println(jsonBody);
  Serial.print(F("バイト数: ")); Serial.println(jsonBody.length());

  // SSL 設定
  sendAT("AT+SHDISC", 2000);
  delay(300);
  sendAT("AT+CSSLCFG=\"ignorertctime\",1,1");
  delay(200);
  sendAT("AT+CSSLCFG=\"sslversion\",1,3");
  delay(200);
  sendAT("AT+CSSLCFG=\"sni\",1,\"" + String(IPEC_HOST) + "\"");
  delay(200);
  sendAT("AT+SHSSL=1,\"\"");
  delay(200);

  // SHCONF: バッファ設定
  sendAT("AT+SHCONF=\"BODYLEN\",2048");
  delay(200);
  sendAT("AT+SHCONF=\"HEADERLEN\",512");
  delay(200);
  sendAT("AT+SHCONF=\"URL\",\"https://" + String(IPEC_HOST) + "\"");
  delay(200);

  // ホストへ接続
  Serial.println(F("サーバーに接続中..."));
  String conn = sendAT("AT+SHCONN", 20000);
  if (conn.indexOf("OK") < 0) {
    Serial.println(F("✗ 接続失敗"));
    return false;
  }
  Serial.println(F("接続 OK ✓"));

  // リクエストヘッダ設定
  sendAT("AT+SHCHEAD");  // ヘッダクリア
  delay(200);
  sendAT("AT+SHAHEAD=\"Content-Type\",\"application/json\"");
  delay(200);
  sendAT("AT+SHAHEAD=\"x-api-key\",\"" + String(IPEC_API_KEY) + "\"");
  delay(200);

  // POST 送信（method=3: POST、body 長さを指定）
  int bodyLen = jsonBody.length();
  Serial.println(F("POST 送信中..."));

  // ボディを SHWRITE で書き込む
  String writeCmd = "AT+SHWRITE=0," + String(bodyLen);
  sendAT(writeCmd, 3000);
  delay(200);
  Serial1.print(jsonBody);  // ボディ直接送信
  delay(500);
  String rawResp = "";
  unsigned long ws = millis();
  while (millis() - ws < 2000) {
    while (Serial1.available()) rawResp += (char)Serial1.read();
    yield();
  }
  if (rawResp.length() > 0) Serial.println(rawResp);

  // POST リクエスト実行
  String result = sendAT("AT+SHREQ=\"" + String(IPEC_PATH) + "\",3," + String(bodyLen), 20000);

  // ステータスコード取得（+SHREQ: 3,<code>,<len>）
  int statusCode = 0;
  int idx = result.indexOf("+SHREQ: ");
  if (idx >= 0) {
    String s = result.substring(idx + 8);
    int c1 = s.indexOf(",");
    int c2 = s.indexOf(",", c1 + 1);
    if (c1 >= 0 && c2 > c1) statusCode = s.substring(c1 + 1, c2).toInt();
  }

  Serial.print(F("レスポンスコード: ")); Serial.println(statusCode);

  // レスポンスボディ読み込み
  String body = sendAT("AT+SHREAD=0,512", 5000);
  Serial.print(F("レスポンスボディ: ")); Serial.println(body);

  sendAT("AT+SHDISC", 3000);

  // 成功判定
  if (statusCode >= 200 && statusCode < 300) {
    Serial.println(F("✓ POST 成功 — サーバー受信確認"));
    return true;
  } else {
    Serial.print(F("✗ POST 失敗 (code="));
    Serial.print(statusCode);
    Serial.println(F(")"));
    return false;
  }
}

// ══════════════════════════════════════════════
// Arduino エントリ
// ══════════════════════════════════════════════
static bool s_lteReady = false;
static int  s_sendCount = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) yield();
  delay(1000);

  Serial.println(F("\n[iPEC POST] 検証 Step1: iPEC サーバー HTTP POST テスト"));
  Serial.println(F("========================================================"));
  Serial.print(F("エンドポイント: https://"));
  Serial.print(IPEC_HOST);
  Serial.println(IPEC_PATH);

  if (String(IPEC_API_KEY) == "YOUR_API_KEY_HERE") {
    Serial.println(F("\n⚠ IPEC_API_KEY が未設定です。"));
    Serial.println(F("  iPEC から取得したキーを IPEC_API_KEY に設定してください。"));
    Serial.println(F("  このまま続けますが、401 Unauthorized になる可能性があります。"));
  }

  // 電源 ON
  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  // シリアル初期化
  Serial1.setPins(LTE_RX_PIN, LTE_TX_PIN);
  Serial1.begin(LTE_BAUD);
  delay(100);

  // モジュール起動待ち
  Serial.print(F("SIM7080G 起動待ち（15秒）"));
  for (int i = 0; i < 15; i++) {
    delay(1000);
    Serial.print('.');
    while (Serial1.available()) Serial.write(Serial1.read());
  }
  Serial.println();

  // AT 疎通
  if (!probeAT()) {
    Serial.println(F("[ERROR] AT 応答なし。配線・電源を確認してください。"));
    return;
  }

  // LTE-M 接続
  if (!lteConnect()) {
    Serial.println(F("[ERROR] LTE-M 接続失敗。SIM / アンテナを確認してください。"));
    return;
  }

  s_lteReady = true;
  Serial.println(F("\n[準備完了] 60秒ごとに POST します。"));
  Serial.println(F("シリアルモニタから AT コマンドも送信できます。"));
}

static uint32_t s_lastSend = 0;

void loop() {
  if (!s_lteReady) {
    // AT コマンドパススルー
    if (Serial.available()) {
      String line = Serial.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) { Serial1.print(line + "\r\n"); }
    }
    while (Serial1.available()) Serial.write(Serial1.read());
    yield();
    return;
  }

  uint32_t now = millis();
  // 初回は即送信
  if (s_lastSend == 0 || now - s_lastSend >= SEND_INTERVAL_MS) {
    s_lastSend = now;
    s_sendCount++;

    Serial.print(F("\n=== 送信 #")); Serial.print(s_sendCount); Serial.println(F(" ==="));

    // ダミーデータ（本番は HX711 等から取得）
    int ch1 = 1000 + s_sendCount * 10;
    int ch2 = 2000 + s_sendCount * 5;

    String json = buildJson(ch1, ch2);
    bool ok = postToIpec(json);

    Serial.print(F("結果: "));
    Serial.println(ok ? F("✓ 成功") : F("✗ 失敗"));
    Serial.print(F("次回送信まで "));
    Serial.print(SEND_INTERVAL_MS / 1000);
    Serial.println(F(" 秒"));
  }

  // AT コマンドパススルー（手動確認用）
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      Serial1.print(line + "\r\n");
      Serial.print(F(">> ")); Serial.println(line);
    }
  }
  while (Serial1.available()) Serial.write(Serial1.read());
  yield();
}
