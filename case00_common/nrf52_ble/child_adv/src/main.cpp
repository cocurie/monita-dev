/**
 * BLE 子機 — アドバタイズに Manufacturer Data を載せる（Monita 共通モジュール）
 * 基板: Seeed XIAO nRF52840 (Sense) / Adafruit Bluefruit52 Peripheral
 *
 * 関連: nRF52840_Monita_common/README.md（案件 #2 #5 #6 #7 #8 向け共通化の置き場）
 */

#include <Adafruit_TinyUSB.h>  // USB Serial
#include <string.h>
#include <bluefruit.h>

// ── 設定 ──────────────────────────────────────────
static char const *const DEVICE_NAME = "nRfBLE1";  // アドバタイズ名 & デバイス名
static uint16_t const ADV_INTERVAL_MS = 1000;     // アドバタイズ更新間隔 (ms)

static char const *const MFR_PAYLOAD = "cocrea0403";  // ManufacturerData ペイロード（ASCII 例）
static uint8_t const MFR_COMPANY_ID[] = {0xFF, 0xFF};  // Company ID（テスト用 0xFFFF）
// ─────────────────────────────────────────────────

static unsigned long lastAdvStart = 0;

static void startAdvertising() {
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);

  size_t const payloadLen = strlen(MFR_PAYLOAD);
  uint8_t buf[2 + payloadLen];
  buf[0] = MFR_COMPANY_ID[0];
  buf[1] = MFR_COMPANY_ID[1];
  memcpy(&buf[2], MFR_PAYLOAD, payloadLen);
  Bluefruit.Advertising.addManufacturerData(buf, sizeof(buf));

  Bluefruit.ScanResponse.addName();

  uint16_t const interval = (uint16_t)(ADV_INTERVAL_MS * 1000u / 625u);
  Bluefruit.Advertising.setInterval(interval, interval);
  Bluefruit.Advertising.setFastTimeout(0);
  Bluefruit.Advertising.start(0);

  lastAdvStart = millis();

  Serial.println(F("=== Advertising started ==="));
  Serial.print(F("  Device name  : "));
  Serial.println(DEVICE_NAME);
  Serial.print(F("  Company ID   : 0x"));
  Serial.print(MFR_COMPANY_ID[1], HEX);
  Serial.println(MFR_COMPANY_ID[0], HEX);
  Serial.print(F("  Payload      : "));
  Serial.println(MFR_PAYLOAD);
  Serial.print(F("  Payload (HEX): "));
  for (size_t i = 0; i < payloadLen; i++) {
    if ((uint8_t)MFR_PAYLOAD[i] < 0x10) {
      Serial.print('0');
    }
    Serial.print((uint8_t)MFR_PAYLOAD[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
  Serial.print(F("  Interval     : "));
  Serial.print(ADV_INTERVAL_MS);
  Serial.println(F(" ms"));
  Serial.println(F("==========================="));
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
    yield();
  }

  Serial.println(F("\n--- XIAO nRF52840 BLE Advertiser (MSD) ---"));

  Bluefruit.begin();
  Bluefruit.setName(DEVICE_NAME);

  startAdvertising();
}

void loop() {
  if (millis() - lastAdvStart >= (unsigned long)ADV_INTERVAL_MS) {
    Serial.print('[');
    Serial.print(millis() / 1000UL);
    Serial.println(F("s] Re-advertising..."));
    startAdvertising();
  }
}
