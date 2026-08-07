---
title: Gateway テストスケッチ インデックス
domain: iot_device
tags: [gateway, nRF52840, experimental]
updated: 2026-08-07
---

# Gateway テストスケッチ

Monita Gateway 基板の単体機能検証スケッチ一覧。

| フォルダ | 内容 |
|----------|------|
| [01_sd_write](01_sd_write/) | SD カード読み書き・速度計測（2026-07-01） |
| [02_sd_ltem](02_sd_ltem/) | SD + LTE-M（SIM7080G / 1NCE）統合テスト・GAS ダミー送信（2026-07-01） |
| [03_lora_downlink_sender](03_lora_downlink_sender/) | Flex子機（`case01_Flex/test_sketches/25_lora_downlink_child`）宛てのダウンリンク送信テストツール。D0ボタンが無いためSEND_INTERVAL_MS間隔で自動連続送信（2026-08-07） |
