//
// Step15: MCP9601 熱電対アンプ I2C 動作確認（断線・短絡検知対応）
// XIAO nRF52840  SDA=D4  SCL=D5（デフォルトI2Cピン）
// I2Cアドレス: 0x60（ADDR=GND 時）
//
// ※ MCP9601はVSENSEピンによる断線(OC)・短絡(SC)検知に対応
//    熱電対未接続時 → ステータスレジスタのOPENCIRCUITビットが立つ
//    COLD           → ICチップ周囲温度として常に有効
//

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MCP9601.h>

static constexpr uint8_t MCP_ADDR = 0x60;

// 断線・短絡フラグのデバウンス回数（1カウント≒ループ1周＝約1秒 → 10回で約10秒）
// VSENSEノードは高インピーダンス（約338kΩ）でノイズを拾いやすく、
// 正常時16.9%VDDに対しOC判定閾値19%まで約69mVしか余裕がない。
// このため単発のフラグは誤報とみなし、連続でこの回数立った時のみ確定させる。
//
// 実測（裸接合の熱電対を手で握った状態）ではOCフラグ生起率が約38%あり、
// 3回では誤警報がすり抜けたため10回に設定。ただしこれは対症療法で、
// 根本対策はver1.1での平衡コモンモードコンデンサ追加。
static constexpr uint8_t FAULT_DEBOUNCE_COUNT = 10;

Adafruit_MCP9601 mcp;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("=== Step15: MCP9601 動作確認 ===");
  Serial.println("SDA=D4  SCL=D5  addr=0x60");
  Serial.println();

  Wire.begin();  // D4=SDA, D5=SCL（nRF52はピン固定、引数不要）
  delay(100);    // モジュール起動待ち

  // I2Cスキャン（アドレス確認 + MCP9601範囲チェックを1ループで）
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
    Serial.println("[DIAG] MCP9601アドレス範囲のエラーコード:");
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
    Serial.println("[ERROR] 0x60〜0x67 の範囲に MCP9601 が見つかりません");
    while (1) delay(100);
  }

  Serial.print("[INFO] MCP9601 アドレス: 0x");
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
    Serial.println("[ERROR] MCP9601 初期化失敗（デバイスID不一致）");
    while (1) delay(100);
  }

  Serial.println("[OK]   MCP9601 検出");

  mcp.setThermocoupleType(MCP9600_TYPE_K);   // K型熱電対
  mcp.setFilterCoefficient(3);               // 平滑化係数（0=なし〜7=最大）
  mcp.setADCresolution(MCP9600_ADCRESOLUTION_18); // 18bit（最高分解能）
  mcp.enable(true);                          // 連続変換モード開始

  Serial.print("[設定] 熱電対: K型 / フィルタ: 3 / ADC: 18bit / 連続変換 / デバウンス: ");
  Serial.print(FAULT_DEBOUNCE_COUNT);
  Serial.println("回連続");
  Serial.println();
  Serial.println("※ 断線・短絡はSTATUSレジスタ bit4/bit5 で検知（連続検出時のみ警報）");
  Serial.println("※ [NOISE]行は単発フラグの棄却数＝ノイズ環境の目安");
  Serial.println("---------------------------------------------------");
}

void loop() {
  float hot  = mcp.readThermocouple(); // ホット接点（熱電対先端）
  float cold = mcp.readAmbient();      // コールド接点（IC周囲温度）

  Serial.print("HOT  : "); Serial.print(hot,  2); Serial.println(" C");
  Serial.print("COLD : "); Serial.print(cold, 2); Serial.println(" C");
  Serial.print("DELTA: "); Serial.print(hot - cold, 2); Serial.println(" C");

  // ステータスレジスタ（0x04）確認
  //   bit4(0x10) = Open-Circuit / Input Range 兼用、bit5(0x20) = Short-Circuit
  //   ※ MCP9600とはビット位置が異なるので注意
  uint8_t st = mcp.getStatus();
  const bool ocRaw = (st & MCP9601_STATUS_OPENCIRCUIT)  != 0;
  const bool scRaw = (st & MCP9601_STATUS_SHORTCIRCUIT) != 0;

  // 連続してフラグが立った時のみ異常として確定させる（デバウンス）
  static uint8_t  ocCount = 0, scCount = 0;
  static uint32_t ocSuppressed = 0, scSuppressed = 0;

  if (ocRaw) { if (ocCount < FAULT_DEBOUNCE_COUNT) ocCount++; } else { ocCount = 0; }
  if (scRaw) { if (scCount < FAULT_DEBOUNCE_COUNT) scCount++; } else { scCount = 0; }

  const bool ocFault = (ocCount >= FAULT_DEBOUNCE_COUNT);
  const bool scFault = (scCount >= FAULT_DEBOUNCE_COUNT);

  if (ocRaw && !ocFault) ocSuppressed++;  // ノイズとして棄却
  if (scRaw && !scFault) scSuppressed++;

  if (ocFault) Serial.println("[WARN] オープン回路（熱電対未接続または断線）");
  if (scFault) Serial.println("[WARN] ショート検出（GNDまたはVCC）");

  // 棄却した単発フラグの累計 = 設置環境のノイズ指標
  if (ocSuppressed || scSuppressed) {
    Serial.print("[NOISE] 棄却 OC="); Serial.print(ocSuppressed);
    Serial.print(" SC=");             Serial.println(scSuppressed);
  }

  Serial.println("---------------------------------------------------");
  delay(1000);
}
