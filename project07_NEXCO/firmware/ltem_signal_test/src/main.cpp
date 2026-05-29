/**
 * LTE-M 電波強度テスト — NEXCO現場視察用
 *
 * 目的  : DOCOMO SIM / 1NCE SIM での電波状況・通信確認
 * MCU   : Seeed XIAO nRF52840
 * モジュール: M5Stamp CAT-M（SIM7080G）
 * 送信先 : InfluxDB Cloud（電波情報を時系列で記録）
 *
 * 配線:
 *   XIAO D6 (TX) → SIM7080G RX
 *   XIAO D7 (RX) ← SIM7080G TX
 *   XIAO 5V      → SIM7080G 5V
 *   XIAO GND     → SIM7080G GND
 *
 * SIM切り替え:
 *   下の「SIM設定」セクションでコメントアウトを切り替える
 *
 * 電波強度の見方:
 *   RSSI   : 総合受信電力。-70dBm以上が良好、-90dBm以下は不安定
 *   RSRP   : 参照信号受信電力。-100dBm以上が良好
 *   RSRQ   : 参照信号受信品質。-15dB以上が良好
 *   SINR   : 信号対干渉雑音比。高いほど良好（通常0〜30dB）
 *   CSQ    : 0〜31の指標。計算式: dBm = -113 + (CSQ × 2)
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

// ══════════════════════════════════════════════
// SIM設定（使用するSIMに合わせてコメントを切り替え）
// ══════════════════════════════════════════════

// ── 1NCE SIM ──
const char* APN        = "iot.1nce.net";
const char* SIM_NAME   = "1NCE";

// ── DOCOMO SIM（使う場合は上の2行をコメントアウトして下を有効化）──
// const char* APN        = "spmode.ne.jp";
// const char* SIM_NAME   = "DOCOMO";

// ══════════════════════════════════════════════
// InfluxDB設定
// ══════════════════════════════════════════════
const char* INFLUX_HOST   = "us-east-1-1.aws.cloud2.influxdata.com";
const char* INFLUX_ORG    = "Co-Crea";
const char* INFLUX_BUCKET = "LTE_test";
const char* INFLUX_TOKEN  = "REPLACE_WITH_YOUR_INFLUXDB_WRITE_TOKEN";

// ══════════════════════════════════════════════
// 測定間隔
// ══════════════════════════════════════════════
static uint32_t const MEASURE_INTERVAL_MS = 30000;  // 30秒ごとに計測・送信

// ══════════════════════════════════════════════
// 電波情報構造体
// ══════════════════════════════════════════════
struct SignalInfo {
  int   csq;       // CSQ生値（0-31, 99=不明）
  int   rssi_dbm;  // RSSI dBm換算
  int   rsrp;      // RSRP (dBm)
  int   rsrq;      // RSRQ (dB)
  int   sinr;      // SINR (dB)
  String band;     // 使用バンド
  String op;       // オペレーター名
  bool  valid;
};

// ═══════════════════════════════════════════════
// ATコマンド送受信
// ═══════════════════════════════════════════════
String sendAT(String cmd, int waitMs = 5000) {
  Serial.print(F(">> "));
  Serial.println(cmd);
  Serial1.print(cmd + "\r\n");

  long start = millis();
  String response = "";
  while (millis() - start < waitMs) {
    while (Serial1.available()) {
      response += (char)Serial1.read();
    }
  }
  Serial.println(response);
  return response;
}

// ═══════════════════════════════════════════════
// 電波強度取得
// ═══════════════════════════════════════════════
SignalInfo getSignalInfo() {
  SignalInfo info = {0, 0, 0, 0, 0, "", "", false};

  // ── AT+CSQ: 基本電波強度 ──
  String csqResp = sendAT("AT+CSQ", 3000);
  // 応答例: +CSQ: 18,0
  int csqIdx = csqResp.indexOf("+CSQ: ");
  if (csqIdx >= 0) {
    int commaIdx = csqResp.indexOf(",", csqIdx);
    if (commaIdx > csqIdx) {
      info.csq = csqResp.substring(csqIdx + 6, commaIdx).toInt();
      if (info.csq != 99) {
        info.rssi_dbm = -113 + (info.csq * 2);
      }
    }
  }

  // ── AT+CPSI?: LTE-M詳細情報 ──
  String cpsiResp = sendAT("AT+CPSI?", 3000);
  // 応答例: +CPSI: LTE-M,Online,440-10,0x1234,12345,0,EUTRAN-BAND1,100,4,4,-100,-12,-80,11
  int cpsiIdx = cpsiResp.indexOf("+CPSI: ");
  if (cpsiIdx >= 0) {
    String data = cpsiResp.substring(cpsiIdx + 7);
    // カンマ区切りで分割
    int f = 0;
    int pos = 0;
    String fields[15];
    while (pos < data.length() && f < 15) {
      int next = data.indexOf(",", pos);
      if (next < 0) next = data.length();
      fields[f++] = data.substring(pos, next);
      pos = next + 1;
    }
    // fields[0]=mode, [1]=status, [2]=MCC-MNC, [3]=LAC, [4]=cellID
    // [6]=band, [13]=RSRP, [14]=RSRQ は実測で位置が変わることがあるため
    // CPSI応答をそのまましてシリアルに表示する（後述）
    if (f > 6) info.band = fields[6];
  }

  // ── AT+COPS?: オペレーター名 ──
  String copsResp = sendAT("AT+COPS?", 3000);
  // 応答例: +COPS: 0,0,"NTT DOCOMO",9
  int copsIdx = copsResp.indexOf("\"");
  if (copsIdx >= 0) {
    int copsEnd = copsResp.indexOf("\"", copsIdx + 1);
    if (copsEnd > copsIdx) {
      info.op = copsResp.substring(copsIdx + 1, copsEnd);
    }
  }

  info.valid = (info.csq != 0 && info.csq != 99);
  return info;
}

// ═══════════════════════════════════════════════
// 電波情報をシリアルに表示
// ═══════════════════════════════════════════════
void printSignalInfo(SignalInfo &info) {
  Serial.println(F("┌─────────────────────────────┐"));
  Serial.print(F("│ SIM      : ")); Serial.println(SIM_NAME);
  Serial.print(F("│ Operator : ")); Serial.println(info.op.length() > 0 ? info.op : "不明");
  Serial.print(F("│ Band     : ")); Serial.println(info.band.length() > 0 ? info.band : "不明");
  Serial.print(F("│ CSQ      : ")); Serial.print(info.csq);
  Serial.print(F(" (RSSI≈")); Serial.print(info.rssi_dbm); Serial.println(F("dBm)"));

  // CSQの評価
  Serial.print(F("│ 電波評価 : "));
  if      (info.csq >= 20) Serial.println(F("◎ 良好"));
  else if (info.csq >= 10) Serial.println(F("○ 普通"));
  else if (info.csq >=  5) Serial.println(F("△ 弱い"));
  else if (info.csq !=  0) Serial.println(F("✗ 非常に弱い"));
  else                     Serial.println(F("✗ 圏外"));

  Serial.println(F("└─────────────────────────────┘"));
}

// ═══════════════════════════════════════════════
// ネットワーク初期化
// ═══════════════════════════════════════════════
bool initNetwork() {
  Serial.println(F("\n--- ネットワーク初期化 ---"));
  Serial.print(F("SIM: ")); Serial.println(SIM_NAME);
  Serial.print(F("APN: ")); Serial.println(APN);

  sendAT("AT+CNMP=38");  delay(500);  // LTE-Mのみ
  sendAT("AT+CMNB=1");   delay(500);  // Cat-M1
  String apnCmd = "AT+CGDCONT=1,\"IP\",\"";
  apnCmd += APN;
  apnCmd += "\"";
  sendAT(apnCmd); delay(500);

  Serial.println(F("ネットワーク登録待ち..."));
  for (int i = 0; i < 12; i++) {
    String reg = sendAT("AT+CREG?", 3000);
    if (reg.indexOf("0,1") >= 0 || reg.indexOf("0,5") >= 0) {
      Serial.println(F("✓ ネットワーク登録成功"));
      break;
    }
    if (i == 11) { Serial.println(F("✗ タイムアウト")); return false; }
    delay(5000);
  }

  String att = sendAT("AT+CGATT?", 3000);
  if (att.indexOf("+CGATT: 1") < 0) {
    Serial.println(F("✗ データ接続失敗")); return false;
  }
  Serial.println(F("✓ Attach完了"));
  delay(3000);

  sendAT("AT+CNACT=0,1", 15000);
  delay(3000);

  String cnact = sendAT("AT+CNACT?", 3000);
  if (cnact.indexOf("0,1") < 0) {
    Serial.println(F("✗ IPアドレス取得失敗")); return false;
  }

  Serial.println(F("✓ ネットワーク初期化完了"));
  return true;
}

// ═══════════════════════════════════════════════
// InfluxDBへ送信（電波情報含む）
// ═══════════════════════════════════════════════
bool postToInfluxDB(SignalInfo &info) {
  Serial.println(F("\n--- InfluxDB POST ---"));

  // Line Protocol: タグにSIM名、フィールドに電波情報
  String body = "ltem_signal,device=xiao,sim=";
  body += SIM_NAME;
  body += " csq=";
  body += String(info.csq);
  body += "i,rssi=";
  body += String(info.rssi_dbm);
  if (info.op.length() > 0) {
    // operator名はタグには入れず文字列フィールドとして除外（Line Protocolの制約）
  }
  int bodyLen = body.length();

  sendAT("AT+SHDISC", 3000); delay(500);

  sendAT("AT+CSSLCFG=\"ignorertctime\",1,1"); delay(300);
  sendAT("AT+CSSLCFG=\"sslversion\",1,3");    delay(300);
  sendAT("AT+CSSLCFG=\"sni\",1,\"" + String(INFLUX_HOST) + "\""); delay(300);
  sendAT("AT+SHSSL=1,\"\""); delay(300);

  sendAT("AT+SHCONF=\"BODYLEN\",1024");  delay(300);
  sendAT("AT+SHCONF=\"HEADERLEN\",350"); delay(300);
  sendAT("AT+SHCONF=\"URL\",\"https://" + String(INFLUX_HOST) + "\""); delay(300);

  Serial.println(F("接続中..."));
  String conn = sendAT("AT+SHCONN", 15000);
  if (conn.indexOf("OK") < 0) {
    Serial.println(F("✗ 接続失敗")); return false;
  }
  Serial.println(F("✓ 接続成功"));
  delay(500);

  sendAT("AT+SHCHEAD"); delay(300);
  sendAT("AT+SHAHEAD=\"Authorization\",\"Token " + String(INFLUX_TOKEN) + "\""); delay(300);
  sendAT("AT+SHAHEAD=\"Content-Type\",\"text/plain; charset=utf-8\""); delay(300);

  Serial.print(F("body: ")); Serial.println(body);
  Serial1.print("AT+SHBOD=" + String(bodyLen) + ",5000\r\n");
  delay(2000);
  Serial1.print(body);
  delay(1000);

  String path = "/api/v2/write?org=";
  path += INFLUX_ORG;
  path += "&bucket=";
  path += INFLUX_BUCKET;
  path += "&precision=s";
  String result = sendAT("AT+SHREQ=\"" + path + "\",3", 15000);
  sendAT("AT+SHDISC");

  if (result.indexOf(",204,") >= 0) {
    Serial.println(F("✓ POST成功！InfluxDBを確認してください"));
    return true;
  } else {
    Serial.println(F("✗ POST失敗"));
    return false;
  }
}

// ═══════════════════════════════════════════════
// setup
// ═══════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();
  Serial1.begin(115200);

  Serial.println(F("\n===================================="));
  Serial.println(F("  LTE-M 電波強度テスト（NEXCO現場）"));
  Serial.println(F("===================================="));
  Serial.print(F("SIM: ")); Serial.println(SIM_NAME);
  Serial.print(F("APN: ")); Serial.println(APN);
  Serial.println();

  delay(3000);

  sendAT("AT");
  sendAT("AT+CPIN?");

  // 接続前の電波状況を確認
  Serial.println(F("\n=== 接続前の電波状況 ==="));
  SignalInfo preInfo = getSignalInfo();
  printSignalInfo(preInfo);

  if (!initNetwork()) {
    Serial.println(F("初期化失敗。ATコマンドモードで継続します。"));
    Serial.println(F("手動で AT+CSQ や AT+CPSI? を送信できます。"));
    return;
  }

  // 接続後の電波状況を確認・送信
  Serial.println(F("\n=== 接続後の電波状況 ==="));
  SignalInfo postInfo = getSignalInfo();
  printSignalInfo(postInfo);

  postToInfluxDB(postInfo);
}

// ═══════════════════════════════════════════════
// loop: 30秒ごとに電波強度を計測・送信
// ═══════════════════════════════════════════════
static uint32_t lastMeasure = 0;

void loop() {
  uint32_t now = millis();

  // 30秒ごとに自動計測・送信
  if (now - lastMeasure >= MEASURE_INTERVAL_MS) {
    lastMeasure = now;
    Serial.println(F("\n=== 定期計測 ==="));
    SignalInfo info = getSignalInfo();
    printSignalInfo(info);
    postToInfluxDB(info);
  }

  // 手動ATコマンドモード
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) sendAT(line);
  }
  while (Serial1.available()) {
    Serial.write(Serial1.read());
  }
}
