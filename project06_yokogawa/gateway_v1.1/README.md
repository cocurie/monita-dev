---
title: Monita Gateway v1.1 ファームウェア（横河ブリッジHD案件専用・8CH対応）
domain: iot_device
tags: [gateway, nRF52840, SIM7080G, BLE, LTE-M, DS3231, SD, GAS, DIP, 横河ブリッジHD, 8ch]
updated: 2026-08-05
---

# gateway_v1.1（横河ブリッジHD案件専用）

**★2026-08-05: `case02_Gateway/firmware/gateway_v1.1`（汎用・v3.03/4CH固定）から分岐した横河ブリッジHD案件専用版。**
横河ver1.1フィールドユニット（[`project06_yokogawa/ver1.1`](../ver1.1/)、8CH構成・`BLE_PKT_TYPE=0x11`）のBLEアドバタイズを受信し、
LTE-M経由でGAS（Google Apps Script）に送信するGatewayファームウェア。

汎用版との主な違い（詳細は`src/main.cpp`冒頭コメント参照）:
- `EXPECTED_PKT_TYPE = 0x11`（汎用版の`0x03`＝Flex v3.03用とは非互換）
- GAS送信ペイロードがCH1〜8（横河のFW_VERSIONバイト無しレイアウトに対応、Epoch+DeviceID+CH1-8=21バイト/台=42 hex文字）

Monita Flex（子機）から **BLE アドバタイジング** で受信したセンサデータを LTE-M 経由で GAS に送信する。
**電源は XIAO nRF52840 の Type-C 給電（AC電源）、全部品 DIP 対応。**
LoRa受信コード（`COMM_MODE_LORA`）は汎用版から引き継いだままだが、横河ver1.1フィールドユニットはBLEのみ対応のため未使用（`platformio.ini`は`COMM_MODE_BLE`固定）。

## ⚠️ 未対応・要作業（2026-08-05時点）

- **`GAS_SCRIPT_ID`が横河専用のものに未設定**（現状NEXCO案件のGASを指したまま）。**必ず横河専用のGASデプロイURLに差し替えること**
- **GAS側スクリプトが新しい8CH・42 hex文字フォーマットに未対応**。GAS側に`parseGatewayV11Record`相当の8CH版パーサーとスプレッドシート列（CH1〜8）の追加が必要

要件定義（汎用版ベース）: `【7】Monita/開発/Gatway基板/gateway_requirements_v1.10.md`
汎用版との差分の経緯: `src/main.cpp`冒頭コメント参照
対応するフィールドユニット: [`project06_yokogawa/ver1.1`](../ver1.1/)

## ハードウェア構成

| 役割 | 部品 |
|------|------|
| MCU | Seeed XIAO nRF52840 |
| 通信 | M5Stamp CAT-M（SIM7080G）、（LoRaビルドのみ）E220-900T22S(JP)-EV2 |
| RTC | DS3231 |
| ストレージ | microSD（SPI） |
| 電源 | XIAO nRF52840 Type-C給電（AC/USBアダプタ）。全部品DIP |

## 配線（v1.1、AC電源版）

| 信号 | XIAO ピン | 接続先 |
|------|-----------|--------|
| UART TX | D6 | SIM7080G RX |
| UART RX | D7 | SIM7080G TX |
| I2C SDA | D4 | DS3231 SDA |
| I2C SCL | D5 | DS3231 SCL |
| SPI SCK | D8 | SD CLK |
| SPI MISO | D9 | SD DAT0 |
| SPI MOSI | D10 | SD CMD |
| SD CS | D3 | SD CS（直結、net N$6） |
| LoRa RX（LoRaビルドのみ） | D0 | E220 TXD（net UART_RX_2） |
| LoRa TX（LoRaビルドのみ） | D1 | E220 RXD（net UART_TX_2） |
| LoRa M0/M1（LoRaビルドのみ） | D2 | E220 M0・M1（基板側で両ピンを短絡し1本のGPIOで共通駆動、net LORA_SETTING） |
| 5V | 5V | SIM7080G 5V（Type-C給電時のみ通電、v1.0と同じ直結） |
| 3V3 | 3V3 | DS3231 VCC / SD VDD / E220 VCC |

**★2026-07-19**: 回路図 `ver1.10.sch`（netlist_gateway_1）に合わせてピン割当を確定。E220のM0/M1は基板上で短絡済み（LORA_SETTINGネット）。

**⚠️ 実機未検証**: UARTE1経由のLoRa受信は基板完成前のため実機での動作確認が済んでいない。

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

`platformio.ini` 末尾の `build_flags` で `COMM_MODE_BLE` / `COMM_MODE_LORA` のいずれか1つのコメントを外して選択する（既定はBLE）。

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

- ファームウェアは v1.0 で確立した通信安定化策一式（WDT・再送キュー・BLEフィルタ・BLEスキャン前倒し）をベースラインとして継承
- BLE / LoRa（E220-900T22S(JP)）をビルド時選択（`COMM_MODE_BLE` / `COMM_MODE_LORA`）で追加
- 電源はAC電源（XIAO Type-C給電）、全部品DIP。SIM7080GはXIAOの5Vに直結（v1.0と同じ方式）
- （旧検討）LiPoバッテリー駆動＋TCA9534によるSMD化は `gateway_v1.10_ARCHIVE_battery_TCA9534_design.md` にアーカイブ済み
- **★2026-07-18**: DS3231の網時刻自動設定（`AT+CCLK?`）、BLE MSD解析の境界チェック、LoRa送信中取りこぼし対策、LoRa設定書込検証、送信バッチの8台分割を追加
- **★2026-07-18**: コントローラー連携（BLE設定）をLoRaビルドに追加。送信間隔変更・コマンド（即時送信/NW再登録/起動確認/リセット）・ステータス通知をBLE GATTで提供。詳細は要件定義「コントローラー連携（BLE設定）」章
- **★2026-07-19**: 回路図 ver1.10.sch に合わせてピン割当を確定。SD CS=D3、LoRa RX=D0/TX=D1/M0M1=D2
- **★2026-07-21**: アプリ層ウォッチドッグを追加。30分間GAS送信成功が無ければ `NVIC_SystemReset` で強制再起動。ハードWDTでは捕捉できない「モデム接続維持のまま送信だけ失敗し続けるソフトハング」への対策（有野川現場2026-07-17停止の教訓）
- **★2026-07-21**: 段階的復旧を追加。送信が3サイクル連続で全滅したら、全再起動の前に**モデムのソフトリセット（`CFUN=0/1`＋再接続）**を先に試す（`modemSoftReset()`）。SSL/HTTPスタック固着への軽量・高速な一段目の復旧手段
- **★2026-07-21**: `sendAT()` の応答バッファに上限（2048バイト、`SENDAT_MAX_RESPONSE_LEN`）を追加（`GATEWAY_FW_VERSION` 7）。配線ノイズ等でRX1にゴミデータが流れ込み続けた場合のヒープ枯渇を防止。電波状況良好な卓上テストでも発生した突然停止の根本原因と推定

## 関連タスク

- `tsuruta_tasks.md` — Monita Gateway 開発タスク
- `【7】Monita/開発/Gatway基板/gateway_requirements_v1.10.md` — 要件定義（v1.1）
- `【7】Monita/開発/Gatway基板/gateway_requirements_v1.00.md` — 要件定義（v1.0、旧版）
