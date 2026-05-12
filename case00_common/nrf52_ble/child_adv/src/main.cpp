/**
 * BLE 子機 — アドバタイズに Manufacturer Data を載せる（Monita 共通モジュール）
 * 基板: Seeed XIAO nRF52840 (Sense)
 * コア: nordicnrf52 + ArduinoBLE
 *
 * 関連: nRF52840_Monita_common/README.md（案件 #2 #5 #6 #7 #8 向け共通化の置き場）
 */

#include <ArduinoBLE.h>
#include <string.h>

// ── 設定 ──────────────────────────────────────────
static char const *const DEVICE_NAME = "nRfBLE1";  // アドバタイズ名 & デバイス名
static uint16_t const ADV_INTERVAL_MS = 1000;     // アドバタイズ更新間隔 (ms)

static char const *const MFR_PAYLOAD = "cocrea0403";  // ManufacturerData ペイロード（ASCII 例）
static uint8_t const MFR_COMPANY_ID[] = {0xFF, 0xFF};  // Company ID（テスト用 0xFFFF）
// ─────────────────────────────────────────────────

static unsigned long lastAdvStart = 0;

static void startAdvertising() {
  BLE.stopAdvertise();

  size_t const payloadLen = strlen(MFR_PAYLOAD);
  uint8_t buf[2 + payloadLen];
  buf[0] = MFR_COMPANY_ID[0];
  buf[1] = MFR_COMPANY_ID[1];
  memcpy(&buf[2], MFR_PAYLOAD, payloadLen);

  BLEAdvertisingData advData;
  advData.setManufacturerData(buf, sizeof(buf));
  BLE.setAdvertisingData(advData);

  BLEAdvertisingData scanData;
  scanData.setLocalName(DEVICE_NAME);
  BLE.setScanResponseData(scanData);

  BLE.setAdvertisingInterval((uint16_t)(ADV_INTERVAL_MS * 1000u / 625u));
  BLE.advertise();

  lastAdvStart = millis();

  Serial.println("=== Advertising started ===");
  Serial.print("  Device name  : ");
  Serial.println(DEVICE_NAME);
  Serial.print("  Company ID   : 0x");
  Serial.print(MFR_COMPANY_ID[1], HEX);
  Serial.println(MFR_COMPANY_ID[0], HEX);
  Serial.print("  Payload      : ");
  Serial.println(MFR_PAYLOAD);
  Serial.print("  Payload (HEX): ");
  for (size_t i = 0; i < payloadLen; i++) {
    if ((uint8_t)MFR_PAYLOAD[i] < 0x10) {
      Serial.print('0');
    }
    Serial.print((uint8_t)MFR_PAYLOAD[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
  Serial.print("  Interval     : ");
  Serial.print(ADV_INTERVAL_MS);
  Serial.println(" ms");
  Serial.println("===========================");
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
    yield();
  }

  Serial.println("\n--- XIAO nRF52840 BLE Advertiser (MSD) ---");

  if (!BLE.begin()) {
    Serial.println("BLE initialization failed!");
    while (1);
  }

  BLE.setLocalName(DEVICE_NAME);
  startAdvertising();
}

void loop() {
  BLE.poll();
  if (millis() - lastAdvStart >= (unsigned long)ADV_INTERVAL_MS) {
    Serial.print('[');
    Serial.print(millis() / 1000UL);
    Serial.println("s] Re-advertising...");
    startAdvertising();
  }
}
