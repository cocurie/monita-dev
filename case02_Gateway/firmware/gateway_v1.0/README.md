---
title: Monita Gateway v1 ファームウェア
domain: iot_device
tags: [gateway, nRF52840, SIM7080G, BLE, LTE-M, DS3231, SD, GAS]
updated: 2026-06-24
---

# gateway_v1

Monita Flex（子機）から BLE アドバタイジングで受信したセンサデータを LTE-M 経由で GAS（Google Apps Script）に送信する Gateway ファームウェア。

## ハードウェア構成

| 役割 | 部品 |
|------|------|
| MCU | Seeed XIAO nRF52840 |
| 通信 | M5Stamp CAT-M（SIM7080G） |
| RTC | DS3231 |
| ストレージ | microSD（SPI） |

## 配線

| 信号 | XIAO ピン | 接続先 |
|------|-----------|--------|
| UART TX | D6 | SIM7080G RX |
| UART RX | D7 | SIM7080G TX |
| 5V | 5V（VBUS） | SIM7080G 5V |
| I2C SDA | D4 | DS3231 SDA |
| I2C SCL | D5 | DS3231 SCL |
| SD CS | D3 | SD CD/DAT3 |
| SPI SCK | D8 | SD CLK |
| SPI MISO | D9 | SD DAT0 |
| SPI MOSI | D10 | SD CMD |
| 3V3 | 3V3 | DS3231 VCC / SD VDD |

## SIM 切り替え

`src/main.cpp` の冒頭の define を切り替える：

```cpp
#define SIM_1NCE    // 1NCE SIM を使う場合
// #define SIM_PLAN_D  // Plan-D SIM を使う場合
```

Plan-D の APN は `planex.net` としているが、正式 APN が確定したら修正すること。

## GAS 設定

`GAS_SCRIPT_ID` にデプロイ URL の ID 部分（`AKfycb...`）を設定する。

GAS 側の `doGet(e)` で受け取るパラメータ：

| パラメータ | 内容 |
|-----------|------|
| `ts` | タイムスタンプ（DS3231 / millis フォールバック） |
| `mac` | Flex の MAC アドレス |
| `payload` | Manufacturer Data のペイロード（HEX 文字列） |
| `rssi` | 受信 RSSI (dBm) |
| `sim` | 使用 SIM 名 |

## ビルド

```bash
cd firmware/gateway_v1
pio run
pio run --target upload
```

## 動作フロー

1. 起動時に DS3231・SD・BLE・SIM7080G を初期化
2. BLE パッシブスキャンで Flex のアドバタイジングを常時受信
3. 5分ごとにバッファをフラッシュし、各 Flex レコードを GAS に HTTP GET 送信
4. 送信前に SD カード（`gateway.csv`）にバックアップ記録

## 関連タスク

- `tsuruta_tasks.md` — Monita Gateway 開発タスク
- `【7】Monita/開発/Gatway基板/gateway_requirements_v1.00.md` — 要件定義
