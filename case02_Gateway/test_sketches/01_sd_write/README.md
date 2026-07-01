---
title: Gateway Step01 — SD カード書き込みテスト
domain: iot_device
tags: [gateway, nRF52840, SD, SPI, experimental]
updated: 2026-07-01
---

# Gateway Step01: SD カード書き込みテスト

## 目的

Monita Gateway 基板の microSD スロットに対して、読み書き・追記・速度計測が正常に動作することを確認する。

## ハード

| 項目 | 内容 |
|------|------|
| MCU | Seeed XIAO nRF52840 |
| SD スロット | SPI 接続（D3=CS、D8=SCK、D9=MISO、D10=MOSI）|
| 電源 | XIAO 3V3 → SD VDD |

## 配線

```
SD CS   → XIAO D3
SD CLK  → XIAO D8  (SCK)
SD DAT0 → XIAO D9  (MISO)
SD CMD  → XIAO D10 (MOSI)
SD VDD  → XIAO 3V3
SD VSS  → GND
```

## ビルド

```bash
pio run
pio run --target upload
pio device monitor
```

## テスト内容

| Step | 内容 |
|------|------|
| Step1 | SD 初期化・カード種別・容量の表示 |
| Step2 | test.txt 書き込み（2行） |
| Step3 | test.txt 読み返し |
| Step4 | test.txt 追記 |
| Step5 | 100行書き込み速度計測 |

## 正本との関係

Gateway 本番ファーム (`firmware/gateway_v1.0`) の SD 初期化・ログ保存機能の単体テスト。
本番ファームは同じ標準 SD ライブラリ・同じピン配置を使用。

## 関連タスク

`tsuruta_tasks.md` — Gateway 基板 SD 機能検証
