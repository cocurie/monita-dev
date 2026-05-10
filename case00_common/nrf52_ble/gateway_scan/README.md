---
title: BLE 親機 — スキャン＋Manufacturer Data ログ
tags: [BLE, Central, スキャン, nRF52840, Monita, 案件5, BLE公園人流]
status: active
updated: 2026-05-03
---

# pio_nrf52_ble_gateway_scan

**BLE 親機（ゲートウェイ）**: パッシブスキャンでアドバタイズを受信し、Manufacturer Specific Data（Company ID / Payload）をシリアルにログ出力する。

- `platformio.ini` … 子機 `pio_nrf52_ble_child_adv_mfrdata` と同系の Seeed XIAO nRF52840 向け設定
- `src/main.cpp` … アドバタイズ受信・RSSI / CompID / Payload フィルタ・ハートビート

実装参考（別案件）: [BLE公園人流測定/firmware/xiao_ble_scan](../../../BLE公園人流測定/firmware/xiao_ble_scan/)
