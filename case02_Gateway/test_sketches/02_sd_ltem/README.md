---
title: Gateway Step02 — SD + LTE-M（SIM7080G / 1NCE）統合テスト
domain: iot_device
tags: [gateway, nRF52840, SD, SIM7080G, LTE-M, 1NCE, GAS, experimental]
updated: 2026-07-01
---

# Gateway Step02: SD + LTE-M 統合テスト

## 目的

SD カードへの保存と SIM7080G（1NCE SIM）を使った LTE-M 通信を同時に検証する。
ダミーデータを GAS へ HTTP 送信し、その結果を SD へ CSV ログとして保存する。

## ハード

| 項目 | 内容 |
|------|------|
| MCU | Seeed XIAO nRF52840 |
| LTE-M | M5Stamp SIM7080G（5V 供給必須） |
| SIM | 1NCE IoT SIM（APN: iot.1nce.net） |
| SD | SPI 接続（D3=CS、D8=SCK、D9=MISO、D10=MOSI） |

## 配線

```
XIAO D6 (TX) → SIM7080G RX
XIAO D7 (RX) ← SIM7080G TX
XIAO 5V      → SIM7080G 5V   ← 必須（3.3V では起動しない）
XIAO GND     → SIM7080G GND

SD CS        → XIAO D3
SD CLK       → XIAO D8 (SCK)
SD DAT0      → XIAO D9 (MISO)
SD CMD       → XIAO D10 (MOSI)
SD VDD       → XIAO 3V3
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
| Step1 | SD 初期化（失敗時も後続継続） |
| Step2 | SIM7080G AT 疎通・SIM 認識確認 |
| Step3 | LTE-M ネットワーク登録（1NCE、最大120秒） |
| Step4 | GAS へダミーデータ（1台分）を HTTP GET 送信 |
| Step5 | SD の test_log.csv を読み返して確認 |

## SD ログ形式

ファイル名: `test_log.csv`

```
step,result,detail
step1,OK,SD init
step2,OK,modem+SIM ready
step3,OK,network ready
step4,OK,status=302
```

## GAS への送信パラメータ（ダミー）

```
ts=TEST&sim=1NCE&n=1&m0=AA-BB-CC-DD-EE-FF&p0=DEADBEEF&r0=-70
```

GAS 送信成功時はスプレッドシートに TEST 行が追加される（ステータスコード 302 = 成功）。

## 注意事項

- SIM7080G への 5V 供給が必須。XIAO の USB-C 給電（VBUS ピン）から取ること。
- 起動後 Step2 の AT 疎通まで約 15〜20 秒かかる。
- Step3 のネットワーク登録は電波状況により最大 2〜3 分かかる場合がある。

## 正本との関係

Gateway 本番ファーム (`firmware/gateway_v1.0`) の SD + LTE-M 送信機能の単体テスト。
GAS スクリプト ID・SIM 設定・ピン配置はすべて本番ファームと共通。

## 関連タスク

`tsuruta_tasks.md` — Gateway 基板 SD + LTE-M 統合検証
