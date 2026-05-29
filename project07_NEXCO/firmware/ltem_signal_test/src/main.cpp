/**
 * LTE-M 電波強度テスト — NEXCO現場視察用
 * 送信先: Googleスプレッドシート（GAS経由）
 *
 * MCU   : Seeed XIAO nRF52840
 * モジュール: M5Stamp CAT-M（SIM7080G）
 *
 * 配線:
 *   XIAO D6 (TX) → SIM7080G RX
 *   XIAO D7 (RX) ← SIM7080G TX
 *   XIAO 5V      → SIM7080G 5V
 *   XIAO GND     → SIM7080G GND
 *
 * 事前準備：Google Apps Script を以下の内容でデプロイし、
 * スクリプトIDを GAS_SCRIPT_ID に設定する。
 *
 * --- GAS コード ---
 * function doGet(e) {
 *   var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
 *   sheet.appendRow([
 *     new Date(),
 *     e.parameter.sim  || '',
 *     e.parameter.op   || '',
 *     e.parameter.csq  || '',
 *     e.parameter.rssi || '',
 *     e.parameter.band || '',
 *   ]);
 *   return ContentService.createTextOutput('OK');
 * }
 * ------------------
 * デプロイURL例: https://script.google.com/macros/s/AKfycb.../exec
 * スクリプトID = URL中の "AKfycb..." の部分
 *
 * 電波強度の見方:
 *   CSQ  : 0〜31。20以上◎、10以上○、5以上△、それ以下✗
 *   RSSI : CSQから換算。-113+(CSQ×2) dBm。-70以上が良好
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

// ══════════════════════════════════════════════
// ▼ ここを書き換える
// ══════════════════════════════════════════════

// GAS スクリプトID（デプロイURLの "AKfycb..." 部分）
const char* GAS_SCRIPT_ID = "ここにスクリプトIDを貼り付ける";

// SIM切り替え（使う方だけ有効にする）
// ── 1NCE SIM ──
const char* APN      = "iot.1nce.net";
const char* SIM_NAME = "1NCE";
// ── DOCOMO SIM（使う場合は上2行をコメントアウト）──
// const char* APN      = "spmode.ne.jp";
// const char* SIM_NAME = "DOCOMO";

// ══════════════════════════════════════════════
// 測定間隔（現場で動かし続けるとき）
// ══════════════════════════════════════════════
static uint32_t const MEASURE_INTERVAL_MS = 60000;  // 60秒ごと

// ══════════════════════════════════════════════
// ATコマンド送受信
// ══════════════════════════════════════════════
String sendAT(String cmd, int waitMs = 5000) {
  Serial.print(F(">> ")); Serial.println(cmd);
  Serial1.print(cmd + "\r\n");
  long start = millis();
  String res = "";
  while (millis() - start < waitMs) {
    while (Serial1.available()) res += (char)Serial1.read();
  }
  Serial.println(res);
  return res;
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

SignalInfo getSignalInfo() {
  SignalInfo info = {0, 0, "", ""};

  // CSQ
  String csqR = sendAT("AT+CSQ", 3000);
  int idx = csqR.indexOf("+CSQ: ");
  if (idx >= 0) {
    int comma = csqR.indexOf(",", idx);
    info.csq = csqR.substring(idx + 6, comma).toInt();
    if (info.csq != 99) info.rssi_dbm = -113 + info.csq * 2;
  }

  // バンド情報
  String cpsi = sendAT("AT+CPSI?", 3000);
  int cpsiIdx = cpsi.indexOf("+CPSI: ");
  if (cpsiIdx >= 0) {
    String data = cpsi.substring(cpsiIdx + 7);
    int f = 0, pos = 0;
    String fields[10];
    while (pos < data.length() && f < 10) {
      int next = data.indexOf(",", pos);
      if (next < 0) next = data.length();
      fields[f++] = data.substring(pos, next);
      pos = next + 1;
    }
    if (f > 6) info.band = fields[6];
  }

  // オペレーター名
  String cops = sendAT("AT+COPS?", 3000);
  int q1 = cops.indexOf("\"");
  if (q1 >= 0) {
    int q2 = cops.indexOf("\"", q1 + 1);
    if (q2 > q1) info.op = cops.substring(q1 + 1, q2);
  }

  return info;
}

// 電波評価表示
void printSignalInfo(SignalInfo &info) {
  Serial.println(F("┌─────────────────────────────┐"));
  Serial.print(F("│ SIM      : ")); Serial.println(SIM_NAME);
  Serial.print(F("│ Operator : ")); Serial.println(info.op.length() ? info.op : "不明");
  Serial.print(F("│ Band     : ")); Serial.println(info.band.length() ? info.band : "不明");
  Serial.print(F("│ CSQ      : ")); Serial.print(info.csq);
  Serial.print(F("  RSSI≈")); Serial.print(info.rssi_dbm); Serial.println(F("dBm"));
  Serial.print(F("│ 評価     : "));
  if      (info.csq >= 20) Serial.println(F("◎ 良好"));
  else if (info.csq >= 10) Serial.println(F("○ 普通"));
  else if (info.csq >=  5) Serial.println(F("△ 弱い"));
  else if (info.csq >   0) Serial.println(F("✗ 非常に弱い"));
  else                     Serial.println(F("✗ 圏外 / 不明"));
  Serial.println(F("└─────────────────────────────┘"));
}

// ══════════════════════════════════════════════
// URLパース補助
// ══════════════════════════════════════════════

// "Location: https://host/path\r\n" からURL部分を抜き出す
String parseLocation(String headers) {
  int idx = headers.indexOf("Location: ");
  if (idx < 0) idx = headers.indexOf("location: ");
  if (idx < 0) return "";
  int start = idx + 10;
  int end = headers.indexOf("\r\n", start);
  if (end < 0) end = headers.indexOf("\n", start);
  if (end < 0) end = headers.length();
  return headers.substring(start, end);
}

// "https://hostname/path?q=1" → "hostname"
String parseHost(String url) {
  int s = url.indexOf("://");
  if (s < 0) return "";
  s += 3;
  int e = url.indexOf("/", s);
  if (e < 0) e = url.length();
  return url.substring(s, e);
}

// "https://hostname/path?q=1" → "/path?q=1"
String parsePath(String url) {
  int s = url.indexOf("://");
  if (s < 0) return "/";
  s = url.indexOf("/", s + 3);
  if (s < 0) return "/";
  return url.substring(s);
}

// ══════════════════════════════════════════════
// スプレッドシートへ送信（GAS経由・リダイレクト対応）
// ══════════════════════════════════════════════
bool postToSheet(SignalInfo &info) {
  Serial.println(F("\n--- スプレッドシート送信 ---"));

  // クエリパラメータ組み立て
  String params = "sim=";
  params += SIM_NAME;
  params += "&csq=";
  params += String(info.csq);
  params += "&rssi=";
  params += String(info.rssi_dbm);
  params += "&op=";
  params += info.op;
  params += "&band=";
  params += info.band;

  String scriptPath = "/macros/s/";
  scriptPath += GAS_SCRIPT_ID;
  scriptPath += "/exec?";
  scriptPath += params;

  // ── Step1: script.google.com に GET ──────────
  sendAT("AT+SHDISC", 2000);
  delay(300);

  sendAT("AT+CSSLCFG=\"ignorertctime\",1,1"); delay(200);
  sendAT("AT+CSSLCFG=\"sslversion\",1,3");    delay(200);
  sendAT("AT+CSSLCFG=\"sni\",1,\"script.google.com\""); delay(200);
  sendAT("AT+SHSSL=1,\"\""); delay(200);
  sendAT("AT+SHCONF=\"BODYLEN\",1024");  delay(200);
  sendAT("AT+SHCONF=\"HEADERLEN\",1024"); delay(200);
  sendAT("AT+SHCONF=\"URL\",\"https://script.google.com\""); delay(200);

  Serial.println(F("script.google.com に接続中..."));
  String conn = sendAT("AT+SHCONN", 15000);
  if (conn.indexOf("OK") < 0) {
    Serial.println(F("✗ 接続失敗"));
    return false;
  }

  Serial.println(F("GET送信中..."));
  String result = sendAT("AT+SHREQ=\"" + scriptPath + "\",1", 15000);

  // ステータスコード取得 "+SHREQ: 1,302,0"
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

  // ── Step2: 302 → Location ヘッダを取得 ──────
  Serial.println(F("302リダイレクト → Locationを取得中..."));
  String headers = sendAT("AT+SHREADHD=0,800", 5000);
  String location = parseLocation(headers);
  Serial.print(F("Location: ")); Serial.println(location);

  sendAT("AT+SHDISC"); delay(500);

  if (location.length() == 0) {
    Serial.println(F("✗ Locationが取得できなかった"));
    return false;
  }

  // ── Step3: リダイレクト先に GET ─────────────
  String redirHost = parseHost(location);
  String redirPath = parsePath(location);
  Serial.print(F("リダイレクト先 Host: ")); Serial.println(redirHost);
  Serial.print(F("リダイレクト先 Path: ")); Serial.println(redirPath);

  sendAT("AT+CSSLCFG=\"sni\",1,\"" + redirHost + "\""); delay(200);
  sendAT("AT+SHCONF=\"URL\",\"https://" + redirHost + "\""); delay(200);

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
bool initNetwork() {
  Serial.println(F("\n--- ネットワーク初期化 ---"));
  sendAT("AT+CNMP=38"); delay(500);
  sendAT("AT+CMNB=1");  delay(500);
  sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\""); delay(500);

  for (int i = 0; i < 12; i++) {
    String reg = sendAT("AT+CREG?", 3000);
    if (reg.indexOf("0,1") >= 0 || reg.indexOf("0,5") >= 0) {
      Serial.println(F("✓ ネットワーク登録成功")); break;
    }
    if (i == 11) { Serial.println(F("✗ タイムアウト")); return false; }
    delay(5000);
  }

  String att = sendAT("AT+CGATT?", 3000);
  if (att.indexOf("+CGATT: 1") < 0) {
    Serial.println(F("✗ Attach失敗")); return false;
  }
  Serial.println(F("✓ Attach完了"));
  delay(3000);

  sendAT("AT+CNACT=0,1", 15000); delay(3000);
  String cnact = sendAT("AT+CNACT?", 3000);
  if (cnact.indexOf("0,1") < 0) {
    Serial.println(F("✗ IPアドレス取得失敗")); return false;
  }
  Serial.println(F("✓ ネットワーク初期化完了"));
  return true;
}

// ══════════════════════════════════════════════
// setup
// ══════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();
  Serial1.begin(115200);

  Serial.println(F("\n===================================="));
  Serial.println(F("  LTE-M → スプレッドシート 送信テスト"));
  Serial.println(F("===================================="));
  Serial.print(F("SIM: ")); Serial.println(SIM_NAME);
  Serial.print(F("APN: ")); Serial.println(APN);
  Serial.println();

  delay(3000);
  sendAT("AT");
  sendAT("AT+CPIN?");

  // 接続前の電波状況
  Serial.println(F("\n=== 接続前の電波状況 ==="));
  SignalInfo pre = getSignalInfo();
  printSignalInfo(pre);

  if (!initNetwork()) {
    Serial.println(F("初期化失敗。ATコマンドモードで継続します。"));
    return;
  }

  // 接続後の電波状況 → スプレッドシートへ
  Serial.println(F("\n=== 接続後の電波状況 ==="));
  SignalInfo post = getSignalInfo();
  printSignalInfo(post);
  postToSheet(post);
}

// ══════════════════════════════════════════════
// loop: 60秒ごとに計測・送信 + 手動ATモード
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
    if (line.length() > 0) sendAT(line);
  }
  while (Serial1.available()) Serial.write(Serial1.read());
}
