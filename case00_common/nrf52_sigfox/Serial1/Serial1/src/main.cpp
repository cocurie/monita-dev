/**
 * Sigfox クラウド受信まで確認済みのテストスケッチ（Serial1版）
 *
 * 環境: ArduinoIDE / PlatformIO（Arduino フレームワーク）
 * 配線: Serial1（ハードウェアUART）— D6=TX→Sigfox RX, D7=RX←Sigfox TX（+3V3, GND）
 *
 * 依存: Adafruit TinyUSB（USB Serial）
 * ※ SoftwareSerial は不要になるため削除
 *
 * ArduinoIDEで動かない場合はPlatformIOへ移行する
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

// SoftwareSerial → Serial1 に置き換え
// ピンはD6(TX)/D7(RX)のまま変更なし

String sendAT(String cmd, int waitMs = 2000) {
  Serial.print(">> ");
  Serial.println(cmd);
  Serial1.print(cmd + "\r");

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

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }

  Serial1.begin(9600);   // SoftwareSerialと同じ9600で開始
  Serial.println("=== Sigfox Cloud Test (Serial1) ===");

  delay(3000);  // モジュール起動待ち

  sendAT("AT$I=10");
  delay(500);

  sendAT("AT$I=11");
  delay(500);

  sendAT("AT$GI?");
  delay(500);

  sendAT("AT$RC");
  delay(1000);

  Serial.println("=== Sending to Sigfox Cloud ===");
  String result = sendAT("AT$SF=CAFE", 10000);

  if (result.indexOf("OK") >= 0) {
    Serial.println("✓ 送信成功！クラウドで確認してください");
  } else {
    Serial.println("✗ 送信失敗");
  }
}

void loop() {
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      sendAT(line);
    }
  }
}