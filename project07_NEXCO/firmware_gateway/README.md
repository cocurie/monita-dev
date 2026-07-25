---
title: project07_NEXCO Gateway ファームウェア
domain: iot_device
tags: [NEXCO, gateway, nRF52840, SIM7080G, BLE, LTE-M, DS3231, SD, GAS, 剥落検知]
updated: 2026-07-25
---

# project07_NEXCO firmware_gateway

中国自動車道有野川付近RC床版 コンクリート剥落モニタリング向け Gateway ファームウェア。

**ベース**: `case02_Gateway/firmware/gateway_v1.1`

---

## 対象ハード

| 役割 | 部品 |
|------|------|
| MCU | Seeed XIAO nRF52840 |
| 通信 | M5Stamp CAT-M（SIM7080G）|
| RTC | DS3231 |
| ストレージ | microSD（SPI） |
| 電源 | XIAO nRF52840 Type-C給電（AC電源）|

通信モード: **BLE のみ**（LoRa ビルドは本案件では使用しない）

---

## v1.1 からの変更点（NEXCO 固有）

### ペイロードパーサー（`buildBatchQuery()`）

子機ファーム（`firmware/`）の新フォーマットに対応：

| バイト（MSD内 CompanyID除く） | 内容 | 型 |
|---|---|---|
| [0] | PktType `0x03` | `uint8_t` |
| [1] | DeviceID | `uint8_t` |
| [2] | 温度 | `int8_t`（整数℃） |
| [3-10] | CH1〜4 メジアン | `int16_t` LE ×4 [με] |
| [11-18] | CH1〜4 Max | `int16_t` LE ×4 [με] |
| [19-26] | CH1〜4 Min | `int16_t` LE ×4 [με] |

GAS へ送る hex blob（1台あたり38文字）:
`DeviceID(2) + Temp(2) + CH1-4med(16) + CH1-4Max(16) + CH1-4Min(16)`

### 計測方式

子機: `SAMPLES_PER_AVG=5` サンプル平均 × `MEASURE_COUNT=10` 回 → メジアン/最大/最小

---

## ビルド

```bash
cd project07_NEXCO/firmware_gateway
pio run
pio run --target upload
```

`platformio.ini` の `build_flags`: `COMM_MODE_BLE` 固定。

---

## バージョン管理

`src/main.cpp` を変更して commit するたびに `GATEWAY_FW_VERSION` を +1 すること。

---

## 関連ドキュメント

- 子機ファーム: `project07_NEXCO/firmware_child/`
- GAS スクリプト: `case02_Gateway/gas/project07_nexco/Code.gs`（要更新）
- 設計メモ: `GoogleDrive/【7】Monita/開発/案件/project07_NEXCO/剥落検知センサ設計メモ.md`
- ベース: `case02_Gateway/firmware/gateway_v1.1/`
