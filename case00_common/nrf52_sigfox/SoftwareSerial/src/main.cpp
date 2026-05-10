/**
 * Sigfox テストスケッチ（SoftwareSerial D4/D5版）
 *
 * 配線: D4=TX→Sigfox RX, D5=RX←Sigfox TX（+3V3, GND）
 その通りです。nRF52840はフレキシブルなピンマッピング機能（EasyDMA）を持っており、
 理論上は多くのピンでハードウェアUARTが使えるのが特徴です。ただし今回はSoftwareSerialで対応したので、その特徴は使っていません。
 本来の特徴を活かすにはPlatformIOやZephyrで深く設定する必要があります。
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <SoftwareSerial.h>

#define SF_RX D5
#define SF_TX D4

SoftwareSerial Sigfox(SF_RX, SF_TX);

String sendAT(String cmd, int waitMs = 2000) {
  Serial.print(">> ");
  Serial.println(cmd);
  Sigfox.print(cmd + "\r");

  long start = millis();
  String response = "";

  while (millis() - start < waitMs) {
    while (Sigfox.available()) {
      char c = Sigfox.read();
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

  Sigfox.begin(9600);
  Serial.println("=== Sigfox Cloud Test (SoftwareSerial D4/D5) ===");

  delay(3000);

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