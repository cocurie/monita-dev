---
title: LoRa複数台テスト用 GAS（受信・記録）
domain: iot_device
tags: [experimental, lora, e220, gateway, gas, xiao_nrf52840]
updated: 2026-07-24
---

# LoRa複数台テスト用 GAS

`22_lora_multi_child`（子機・ダミーデータ）↔ `gateway_v1.1`（COMM_MODE_LORA）による
13台LoRa疎通テストで、GatewayがLTE-M経由でGASへ送るデータを受け取り、
スプレッドシートに記録するための**テスト専用の最小GAS**。

> ⚠️ 本番のproject07_NEXCO用GAS（`case02_Gateway/gas/project07_nexco/Code.gs`、
> BLE版26B/台フォーマット）とは**別物**。アラート判定・デバイス別シート振り分けは行わず、
> 全台を1シート（`lora_test`）に素直に記録するだけ。

## 受け取るクエリ形式（Gateway `gateway_v1.1` 由来）

Gatewayの `buildBatchQuery()` / `postBootInfoRow()` が送る形式に対応:

- **データ行**（`row_type`なし）
  - `q` = CSQ（1バイトを16進2文字。10進は `parseInt(q,16)`）
  - `n` = 台数
  - `d` = 各台 9バイト=18hex文字の連結。1台の内訳:
    `DeviceID(1B) + CH1(int16 LE) + CH2 + CH3 + CH4`
- **info行**（起動確認、`row_type=info`）
  - `ts, sim, csq, xiao_id, sim_imei, sd, interval_min, devcount, gw_fw`

※子機の元ペイロードは19バイト（PktType/FW/BATT/Hour/Min/Range含む）だが、
Gatewayが送信時に `DeviceID + CH1-4` の9バイトだけ抜き出して `&d=` に圧縮している。
BATT/FW/時刻/Range/RSSIはこのテストでは送られてこない。

## 記録先シート `lora_test` の列構成

| A | B | C | D | E | F | G | H |
|---|---|---|---|---|---|---|---|
| 受信日時 | DeviceID | CH1 | CH2 | CH3 | CH4 | CSQ(10進) | 備考 |

- データ行: B=`0x0B`等、C〜F=ダミーCH値、G=CSQ
- info行: B=`GW`、C〜F空欄、H=`fw.. xiao=.. imei=.. sd=.. interval_min=.. devcount=..`

## セットアップ

1. 記録先のGoogleスプレッドシートを用意する。
2. スプレッドシートに紐付け（拡張機能 → Apps Script）する場合は `Code.gs` を貼り、
   `SPREADSHEET_ID` は空文字のままでよい。スタンドアロンスクリプトの場合は
   `SPREADSHEET_ID` に記録先スプレッドシートのIDを設定する。
3. 「デプロイ → 新しいデプロイ → ウェブアプリ」。アクセスは「全員」。
4. 発行された `/exec` URLの末尾ID（`AKfycb...`）を、Gatewayファーム
   `case02_Gateway/firmware/gateway_v1.1/src/main.cpp` の `GAS_SCRIPT_ID` に設定する。
   - ★現在 `GAS_SCRIPT_ID` は本番/別テスト用のIDになっている可能性があるため、
     このテスト用に差し替えるか、テスト用GASのIDに向いているか必ず確認すること。

## 動作確認

Gatewayログに `[LORA] フレーム受信 Device ID=0x.. RSSI=..dBm` が出て、
定期送信で `✓ GAS 送信成功！` が出れば、`lora_test` シートに行が増える。
13台テストでは、`devcount` と実際に記録される DeviceID の種類数が一致するか確認する。

## 関連

- 子機: `../src/main.cpp`（22_lora_multi_child）
- Gateway: `case02_Gateway/firmware/gateway_v1.1/src/main.cpp`
- CSQの意味: 開発メモ `gateway_csq_signal_strength_notes`
- 顛末: 開発メモ `lora_multichild_gateway_e220_freq_beat`（M0/M1未短絡でWOR受信化していた件）
