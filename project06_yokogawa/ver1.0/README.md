---
title: Monita Flex 横河基板 ver1.0 ファーム
domain: monita_dev
tags:
  - Monita Flex
  - yokogawa
  - ver1.0
  - XIAO ESP32-C3
  - HX711
  - MCP23008
  - MCP9600
  - ADS1115
  - SD
updated: 2026-07-09
---

# Monita Flex 横河基板 ver1.0

横河ブリッジHD フェーズ1 リモート計測基板（**ver1.0 実基板**）のファーム。
`netlist_yokogawa_v1`（2026/07/09 エクスポート）に基づく。

WiFi 疎通テスト用の試作コードは `../monita-flex-wifi/` にそのまま残す。
本フォルダは **実基板を動かす正本**として開発を進める。

## 対象ハード

- **Seeed XIAO ESP32-C3**
- Monita Flex 横河基板 ver1.0

## センサー構成（フェーズ1 8CH）

| CH | 種別 | 経路 |
|----|------|------|
| CH1〜CH5 | ひずみゲージ/ロードセル/変位（HX711） | 4051 MUX → DOUT |
| CH6 | 熱電対 K型（MCP9600） | I2C（JP17） |
| CH7〜CH8 | ±10V 電圧（ADS1115 差動） | I2C（JP18 の ADC モジュール） |

※ HX711 は 5CH を SN74LV4051（U2）で MUX 切替。セレクトは MCP23008（U1）の GP0-2。

## ピン割当（netlist_yokogawa_v1 確定）

| 信号 | XIAO | GPIO | コネクタ |
|------|------|------|----------|
| HX711 PD_SCK | D1 | GPIO3 | JP10-2 |
| HX711 DOUT | D2 | GPIO4 | JP10-3（MUX Z 出力）|
| I2C SDA | D4 | GPIO6 | JP10-5 |
| I2C SCL | D5 | GPIO7 | JP10-6 |
| SD SPI_CS | D7 | GPIO20 | JP11-1 |
| SD SPI_SCK | D8 | GPIO8 | JP11-2（strapping / R5 プルアップ）|
| SD SPI_MISO | D9 | GPIO9 | JP11-3 |
| SD SPI_MOSI | D10 | GPIO10 | JP11-4 |
| strapping | D0 | GPIO2 | JP10-1（R1 で 3V3 固定・FW未使用）|

### I2C デバイス

| デバイス | アドレス | 役割 |
|----------|----------|------|
| MCP23008（U1） | 0x20 | GP0-2 → 4051 MUX セレクト |
| MCP9600 | 0x60〜0x67（ADDR依存） | CH6 K型熱電対 |
| ADS1115 モジュール | 0x48 | CH7/CH8 ±10V 差動 |

I2C プルアップは基板側 R2（SCL）/R3（SDA）4.7kΩ。

### なぜ全デバイスをソフトウェアI2Cで通信しているか

ESP32-C3 のハードウェアI2Cペリフェラルはクロックストレッチング用タイムアウトカウンタが
**5bit**しかない（`esp32-hal-i2c.c` のコメントに明記。ESP32本体は20bit、ESP32-S2は24bit）。
MCP9600はクロックストレッチングを使うため、このハード制約で `Wire`（ハードウェアI2C）
経由では通信できないことを実機検証で確認した（同一基板・同一配線でXIAO nRF52840に
差し替えると正常動作した）。

ハードウェアWireとソフトI2Cを同一ピンで都度切り替える方式（`Wire.end()/begin()`）は
ペリフェラルの明け渡しタイミングが不安定だったため、`src/soft_i2c.cpp` の
ビットバングI2CでMCP23008・MCP9600とも統一して通信している。

## ビルド

```bash
cd project06_yokogawa/ver1.0
pio run
pio run -t upload
pio device monitor
```

## 開発ステップ

1. **bring-up（現在）** … I2C 3デバイス検出 + MCP23008 で MUX 切替確認
2. HX711 5CH 読み取り（MUX 巡回）
3. ADS1115 ±10V 2CH 差動読み取り
4. MCP9600 熱電対読み取り
5. SD カード記録（CSV）
6. BLE NUS 設定受信（`monita-flex-wifi` から移植）
7. WiFi 送信

## 関連

- 試作（WiFi/BLE）: [`../monita-flex-wifi/`](../monita-flex-wifi/)
- 検証スケッチ: `case01_Flex/test_sketches/`（15_mcp9600 / 16_ADS1115）
