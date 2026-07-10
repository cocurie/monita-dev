/**
 * Monita Flex 横河基板 ver1.0 — ハードウェア立ち上げ（bring-up）
 *
 * 【対象ハード】
 *   Seeed XIAO ESP32-C3 / netlist_yokogawa_v1（2026/07/09）
 *
 * 【この段階の目的】
 *   実基板の配線が正しいかを1本ずつ確認する。I2C バス上の3デバイス
 *   （MCP23008 / MCP9600 / ADS1115）を検出し、MCP23008 で MUX セレクトを
 *   動かし、MCP9600 で CH6（K型熱電対）を読む。HX711本読み取り・
 *   ADS1115・SD・BLE は後続で追加。
 *
 * 【ピン割当】（netlist_yokogawa_v1 確定値）
 *   HX711 PD_SCK : D1  (GPIO3)   JP10-2
 *   HX711 DOUT   : D2  (GPIO4)   JP10-3  ← 4051 MUX の共通出力 Z
 *   I2C SDA      : D4  (GPIO6)   JP10-5
 *   I2C SCL      : D5  (GPIO7)   JP10-6
 *   SD SPI_CS    : D7  (GPIO20)  JP11-1
 *   SD SPI_SCK   : D8  (GPIO8)   JP11-2  ← strapping / R5 プルアップ
 *   SD SPI_MISO  : D9  (GPIO9)   JP11-3
 *   SD SPI_MOSI  : D10 (GPIO10)  JP11-4
 *   (strapping)  : D0  (GPIO2)   JP10-1  ← R1 で 3V3 固定（FW未使用）
 *
 * 【I2C デバイス】（すべてソフトウェア/ビットバングI2Cで通信）
 *   MCP23008 : 0x20（A0/A1/A2=GND）  GP0/GP1/GP2 → 4051 の S0/S1/S2
 *   MCP9600  : JP17（CH6 K型熱電対） アドレスは基板ごとに自動検出
 *   ADS1115  : JP18（CH7/CH8 ±10V）  0x48（ADDR=GND）
 *
 * 【なぜハードウェアWireを使わないか】
 *   ESP32-C3のI2Cペリフェラルはクロックストレッチング用タイムアウト
 *   カウンタが5bitしかなく（esp32-hal-i2c.cのコメント参照）、MCP9600の
 *   クロックストレッチングに対応できないことを実機検証で確認した
 *   （nRF52840では同一基板・同一配線で正常動作）。ハードウェアWireと
 *   ソフトI2Cを同一ピンで頻繁に切り替える方式（Wire.end()/begin()）は
 *   ペリフェラルの明け渡しタイミングが不安定だったため、このバス上の
 *   全デバイスをソフトウェアI2Cに統一している。
 */

#include <Arduino.h>
#include "soft_i2c.h"

// ============================================================
// ピン定義（netlist_yokogawa_v1）
// ============================================================
#define HX711_PD_SCK   D1   // GPIO3
#define HX711_DOUT     D2   // GPIO4
#define PIN_SDA        D4   // GPIO6
#define PIN_SCL        D5   // GPIO7
#define SD_CS          D7   // GPIO20
#define SD_SCK         D8   // GPIO8
#define SD_MISO        D9   // GPIO9
#define SD_MOSI        D10  // GPIO10

// ============================================================
// I2C デバイスアドレス
// ============================================================
static constexpr uint8_t MCP23008_ADDR = 0x20;  // MUX セレクト用 GPIO エクスパンダ
static constexpr uint8_t ADS1115_ADDR  = 0x48;  // ±10V 2CH ADC

// MCP23008 レジスタ
static constexpr uint8_t MCP_IODIR = 0x00;  // 入出力方向（0=出力）
static constexpr uint8_t MCP_GPIO  = 0x09;  // ポート値

// ============================================================
// MCP9600（CH8 K型熱電対）
// ============================================================
static constexpr uint8_t MCP9600_HOTJUNCTION  = 0x00;
static constexpr uint8_t MCP9600_COLDJUNCTION = 0x02;
static constexpr uint8_t MCP9600_STATUS       = 0x04;
static constexpr uint8_t MCP9600_SENSORCONFIG = 0x05; // [6:4]熱電対種別 [2:0]フィルタ係数
static constexpr uint8_t MCP9600_DEVICECONFIG = 0x06; // [6:5]ADC分解能 [1:0]動作モード
static constexpr uint8_t MCP9600_DEVICEID     = 0x20; // 上位byte=0x40(MCP9600)

static uint8_t g_mcp9600_addr = 0;
static bool g_mcp9600_ok = false;

static uint8_t mcp9600Scan() {
  for (uint8_t addr = 0x60; addr <= 0x67; addr++) {
    if (softI2CProbe(addr)) return addr;
  }
  return 0;
}

// K型 / フィルタ係数3 / 18bit分解能 / 通常動作 で初期化
static bool mcp9600Init(uint8_t addr) {
  uint8_t idBuf[2] = {0, 0};
  if (!softI2CReadReg(addr, MCP9600_DEVICEID, idBuf, 2)) return false;
  if (idBuf[0] != 0x40) return false; // MCP9600のデバイスID上位byte

  if (!softI2CWriteReg(addr, MCP9600_SENSORCONFIG, 0x03)) return false; // K型(0)+filter3
  if (!softI2CWriteReg(addr, MCP9600_DEVICECONFIG, 0x80)) return false; // 18bit+通常動作
  return true;
}

static float mcp9600ReadTemp(uint8_t addr, uint8_t reg) {
  uint8_t buf[2];
  if (!softI2CReadReg(addr, reg, buf, 2)) return NAN;
  int16_t raw = (int16_t)((buf[0] << 8) | buf[1]);
  return raw * 0.0625f; // 0.0625°C/LSB
}

static uint8_t mcp9600ReadStatus(uint8_t addr) {
  uint8_t st = 0;
  softI2CReadReg(addr, MCP9600_STATUS, &st, 1);
  return st;
}

// ============================================================
// MCP23008 — GP0/GP1/GP2 で 4051 MUX のチャンネルを選択
// ============================================================
static bool mcpWrite(uint8_t reg, uint8_t val) {
  return softI2CWriteReg(MCP23008_ADDR, reg, val);
}

// GP0-GP2 を出力に設定
static bool mcpInit() {
  return mcpWrite(MCP_IODIR, 0xF8);  // 下位3bit=出力, 残りは入力のまま
}

// MUX チャンネル(0-7)を選択（S0=GP0, S1=GP1, S2=GP2）
static bool muxSelect(uint8_t ch) {
  return mcpWrite(MCP_GPIO, ch & 0x07);
}

// ============================================================
// I2C スキャン（0x08〜0x77 全域、ソフトウェアI2C）
// ============================================================
static void i2cScan() {
  Serial.println("[I2C スキャン]");
  uint8_t found = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    if (softI2CProbe(addr)) {
      Serial.print("  検出: 0x");
      if (addr < 0x10) Serial.print("0");
      Serial.print(addr, HEX);
      if (addr == MCP23008_ADDR)         Serial.print("  ← MCP23008 (MUXセレクト)");
      else if (addr == ADS1115_ADDR)     Serial.print("  ← ADS1115 (±10V)");
      else if (addr >= 0x60 && addr <= 0x67) Serial.print("  ← MCP9600 (熱電対)");
      Serial.println();
      found++;
    }
  }
  if (found == 0) {
    Serial.println("  [ERROR] デバイスが1つも見つかりません → 配線・電源を確認");
  } else {
    Serial.print("  合計 "); Serial.print(found); Serial.println(" 台");
  }
  Serial.println();
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Monita Flex 横河基板 ver1.0 — bring-up ===");
  Serial.println("XIAO ESP32-C3  SDA=D4  SCL=D5（ソフトウェアI2C）");
  Serial.println();

  softI2CInit(PIN_SDA, PIN_SCL);
  delay(100);

  i2cScan();

  if (mcpInit()) {
    Serial.println("[OK] MCP23008 初期化完了（GP0-2 出力）");
  } else {
    Serial.println("[ERROR] MCP23008 初期化失敗");
  }

  g_mcp9600_addr = mcp9600Scan();
  if (g_mcp9600_addr == 0) {
    Serial.println("[WARN] MCP9600 未検出 → CH8温度は読みません");
  } else {
    g_mcp9600_ok = mcp9600Init(g_mcp9600_addr);
    if (g_mcp9600_ok) {
      Serial.print("[OK] MCP9600 初期化完了（0x");
      Serial.print(g_mcp9600_addr, HEX);
      Serial.println("）");
    } else {
      Serial.println("[ERROR] MCP9600 初期化失敗（デバイスID不一致）");
    }
  }
  Serial.println();

  // HX711 の制御ピン
  pinMode(HX711_PD_SCK, OUTPUT);
  pinMode(HX711_DOUT, INPUT);
  digitalWrite(HX711_PD_SCK, LOW);

  Serial.println("MUX を CH0→CH4 まで順に切り替えます（テスターで 4051 出力を確認）");
  Serial.println("---------------------------------------------------");
}

void loop() {
  // MUX チャンネルを1秒ごとに巡回（CH1〜CH5 = MUX 0〜4）
  for (uint8_t ch = 0; ch <= 4; ch++) {
    if (muxSelect(ch)) {
      Serial.print("[MUX] CH"); Serial.print(ch + 1);
      Serial.print(" 選択 (S2S1S0=");
      Serial.print((ch >> 2) & 1); Serial.print((ch >> 1) & 1); Serial.print(ch & 1);
      Serial.println(")");
    } else {
      Serial.print("[MUX] CH"); Serial.print(ch + 1); Serial.println(" 選択失敗");
    }
    delay(1000);
  }

  // CH6: MCP9600 熱電対（K型）
  if (g_mcp9600_ok) {
    float hot  = mcp9600ReadTemp(g_mcp9600_addr, MCP9600_HOTJUNCTION);
    float cold = mcp9600ReadTemp(g_mcp9600_addr, MCP9600_COLDJUNCTION);
    uint8_t st = mcp9600ReadStatus(g_mcp9600_addr);

    Serial.print("[CH6] HOT: "); Serial.print(hot, 2);
    Serial.print(" C  COLD: "); Serial.print(cold, 2);
    Serial.println(" C");

    if (st & 0x02) Serial.println("  [WARN] オープン回路（熱電対未接続または断線）");
    if (st & 0x01) Serial.println("  [WARN] ショート検出（GNDまたはVCC）");
  }

  Serial.println("---------------------------------------------------");
}
