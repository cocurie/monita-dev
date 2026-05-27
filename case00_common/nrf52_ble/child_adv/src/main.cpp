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

static uint8_t const MFR_COMPANY_ID[] = {0xFF, 0xFF};  // Company ID（テスト用 0xFFFF）

// 擬似センサ: analogRead するピン（4CH分）
static uint8_t const SENSOR_PINS[] = {A0, A1, A2, A3};
static uint8_t const NUM_CHANNELS  = sizeof(SENSOR_PINS);
// ─────────────────────────────────────────────────

static unsigned long lastAdvStart = 0;

static void startAdvertising() {
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);

  // CompanyID(2) + uint16_t × NUM_CHANNELS
  uint8_t buf[2 + NUM_CHANNELS * 2];
  buf[0] = MFR_COMPANY_ID[0];
  buf[1] = MFR_COMPANY_ID[1];

  Serial.println(F("=== Advertising started ==="));
  Serial.print(F("  Device name  : "));
  Serial.println(DEVICE_NAME);
  Serial.print(F("  Company ID   : 0x"));
  Serial.print(MFR_COMPANY_ID[1], HEX);
  Serial.println(MFR_COMPANY_ID[0], HEX);
  Serial.print(F("  Sensor (HEX) : "));

  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    uint16_t val = (uint16_t)analogRead(SENSOR_PINS[i]);
    buf[2 + i * 2]     = (uint8_t)(val >> 8);   // big-endian
    buf[2 + i * 2 + 1] = (uint8_t)(val & 0xFF);

    // シリアル出力
    if (val < 0x1000) Serial.print('0');
    if (val < 0x0100) Serial.print('0');
    if (val < 0x0010) Serial.print('0');
    Serial.print(val, HEX);
    Serial.print(' ');
  }
  Serial.println();

  Bluefruit.Advertising.addManufacturerData(buf, sizeof(buf));
  Bluefruit.ScanResponse.addName();

  uint16_t const interval = (uint16_t)(ADV_INTERVAL_MS * 1000u / 625u);
  Bluefruit.Advertising.setInterval(interval, interval);
  Bluefruit.Advertising.setFastTimeout(0);
  Bluefruit.Advertising.start(0);

  lastAdvStart = millis();

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
