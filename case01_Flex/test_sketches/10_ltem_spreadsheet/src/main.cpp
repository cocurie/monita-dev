/**
 * Monita Flex v3.02 — 検証 Step10: SIM7080G LTE-M → Google スプレッドシート送信
 *
 * 確認内容:
 *   1. LTE-M ネットワーク接続（Step9 の続き）
 *   2. 電波強度取得（AT+CSQ / AT+CPSI? / AT+COPS?）
 *   3. GAS 経由でスプレッドシートへ HTTP GET 送信（302 リダイレクト対応）
 *   4. 20 秒ごとに定期送信
 *
 * 配線（Flex v3.02 基板上）:
 *   XIAO D8 (TX) → SIM7080G RX（基板上で接続済み）
 *   XIAO D9 (RX) ← SIM7080G TX（基板上で接続済み）
 *   D10 HIGH で 3V3_SW ON
 *   M5STAMP CatM 5V ← XIAO 5V ピン（ジャンパ追加）
 *
 * SIM: 1NCE IoT SIM（APN: iot.1nce.net）
 *
 * GAS コード（doGet）:
 *   function doGet(e) {
 *     var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
 *     sheet.appendRow([
 *       new Date(),
 *       e.parameter.sim  || '',
 *       e.parameter.op   || '',
 *       e.parameter.csq  || '',
 *       e.parameter.rssi || '',
 *       e.parameter.band || '',
 *     ]);
 *     return ContentService.createTextOutput('OK');
 *   }
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

// ══════════════════════════════════════════════
// ▼ ピン定義（Flex v3.02）
// ══════════════════════════════════════════════
static const uint8_t  SW_POWER_PIN = 10;  // 3V3_SW 制御
static const uint8_t  LTE_TX_PIN   = 8;   // D8 → SIM7080G RX
static const uint8_t  LTE_RX_PIN   = 9;   // D9 ← SIM7080G TX
static const uint32_t LTE_BAUD     = 115200;

// ══════════════════════════════════════════════
// ▼ GAS / SIM 設定（ここを書き換える）
// ══════════════════════════════════════════════

// GAS スクリプトID（デプロイURLの "AKfycb..." 部分、/exec まで含める）
const char* GAS_SCRIPT_ID =
  "AKfycbywRcyl3059evcw-kFo9ypeejbhZWRyY9rILX9TUjlEWJ-4K2nGkZqIrZymA9cYGZ8maQ/exec";

// ── 1NCE SIM ──
const char* APN      = "iot.1nce.net";
const char* SIM_NAME = "1NCE";
const char* APN_USER = "";
const char* APN_PASS = "";

// ── SORACOM SIM（使う場合は上4行をコメントアウトして有効化）──
// const char* APN      = "soracom.io";
// const char* SIM_NAME = "SORACOM";
// const char* APN_USER = "sora";
// const char* APN_PASS = "sora";

// 定期送信間隔
static const uint32_t MEASURE_INTERVAL_MS = 20000;  // 20秒

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
    yield();  // FreeRTOS watchdog 対策
  }

  if (res.length() > 0) Serial.println(res);
  else                   Serial.println(F("(応答なし)"));
  return res;
}

// ══════════════════════════════════════════════
// AT 疎通確認（115200 固定・最大 20 回リトライ）
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
      while (Serial1.available()) Serial1.read();  // バッファクリア
      return true;
    }
    while (Serial1.available()) Serial1.read();
  }
  Serial.println(F(" 失敗"));
  return false;
}

// ══════════════════════════════════════════════
// 電波強度取得
// ══════════════════════════════════════════════
struct SignalInfo {
  int    csq;
  int    rssi_dbm;
  String band;
  String op;
};

static SignalInfo getSignalInfo() {
  SignalInfo info = {0, 0, "", ""};

  // CSQ
  String csqR = sendAT("AT+CSQ", 3000);
  int idx = csqR.indexOf("+CSQ: ");
  if (idx >= 0) {
    int comma = csqR.indexOf(",", idx);
    info.csq = csqR.substring(idx + 6, comma).toInt();
    if (info.csq != 99) info.rssi_dbm = -113 + info.csq * 2;
  }

  // バンド情報（AT+CPSI?）
  String cpsi = sendAT("AT+CPSI?", 3000);
  int cpsiIdx = cpsi.indexOf("+CPSI: ");
  if (cpsiIdx >= 0) {
    String data = cpsi.substring(cpsiIdx + 7);
    int f = 0, pos = 0;
    String fields[10];
    while (pos < (int)data.length() && f < 10) {
      int next = data.indexOf(",", pos);
      if (next < 0) next = data.length();
      fields[f++] = data.substring(pos, next);
      pos = next + 1;
    }
    if (f > 6) info.band = fields[6];
  }

  // オペレーター名（AT+COPS?）
  String cops = sendAT("AT+COPS?", 3000);
  int q1 = cops.indexOf("\"");
  if (q1 >= 0) {
    int q2 = cops.indexOf("\"", q1 + 1);
    if (q2 > q1) info.op = cops.substring(q1 + 1, q2);
  }

  return info;
}

static void printSignalInfo(const SignalInfo& info) {
  Serial.println(F("┌─────────────────────────────┐"));
  Serial.print(F("│ SIM      : ")); Serial.println(SIM_NAME);
  Serial.print(F("│ Operator : ")); Serial.println(info.op.length() ? info.op : "不明");
  Serial.print(F("│ Band     : ")); Serial.println(info.band.length() ? info.band : "不明");
  Serial.print(F("│ CSQ      : ")); Serial.print(info.csq);
  Serial.print(F("  RSSI≈")); Serial.print(info.rssi_dbm); Serial.println(F(" dBm"));
  Serial.print(F("│ 評価     : "));
  if      (info.csq >= 20) Serial.println(F("◎ 良好"));
  else if (info.csq >= 10) Serial.println(F("○ 普通"));
  else if (info.csq >=  5) Serial.println(F("△ 弱い"));
  else if (info.csq >   0) Serial.println(F("✗ 非常に弱い"));
  else                     Serial.println(F("✗ 圏外 / 不明"));
  Serial.println(F("└─────────────────────────────┘"));
}

// ══════════════════════════════════════════════
// URL パース補助
// ══════════════════════════════════════════════
static String parseLocation(const String& headers) {
  int idx = headers.indexOf("Location: ");
  if (idx < 0) idx = headers.indexOf("location: ");
  if (idx < 0) return "";
  int start = idx + 10;
  int end = headers.indexOf("\r\n", start);
  if (end < 0) end = headers.indexOf("\n", start);
  if (end < 0) end = headers.length();
  return headers.substring(start, end);
}

static String parseHost(const String& url) {
  int s = url.indexOf("://");
  if (s < 0) return "";
  s += 3;
  int e = url.indexOf("/", s);
  if (e < 0) e = url.length();
  return url.substring(s, e);
}

static String parsePath(const String& url) {
  int s = url.indexOf("://");
  if (s < 0) return "/";
  s = url.indexOf("/", s + 3);
  if (s < 0) return "/";
  return url.substring(s);
}

// ══════════════════════════════════════════════
// スプレッドシートへ送信（GAS 経由・302 リダイレクト対応）
// ══════════════════════════════════════════════
static bool postToSheet(const SignalInfo& info) {
  Serial.println(F("\n--- スプレッドシート送信 ---"));

  // クエリパラメータ組み立て
  String params = "sim=";   params += SIM_NAME;
  params += "&csq=";        params += String(info.csq);
  params += "&rssi=";       params += String(info.rssi_dbm);
  params += "&op=";         params += info.op;
  params += "&band=";       params += info.band;

  String scriptPath = "/macros/s/";
  scriptPath += GAS_SCRIPT_ID;  // 末尾 /exec まで含む
  scriptPath += "?";
  scriptPath += params;

  // ── Step1: script.google.com に HTTPS GET ──
  sendAT("AT+SHDISC", 2000);
  delay(300);

  sendAT("AT+CSSLCFG=\"ignorertctime\",1,1");               delay(200);
  sendAT("AT+CSSLCFG=\"sslversion\",1,3");                  delay(200);
  sendAT("AT+CSSLCFG=\"sni\",1,\"script.google.com\"");     delay(200);
  sendAT("AT+SHSSL=1,\"\"");                                 delay(200);
  sendAT("AT+SHCONF=\"BODYLEN\",1024");                      delay(200);
  sendAT("AT+SHCONF=\"HEADERLEN\",350");                     delay(200);
  sendAT("AT+SHCONF=\"URL\",\"https://script.google.com\""); delay(200);

  Serial.println(F("script.google.com に接続中..."));
  String conn = sendAT("AT+SHCONN", 15000);
  if (conn.indexOf("OK") < 0) {
    Serial.println(F("✗ 接続失敗"));
    return false;
  }

  Serial.println(F("GET 送信中..."));
  String result = sendAT("AT+SHREQ=\"" + scriptPath + "\",1", 15000);

  // ステータスコード取得 "+SHREQ: 1,302,<len>"
  int statusCode = 0;
  int shreqIdx = result.indexOf("+SHREQ: ");
  if (shreqIdx >= 0) {
    String s = result.substring(shreqIdx + 8);
    int c1 = s.indexOf(",");
    int c2 = s.indexOf(",", c1 + 1);
    if (c1 >= 0 && c2 > c1) statusCode = s.substring(c1 + 1, c2).toInt();
  }
  Serial.print(F("ステータスコード: ")); Serial.println(statusCode);

  if (statusCode == 200) {
    Serial.println(F("✓ 送信成功（リダイレクトなし）"));
    sendAT("AT+SHDISC");
    return true;
  }

  if (statusCode != 302) {
    Serial.println(F("✗ 予期しないレスポンス"));
    sendAT("AT+SHDISC");
    return false;
  }

  // ── Step2: 302 → Location ヘッダを取得 ──
  Serial.println(F("302 リダイレクト → Location 取得中..."));
  int readLen = 350;
  if (shreqIdx >= 0) {
    String s2 = result.substring(shreqIdx + 8);
    int c1b = s2.indexOf(",");
    int c2b = s2.indexOf(",", c1b + 1);
    if (c1b >= 0 && c2b > c1b) {
      int dataLen = s2.substring(c2b + 1).toInt();
      if (dataLen > 0 && dataLen < readLen) readLen = dataLen;
    }
  }
  String headers = sendAT("AT+SHREADHD=0," + String(readLen), 5000);
  String location = parseLocation(headers);
  Serial.print(F("Location: ")); Serial.println(location);

  sendAT("AT+SHDISC"); delay(500);

  if (location.length() == 0) {
    Serial.println(F("✗ Location が取得できなかった"));
    return false;
  }

  // ── Step3: リダイレクト先に GET ──
  String redirHost = parseHost(location);
  String redirPath = parsePath(location);
  Serial.print(F("リダイレクト先 Host: ")); Serial.println(redirHost);

  sendAT("AT+CSSLCFG=\"sni\",1,\"" + redirHost + "\"");         delay(200);
  sendAT("AT+SHCONF=\"URL\",\"https://" + redirHost + "\"");    delay(200);

  Serial.println(F("リダイレクト先に接続中..."));
  conn = sendAT("AT+SHCONN", 15000);
  if (conn.indexOf("OK") < 0) {
    Serial.println(F("✗ リダイレクト先への接続失敗"));
    return false;
  }

  result = sendAT("AT+SHREQ=\"" + redirPath + "\",1", 15000);
  sendAT("AT+SHDISC");

  if (result.indexOf(",200,") >= 0) {
    Serial.println(F("✓ スプレッドシートへの送信成功！"));
    return true;
  }

  Serial.println(F("✗ 最終リクエスト失敗"));
  Serial.println(result);
  return false;
}

// ══════════════════════════════════════════════
// ネットワーク初期化
// ══════════════════════════════════════════════
static bool initNetwork() {
  Serial.println(F("\n--- ネットワーク初期化 ---"));

  sendAT("AT+CNMP=38"); delay(500);   // LTE only
  sendAT("AT+CMNB=1");  delay(500);   // LTE-M
  sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\""); delay(500);

  if (strlen(APN_USER) > 0) {
    sendAT("AT+CGAUTH=1,1,\"" + String(APN_PASS) + "\",\"" + String(APN_USER) + "\"");
    delay(500);
  }

  // CREG 確認（最大 60 秒）
  bool cregOk = false;
  for (int i = 0; i < 12; i++) {
    String reg = sendAT("AT+CREG?", 3000);
    if (reg.indexOf("0,1") >= 0 || reg.indexOf("0,5") >= 0) {
      Serial.println(F("✓ ネットワーク登録成功"));
      cregOk = true;
      break;
    }
    delay(5000);
  }

  // CGATT 確認（CREG が取れなくても Attach 済みなら続行）
  String att = sendAT("AT+CGATT?", 3000);
  if (att.indexOf("+CGATT: 1") < 0) {
    Serial.println(F("✗ Attach 失敗"));
    return false;
  }
  if (!cregOk) {
    Serial.println(F("△ CREG 未確認だが Attach 済 → 続行"));
  }

  // IP アドレス取得
  delay(3000);
  sendAT("AT+CNACT=0,1", 15000); delay(3000);
  String cnact = sendAT("AT+CNACT?", 3000);
  if (cnact.indexOf("0,1") < 0) {
    Serial.println(F("✗ IP アドレス取得失敗"));
    return false;
  }

  Serial.println(F("✓ ネットワーク初期化完了"));
  return true;
}

// ══════════════════════════════════════════════
// Arduino エントリ
// ══════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) yield();
  delay(1000);

  // 3V3_SW ON
  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  // Serial1 は一度だけ初期化（end/begin ループはしない）
  Serial1.setPins(LTE_RX_PIN, LTE_TX_PIN);
  Serial1.begin(LTE_BAUD);
  delay(100);

  Serial.println(F("\n[STEP10] LTE-M → スプレッドシート送信テスト"));
  Serial.print(F("SIM: ")); Serial.print(SIM_NAME);
  Serial.print(F(" / APN: ")); Serial.println(APN);
  Serial.println(F("============================================="));

  // SIM7080G 起動待ち（起動ログも表示）
  Serial.print(F("SIM7080G 起動待ち（15秒）"));
  for (int i = 0; i < 15; i++) {
    delay(1000);
    Serial.print('.');
    while (Serial1.available()) {
      char c = Serial1.read();
      Serial.write(c);
    }
  }
  Serial.println();

  // AT 疎通
  if (!probeAT()) {
    Serial.println(F("\n★ AT 応答なし。loop() でパススルーモードに入ります。"));
    return;
  }

  sendAT("ATE0", 2000);  // エコーオフ

  // SIM 確認
  String cpin = sendAT("AT+CPIN?", 5000);
  if (cpin.indexOf("READY") < 0) {
    Serial.println(F("✗ SIM 未認識。カード挿入を確認してください。"));
    return;
  }
  Serial.println(F("✓ SIM READY"));

  // ネットワーク接続
  if (!initNetwork()) {
    Serial.println(F("\nloop() で AT コマンドを手動送信できます。"));
    return;
  }

  // 初回計測 → 送信
  Serial.println(F("\n=== 初回計測・送信 ==="));
  SignalInfo info = getSignalInfo();
  printSignalInfo(info);
  postToSheet(info);

  Serial.println(F("\n=== Step10 完了（定期送信モードへ） ==="));
}

// ══════════════════════════════════════════════
// loop: 定期計測・送信 + 手動 AT コマンドモード
// ══════════════════════════════════════════════
static uint32_t lastMeasure = 0;

void loop() {
  uint32_t now = millis();
  if (now - lastMeasure >= MEASURE_INTERVAL_MS) {
    lastMeasure = now;
    Serial.println(F("\n=== 定期計測 ==="));
    SignalInfo info = getSignalInfo();
    printSignalInfo(info);
    postToSheet(info);
  }

  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      Serial1.print(line + "\r\n");
      Serial.print(F(">> "));
      Serial.println(line);
    }
  }
  while (Serial1.available()) Serial.write(Serial1.read());
  yield();
}
