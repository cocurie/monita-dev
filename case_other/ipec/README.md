---
title: iPEC サーバー通信検証
domain: case_other
tags: [ipec, lte-m, http-post, sim7080g, json]
updated: 2026-06-14
---

# iPEC サーバー通信検証

## 背景

アイペック社がコクリエの Monita デバイスに関心を持っており、
**iPEC 社のクラウドサーバーへ直接 POST できるか** を検証する。

打ち合わせログ: `【1】個別案件/その他/ipec/20260610_打ち合わせログ.md`

## 検証ステップ

| Step | フォルダ | 内容 | 状態 |
|------|---------|------|------|
| 1 | `01_http_post/` | Flex v3.10（LoRa送信）→ Gateway v1.1（LoRa受信＋LTE-M POST）→ iPEC | 🔲 未検証（実機統合済み、テスト待ち） |

## 構成（2026-08-06更新）

ダミーJSON単体送信から、**実機のLoRa受信値を使ったエンドツーエンドテスト**に変更した。

```
Flex ver3.10（子機、case01_Flex/test_sketches/18_lora_child）
  └─ LoRa送信（DeviceID・CH1-4・電池電圧）
       ↓
Gateway ver1.1（親機、本フォルダ 01_http_post）
  ├─ LoRa受信（19_lora_parentと同一ロジック、UARTE1）
  └─ 受信値をJSONへ詰めてSIM7080G(LTE-M)経由でiPECへPOST（既存ロジック流用、Serial1/UARTE0）
```

- 対の子機のDeviceIDは `01_http_post/src/main.cpp` の `TARGET_DEVICE_ID`（現在 `0x0E`）と一致させること
- 最大送信回数は `TOTAL_SEND`（現在5回）。受信次第POSTし、規定回数で試験終了・サマリー表示

## エンドポイント

```
https://jg9v8fcum8.execute-api.ap-northeast-1.amazonaws.com/dev
```

- Method: POST
- Content-Type: application/json
- x-api-key: `IPEC_API_KEY`（コード内に設定済み）
- 方式: raw TCP + SSL（`AT+CAOPEN`/`AT+CASEND`。SHHTTPクライアントはJSON内のダブルクォートに対応できないため不採用）

## JSON ボディ（LoRa受信値ベース）

```json
{
  "device": "monita-flex-001",
  "time": 1716000060,
  "ch1": 123, "ch2": 223, "ch3": 323, "ch4": 423,
  "batt": 3300,
  "rssi": -42
}
```

キー名は iPEC 側スキーマに合わせて調整する。

## ハード構成

- 親機: Monita Gateway ver1.1 基板（XIAO nRF52840、SIM7080G、E220-900T22S(JP)）
- 子機: Monita Flex ver3.10 基板（XIAO nRF52840、E220-900T22S(JP)、TCA9534経由M0/M1制御）
- 1NCE IoT SIM (APN: iot.1nce.net)
