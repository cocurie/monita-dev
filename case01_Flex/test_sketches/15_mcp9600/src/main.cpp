//
// Step15: MCP9600 熱電対アンプ I2C 動作確認
// XIAO nRF52840  SDA=D4  SCL=D5（デフォルトI2Cピン）
// I2Cアドレス: 0x60（ADDR=GND 時）
//
// ※ 熱電対未接続時の挙動：
//    HOT  → 不定値またはオープン検出ワーニング
//    COLD → ICチップ周囲温度として有効
//

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MCP9600.h>

static constexpr uint8_t MCP_ADDR = 0x60;

Adafruit_MCP9600 mcp;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("=== Step15: MCP9600 動作確認 ===");
  Serial.println("SDA=D4  SCL=D5  addr=0x60");
  Serial.println();

  Wire.begin();  // D4=SDA, D5=SCL（nRF52はピン固定、引数不要）
  delay(100);    // モジュール起動待ち

  // I2Cスキャン（アドレス確認 + MCP9600範囲チェックを1ループで）
  Serial.println("[I2C スキャン]");
  uint8_t mcpAddr = 0;
  uint8_t totalFound = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.print("  検出: 0x");
      if (addr < 0x10) Serial.print("0");
      Serial.println(addr, HEX);
      totalFound++;
      if (addr >= 0x60 && addr <= 0x67) mcpAddr = addr;
    }
  }
  Serial.println();

  if (totalFound == 0) {
    // 0x60〜0x67 のエラーコードを診断出力
    Serial.println("[DIAG] MCP9600アドレス範囲のエラーコード:");
    for (uint8_t addr = 0x60; addr <= 0x67; addr++) {
      Wire.beginTransmission(addr);
      uint8_t err = Wire.endTransmission();
      Serial.print("  0x"); Serial.print(addr, HEX);
      Serial.print(" → err="); Serial.print(err);
      // 2=NACK(バス正常・デバイスなし), 4=その他エラー, 5=タイムアウト
      if      (err == 2) Serial.println(" (NACK: バス正常、デバイス未応答 → VCC/GND確認)");
      else if (err == 4) Serial.println(" (バスエラー → SDA/SCL配線確認)");
      else if (err == 5) Serial.println(" (タイムアウト → SCL/SDA切断の可能性)");
      else               Serial.println();
    }
    Serial.println();
    Serial.println("[ERROR] デバイスが見つかりません → 上記診断を参照してください");
    while (1) delay(100);
  }
  if (mcpAddr == 0) {
    Serial.println("[ERROR] 0x60〜0x67 の範囲に MCP9600 が見つかりません");
    while (1) delay(100);
  }

  Serial.print("[INFO] MCP9600 アドレス: 0x");
  Serial.println(mcpAddr, HEX);

  // スキャン直後の最初のリピーテッドスタート読み取りは失敗しやすいため、
  // 捨て読み（ウォームアップ）を1回入れてからbegin()する。
  Wire.beginTransmission(mcpAddr);
  Wire.write(0x20);
  Wire.endTransmission(false);
  Wire.requestFrom((int)mcpAddr, 2);
  while (Wire.available()) Wire.read();
  delay(20);

  // begin()はデバイスIDレジスタを1回しか読まないため、
  // 稀に発生する初回読み取り失敗に備えて数回リトライする。
  bool ok = false;
  for (uint8_t attempt = 0; attempt < 5 && !ok; attempt++) {
    ok = mcp.begin(mcpAddr, &Wire);
    if (!ok) delay(50);
  }

  if (!ok) {
    Serial.println("[ERROR] MCP9600 初期化失敗（デバイスID不一致）");
    while (1) delay(100);
  }

  Serial.println("[OK]   MCP9600 検出");

  mcp.setThermocoupleType(MCP9600_TYPE_K);   // K型熱電対
  mcp.setFilterCoefficient(3);               // 平滑化係数（0=なし〜7=最大）
  mcp.setADCresolution(MCP9600_ADCRESOLUTION_18); // 18bit（最高分解能）
  mcp.enable(true);                          // 連続変換モード開始

  Serial.println("[設定] 熱電対: K型 / フィルタ: 3 / ADC: 18bit / 連続変換");
  Serial.println();
  Serial.println("※ 熱電対未接続時は HOT が不定値、COLD は周囲温度として有効");
  Serial.println("---------------------------------------------------");
}

void loop() {
  float hot  = mcp.readThermocouple(); // ホット接点（熱電対先端）
  float cold = mcp.readAmbient();      // コールド接点（IC周囲温度）

  Serial.print("HOT  : "); Serial.print(hot,  2); Serial.println(" C");
  Serial.print("COLD : "); Serial.print(cold, 2); Serial.println(" C");
  Serial.print("DELTA: "); Serial.print(hot - cold, 2); Serial.println(" C");

  // ステータスレジスタ確認
  uint8_t st = mcp.getStatus();
  if (st & 0x02) Serial.println("[WARN] オープン回路（熱電対未接続または断線）");
  if (st & 0x01) Serial.println("[WARN] ショート検出（GNDまたはVCC）");

  Serial.println("---------------------------------------------------");
  delay(1000);
}
