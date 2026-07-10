---
title: Monita Gateway v1.1 ファームウェア
domain: iot_device
tags: [gateway, nRF52840, SIM7080G, BLE, LTE-M, DS3231, SD, GAS, SMD]
updated: 2026-07-09
---

# gateway_v1.1

Monita Flex（子機）から BLE アドバタイジングで受信したセンサデータを LTE-M 経由で GAS（Google Apps Script）に送信する Gateway ファームウェア。
**v1.0（DIP試作）をベースに、基板の SMD化・小型化に対応する版。** ファームウェアの機能自体は v1.0 で確立した内容を踏襲している。

要件定義: `【7】Monita/開発/Gatway基板/gateway_requirements_v1.10.md`
v1.0からの差分: `【7】Monita/開発/Gatway基板/gateway_v1.00_to_v1.10_diff.md`

## ハードウェア構成

| 役割 | 部品 |
|------|------|
| MCU | Seeed XIAO nRF52840 |
| 通信 | M5Stamp CAT-M（SIM7080G） |
| RTC | DS3231 |
| ストレージ | microSD（SPI） |

## 配線（v1.0から変更なし。SMD化時に再確認要）

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

## GAS 設定

`GAS_SCRIPT_ID` にデプロイ URL の ID 部分（`AKfycb...`）を設定する。

GAS 側の `doGet(e)` は「子機データ行」と「起動確認用の設定情報行」の2種類を受ける：

### 子機データ（通常送信・複数台バッチ対応）

| パラメータ | 内容 |
|-----------|------|
| `ts` | タイムスタンプ |
| `sim` | 使用 SIM 名 |
| `csq` | SIM7080G 自身のセルラー受信電波強度（0-31, 99=圏外） |
| `n` | このリクエストに含まれる Flex 台数 |
| `m{i}` | i番目の Flex の MAC アドレス |
| `p{i}` | i番目の Manufacturer Data ペイロード（HEX文字列） |
| `r{i}` | i番目の BLE RSSI (dBm) |

payload は GAS 側で PktType・DeviceID・CH1〜CH6・FlexHour/Min に汎用パースする（バイト長からチャンネル数を自動判定。将来12バイト/6チャンネル拡張に対応）。

### 起動確認情報行（`row_type=info`）

| パラメータ | 内容 |
|-----------|------|
| `xiao_id` | XIAO nRF52840 固有ID（FICR DEVICEID） |
| `sim_imei` | SIM7080G の IMEI |
| `sd` | SDカード記録の有無（0/1） |
| `interval_min` | 定期送信インターバル（分） |
| `devcount` | 起動時点で受信済みの子機台数 |

## ビルド

```bash
cd firmware/gateway_v1.1
pio run
pio run --target upload
```

## 動作フロー

1. 起動時に DS3231・SD・BLE・SIM7080G を初期化
2. SIM7080G 起動待ち完了直後から BLE スキャン開始（ネットワーク初期化と並行）
3. ネットワーク接続完了後、起動確認情報行＋受信済み子機データを送信
4. 以降は一定間隔（`SEND_INTERVAL_MS`）でバッファをフラッシュし、複数 Flex をまとめて1回の GET で GAS に送信
5. 送信失敗時は再送キューに保持し、次回サイクルでライブデータとマージして再送
6. 送信前に SD カード（`gateway.csv`）にバックアップ記録
7. ウォッチドッグタイマー（120秒）により無人運用中のハングから自動復旧

## v1.0 からの主な変更点

詳細は `gateway_v1.00_to_v1.10_diff.md` を参照。要点：

- 基板の SMD化・小型化（回路設計は別途進行）
- ファームウェアは v1.0 で確立した通信安定化策一式（WDT・再送キュー・BLEフィルタ・BLEスキャン前倒し・DS3231自動時刻設定）をベースラインとして継承

## 関連タスク

- `tsuruta_tasks.md` — Monita Gateway 開発タスク
- `【7】Monita/開発/Gatway基板/gateway_requirements_v1.10.md` — 要件定義（v1.1）
- `【7】Monita/開発/Gatway基板/gateway_requirements_v1.00.md` — 要件定義（v1.0、旧版）
