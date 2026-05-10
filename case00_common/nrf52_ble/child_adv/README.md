---
title: BLE 子機 — アドバタイズに Manufacturer Data（nRF52840）
tags:
  - BLE
  - アドバタイジング
  - Manufacturer Data
  - nRF52840
  - XIAO
  - Bluefruit
  - Monita
  - 案件5
  - BLE公園人流
updated: 2026-04-30
---

# pio_nrf52_ble_child_adv_mfrdata

**Manufacturer Specific Data** に任意のバイト列（サンプルでは ASCII 文字列）を載せ、一定間隔でアドバタイズを更新する **BLE 子機** サンプル。

## 設定（`src/main.cpp`）

- `DEVICE_NAME` … デバイス名（`Bluefruit.setName`、Scan Response の名前）
- `ADV_INTERVAL_MS` … 再アドバタイズ周期（ミリ秒）
- `MFR_PAYLOAD` / `MFR_COMPANY_ID` … MSD 先頭 2 バイト + ペイロード

Company ID `0xFFFF` はテスト用。製品では **Bluetooth SIG 割当の Company Identifier** を使うこと。

## ビルド

```bash
cd firmware/pio_nrf52_ble_child_adv_mfrdata
pio run
```

## 依存

Seeed `platform-seeedboards` 上の **Arduino + Adafruit Bluefruit nRF52**。USB Serial 用に `Adafruit_TinyUSB.h` をインクルードしている（コアの USB 構成に依存）。
