//
// Step16: ADS1115 I2C ADC 動作確認
// XIAO nRF52840  SDA=D4  SCL=D5
//
// I2Cアドレス: 0x48（ADDR=GND 時、Amazonモジュール標準）
//
// PGA設定: ±2.048V（62.5 µV/LSB）
// CH4/CH5の±10V計測は分圧回路実装後に実施
//
// ── 差動入力（DIFF）の読み方 ──────────────────────────────
// DIFF 0-1 = AIN0 の電圧 − AIN1 の電圧
// DIFF 2-3 = AIN2 の電圧 − AIN3 の電圧
//
// フローティング時の典型的な挙動（このスケッチで確認済み）:
//   単端: AIN0〜AIN3 がそれぞれ ~280mV  → 空中ノイズを拾う
//   差動: DIFF ≈ 0mV                    → 両ピンに乗った同じノイズが相殺
//
// これを「同相除去（コモンモードリジェクション）」と呼ぶ。
// ±10V入力回路では AIN0/AIN1 に差動信号を入力するため、
// 電源リプルや電磁ノイズは両ピンに同じように乗って打ち消され、
// 実際の信号だけを精度よく取り出せる。
// ──────────────────────────────────────────────────────────

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>

Adafruit_ADS1115 ads;

static constexpr float LSB_MV = 0.0625f; // PGA ±2.048V: 62.5µV = 0.0625mV/LSB

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("=== Step16: ADS1115 動作確認 ===");
  Serial.println("SDA=D4  SCL=D5  PGA=±2.048V  62.5µV/LSB");
  Serial.println();

  Wire.begin();
  delay(100);

  // I2Cスキャン
  Serial.println("[I2C スキャン]");
  uint8_t adsAddr = 0;
  uint8_t totalFound = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("  検出: 0x");
      if (addr < 0x10) Serial.print("0");
      Serial.println(addr, HEX);
      totalFound++;
      // ADS1115アドレス範囲: 0x48〜0x4B
      if (addr >= 0x48 && addr <= 0x4B) adsAddr = addr;
    }
  }
  Serial.println();

  if (totalFound == 0) {
    Serial.println("[ERROR] デバイスが見つかりません → 配線・電源を確認してください");
    while (1) delay(100);
  }
  if (adsAddr == 0) {
    Serial.println("[ERROR] 0x48〜0x4B の範囲に ADS1115 が見つかりません");
    while (1) delay(100);
  }

  Serial.print("[INFO] ADS1115 アドレス: 0x");
  Serial.println(adsAddr, HEX);

  // ADS1115 初期化
  ads.setDataRate(RATE_ADS1115_128SPS);
  if (!ads.begin(adsAddr, &Wire)) {
    Serial.println("[ERROR] ADS1115 初期化失敗");
    while (1) delay(100);
  }

  // PGA ±2.048V に設定（62.5µV/LSB）
  ads.setGain(GAIN_TWO);

  Serial.println("[OK]   ADS1115 初期化完了");
  Serial.println("[設定] PGA=±2.048V / 128SPS / 連続変換なし（シングルショット）");
  Serial.println();
  Serial.println("       AIN0〜AIN3: 単端入力（対GND）");
  Serial.println("       DIFF 0-1  : AIN0−AIN1 差動（CH4 ±10V入力用）");
  Serial.println("       DIFF 2-3  : AIN2−AIN3 差動（CH5 ±10V入力用）");
  Serial.println("---------------------------------------------------");
}

void loop() {
  // 単端入力（シングルエンド）
  int16_t raw0 = ads.readADC_SingleEnded(0);
  int16_t raw1 = ads.readADC_SingleEnded(1);
  int16_t raw2 = ads.readADC_SingleEnded(2);
  int16_t raw3 = ads.readADC_SingleEnded(3);

  // 差動入力（AIN0−AIN1 / AIN2−AIN3）
  // 両ピンに共通のノイズ（コモンモード）は相殺され、信号差分のみ残る
  int16_t diff01 = ads.readADC_Differential_0_1();
  int16_t diff23 = ads.readADC_Differential_2_3();

  Serial.println("[単端入力]");
  Serial.print("  AIN0: "); Serial.print(raw0 * LSB_MV, 2); Serial.print(" mV");
  Serial.print("  (raw="); Serial.print(raw0); Serial.println(")");

  Serial.print("  AIN1: "); Serial.print(raw1 * LSB_MV, 2); Serial.print(" mV");
  Serial.print("  (raw="); Serial.print(raw1); Serial.println(")");

  Serial.print("  AIN2: "); Serial.print(raw2 * LSB_MV, 2); Serial.print(" mV");
  Serial.print("  (raw="); Serial.print(raw2); Serial.println(")");

  Serial.print("  AIN3: "); Serial.print(raw3 * LSB_MV, 2); Serial.print(" mV");
  Serial.print("  (raw="); Serial.print(raw3); Serial.println(")");

  Serial.println("[差動入力]");
  Serial.print("  DIFF 0-1: "); Serial.print(diff01 * LSB_MV, 2); Serial.print(" mV");
  Serial.print("  (raw="); Serial.print(diff01); Serial.println(")");

  Serial.print("  DIFF 2-3: "); Serial.print(diff23 * LSB_MV, 2); Serial.print(" mV");
  Serial.print("  (raw="); Serial.print(diff23); Serial.println(")");

  Serial.println("---------------------------------------------------");
  delay(1000);
}
