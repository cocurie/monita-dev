/**
 * 検証 Step3: BLE アドバタイズ（親機）
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>

#define DEVICE_NAME  "TimeParent"

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Bluefruit.begin();
  Bluefruit.setName(DEVICE_NAME);

  // 自分の MAC アドレスを表示
  uint8_t mac[6];
  Bluefruit.Gap.getAddr(mac);
  Serial.printf("[BLE] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
    mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);

  // アドバタイズパケット
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.setInterval(160, 160);

  // 名前は Scan Response に入れる（既存コードに合わせた方式）
  Bluefruit.ScanResponse.addName();

  Bluefruit.Advertising.start(0);

  Serial.println(F("[STEP3] アドバタイズ開始"));
  Serial.println(F("  上記 MAC アドレスが子機のスキャン結果に出ればOK"));
  Serial.printf("  デバイス名: %s\n", DEVICE_NAME);
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last >= 3000) {
    last = millis();
    Serial.print(F("[ADV] isRunning="));
    Serial.println(Bluefruit.Advertising.isRunning() ? "YES" : "NO");
  }
}
