# minatogawa_v7

**阪神高速 湊川IC付近 多チャンネル歪み計測システム ver 7**

## 概要

| 項目 | 内容 |
|------|------|
| MCU | Arduino Mega 2560 |
| センサ | HX711 × 20ch |
| 通信モジュール | M5Stamp SIM7080G (CAT-M1) |
| SIM | SORACOM (APN: soracom.io) |
| 送信先 | SORACOM Unified Endpoint (uni.soracom.io:23080, TCP) |
| 送信間隔 | 100分 |
| ビルド環境 | PlatformIO + Arduino framework |

## v6 からの変更点

- **通信モジュール**: SARA-R410M → M5Stamp SIM7080G
- **SIM**: SORACOM (APN: soracom.io)
- **ATコマンド**: SARA系 → SIM7080G系（CAOPEN / CASEND / CACLOSE）
- **AT応答チェック追加**（v6 は読み捨てのみ）
- **QuickStats 依存廃止**（median 内部実装、SRAM 約 380byte 節約）
- **HX711 ピン配置は v6 から変更なし**

> **SoftwareSerial (D10/D11) を継続使用する理由**
> Mega のハードウェア UART ピンは HX711 と全て競合している。
> Serial1=D18/D19 (CH16), Serial2=D16/D17 (CH15), Serial3=D14/D15 (CH14)。
> D10/D11 は HX711 未使用のため SoftwareSerial を引き続き採用。
> 9600 baud に下げて安定性を確保。

## ピン配置

### HX711（v6 から変更なし）

| CH | DATA | CLK |
|----|------|-----|
| 1 | A0 | A1 |
| 2 | A2 | A3 |
| 3 | A4 | A5 |
| 4 | A6 | A7 |
| 5 | A8 | A9 |
| 6 | A10 | A11 |
| 7 | A12 | A13 |
| 8 | A14 | A15 |
| 9 | D2 | D3 |
| 10 | D4 | D5 |
| 11 | D6 | D7 |
| 12 | D8 | D9 |
| 13 | D12 | D13 |
| 14 | D14 | D15 |
| 15 | D16 | D17 |
| 16 | D18 | D19 |
| 17 | D22 | D23 |
| 18 | D24 | D25 |
| 19 | D26 | D27 |
| 20 | D28 | D29 |

### SIM7080G (SoftwareSerial)

| Mega ピン | SIM7080G |
|-----------|----------|
| D11 (SW-TX) | RXD |
| D10 (SW-RX) | TXD |
| D36 | PWR |
| 5V | VCC |
| GND | GND |

## ビルド

```bash
pio run
pio run --target upload
pio device monitor
```

## ゼロ点補正

`src/main.cpp` の `zeroModification = true` に変更して書き込むと EEPROM にゼロ点を保存。確認後 `false` に戻して本番運用。
