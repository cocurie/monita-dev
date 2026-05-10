/**
 * SIM7080G → InfluxDB Cloud HTTP POST テストスケッチ
 *
 * 確認日   : 2026-05-02
 * 環境     : ArduinoIDE / Arduino フレームワーク
 * MCU      : Seeed Studio XIAO nRF52840
 * モジュール: M5Stamp CAT-M（SIM7080G）
 * SIM      : 1NCE IoT SIM
 * 送信先   : InfluxDB Cloud
 *
 * 配線:
 *   XIAO D6 (TX) → SIM7080G RX
 *   XIAO D7 (RX) ← SIM7080G TX
 *   XIAO 5V      → SIM7080G 5V
 *   XIAO GND     → SIM7080G GND
 *
 * InfluxDB設定:
 *   Organization : Co-Crea
 *   Bucket       : LTE_test
 *   Endpoint     : https://us-east-1-1.aws.cloud2.influxdata.com
 *
 * 注意:
 *   AT+CNACT=0,1 はすでにアクティブな場合ERRORを返すが正常。
 *   AT+CNACT? でIPアドレスが取れていれば接続成功と判断する。
 *   AT+SHBOD後のボディはSerial1に直接書き込む（sendAT経由ではない）。
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

const char* INFLUX_HOST   = "us-east-1-1.aws.cloud2.influxdata.com";
const char* INFLUX_ORG    = "Co-Crea";
const char* INFLUX_BUCKET = "LTE_test";
const char* INFLUX_TOKEN  = "REPLACE_WITH_YOUR_INFLUXDB_WRITE_TOKEN";

float temperature = 25.3;
float humidity    = 60.1;

String sendAT(String cmd, int waitMs = 5000) {
  Serial.print(">> ");
  Serial.println(cmd);
  Serial1.print(cmd + "\r\n");

  long start = millis();
  String response = "";
  while (millis() - start < waitMs) {
    while (Serial1.available()) {
      char c = Serial1.read();
      response += c;
    }
  }
  Serial.println(response);
  return response;
}

bool initNetwork() {
  Serial.println("--- ネットワーク初期化 ---");

  sendAT("AT+CNMP=38");  delay(500);
  sendAT("AT+CMNB=1");   delay(500);
  sendAT("AT+CGDCONT=1,\"IP\",\"iot.1nce.net\""); delay(500);

  Serial.println("ネットワーク登録待ち...");
  for (int i = 0; i < 12; i++) {
    String reg = sendAT("AT+CREG?", 3000);
    if (reg.indexOf("0,1") >= 0 || reg.indexOf("0,5") >= 0) {
      Serial.println("✓ ネットワーク登録成功");
      break;
    }
    if (i == 11) {
      Serial.println("✗ タイムアウト");
      return false;
    }
    delay(5000);
  }

  String att = sendAT("AT+CGATT?", 3000);
  if (att.indexOf("+CGATT: 1") < 0) {
    Serial.println("✗ データ接続失敗");
    return false;
  }
  Serial.println("✓ Attach完了");

  delay(3000);

  // PDPコンテキスト有効化
  // すでにアクティブな場合はERRORが返るが正常動作
  Serial.println("PDPコンテキスト有効化中...");
  sendAT("AT+CNACT=0,1", 15000);
  delay(3000);

  // IPアドレスが取れているか確認
  String cnact = sendAT("AT+CNACT?", 3000);
  if (cnact.indexOf("0,1") < 0) {
    Serial.println("✗ IPアドレス取得失敗");
    return false;
  }

  Serial.println("✓ ネットワーク初期化完了");
  return true;
}

bool postToInfluxDB() {
  Serial.println("--- InfluxDB POST ---");

  // Line Protocol形式のボディ
  String body = "sensor,device=xiao temperature=";
  body += String(temperature, 1);
  body += ",humidity=";
  body += String(humidity, 1);
  int bodyLen = body.length();

  // 前回セッションの切断
  sendAT("AT+SHDISC", 3000);
  delay(500);

  // SSL設定
  sendAT("AT+CSSLCFG=\"ignorertctime\",1,1"); delay(300);
  sendAT("AT+CSSLCFG=\"sslversion\",1,3");    delay(300);
  sendAT("AT+CSSLCFG=\"sni\",1,\"" + String(INFLUX_HOST) + "\""); delay(300);
  sendAT("AT+SHSSL=1,\"\""); delay(300);

  // HTTPパラメータ設定
  sendAT("AT+SHCONF=\"BODYLEN\",1024");  delay(300);
  sendAT("AT+SHCONF=\"HEADERLEN\",350"); delay(300);
  sendAT("AT+SHCONF=\"URL\",\"https://" + String(INFLUX_HOST) + "\""); delay(300);

  // HTTP接続
  Serial.println("接続中...");
  String conn = sendAT("AT+SHCONN", 15000);
  if (conn.indexOf("OK") < 0) {
    Serial.println("✗ 接続失敗");
    return false;
  }
  Serial.println("✓ 接続成功");
  delay(500);

  // ヘッダ設定
  sendAT("AT+SHCHEAD"); delay(300);
  sendAT("AT+SHAHEAD=\"Authorization\",\"Token " + String(INFLUX_TOKEN) + "\""); delay(300);
  sendAT("AT+SHAHEAD=\"Content-Type\",\"text/plain; charset=utf-8\""); delay(300);

  // ボディ送信
  // AT+SHBOD後に>プロンプトが返るので、Serial1に直接書き込む
  Serial.println("ボディ送信中...");
  Serial.println("body: " + body);
  Serial1.print("AT+SHBOD=" + String(bodyLen) + ",5000\r\n");
  delay(2000);  // >プロンプト待ち
  Serial1.print(body);  // 生のテキストとして送信（\r\nなし）
  delay(1000);

  // POST送信
  Serial.println("POST送信中...");
  String path = "/api/v2/write?org=" + String(INFLUX_ORG) + "&bucket=" + String(INFLUX_BUCKET) + "&precision=s";
  String result = sendAT("AT+SHREQ=\"" + path + "\",3", 15000);

  sendAT("AT+SHDISC");

  // 204 No Content = InfluxDB書き込み成功
  if (result.indexOf(",204,") >= 0) {
    Serial.println("✓ POST成功！InfluxDBを確認してください");
    return true;
  } else {
    Serial.println("✗ POST失敗");
    Serial.println("response: " + result);
    return false;
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial1.begin(115200);
  Serial.println("=== SIM7080G → InfluxDB Cloud ===");

  delay(3000);  // モジュール起動待ち

  sendAT("AT");
  sendAT("AT+CPIN?");

  if (!initNetwork()) {
    Serial.println("初期化失敗。再起動してください。");
    return;
  }

  postToInfluxDB();
}

void loop() {
  // 手動ATモード：シリアルモニタからATコマンドを直接送信できる
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      sendAT(line);
    }
  }

  // SIM7080Gからの非同期メッセージをそのまま表示
  while (Serial1.available()) {
    Serial.write(Serial1.read());
  }
}