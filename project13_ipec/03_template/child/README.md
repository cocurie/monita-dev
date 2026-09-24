---
title: Monita Flex v3.10 — Sigfox/BLE/LoRa 計測スケッチ（PlatformIO）
domain: monita_dev
tags:
  - Monita Flex
  - v3.10
  - HX711
  - Sigfox
  - BLE
  - LoRa
  - E220-900T22S
  - TCA9546A
  - TCA9534
  - XIAO nRF52840
  - Wire
updated: 2026-07-16
---

# Monita_Flex_v3.10_lora

Monita Flex **v3.10** 向けファームの置き場。`v3.03_sigfox` をベースに、**LoRa（E220-900T22S(JP)-EV2）を第3の通信手段として追加**したものです。Sigfox・BLE・LoRaはロットごとにビルド時選択（同時実装しない）。

## 対象ハード

**Monita Flex v3.10 基板**（v3.03基板に、LoRa用にTCA9534(U6)のP2/P3を新規配線したもの）。

v3.03基板にもそのまま書き込み可能（`COMM_MODE_SIGFOX`/`COMM_MODE_BLE`ビルドはピン割当が同一）。ただし`COMM_MODE_LORA`ビルドはP2/P3が配線されたv3.10基板でのみ動作する。

## ハード・ピン（正本）

- **v3.10 設計案・全体**: [Monita_Flex_構成_v3.10.md](../../Monita_Flex_構成_v3.10.md)
- **v3.03 設計案（ベース版）**: [Monita_Flex_構成_v3.03.md](../../Monita_Flex_構成_v3.03.md)
- **対応するGateway側**: `Gatway基板/gateway_requirements_v1.10.md`「LoRa受信対応」章

本スケッチ内の定数は **v3.10 正本に追従**すること。特に **`TCA9534_ADDR`** は回路図の A0〜A2 に合わせて変更してください。

## ビルド

```bash
cd v3.10_lora
pio run                # platformio.ini の build_flags で選択したモードでビルド
pio run -t upload
```

`platformio.ini` 末尾の `build_flags` で `COMM_MODE_SIGFOX` / `COMM_MODE_BLE` / `COMM_MODE_LORA` のいずれか1つのコメントを外して選択する。

## v3.03 スケッチとの主な差分（実装）

| 項目 | v3.03 | v3.10（本フォルダ） |
|------|-------|---------------------|
| 通信モード | `COMM_MODE_SIGFOX` / `COMM_MODE_BLE` | 上記2つに加え **`COMM_MODE_LORA` を追加** |
| TCA9534 config レジスタ | `0xFC`（P0/P1のみ出力） | LoRaビルド時 `0xF0`（P0〜P3出力。P2/P3=E220 M0/M1） |
| UART(D8/D9) | Sigfox専用 | Sigfox または **LoRa**（ビルド時排他で共用） |
| その他（I²C・HX711・D0） | v3.03 と同一 | 変更なし |

## LoRa（E220-900T22S(JP)）まわりの実装メモ

- **M0/M1制御**: TCA9534(U6) P2=M0, P3=M1（直結GPIOではなくI2C拡張GPIO経由。詳細はコード内コメント参照）
- **AUX**: 未接続。固定ディレイ（`LORA_MODE_SWITCH_DELAY_MS`）で代替
- **設定書き込みロジック**: 起動（起床）毎にREAD(0xC1)で現在値を確認し、想定値と異なればWRITE(0xC0)で書き込む方式（選択肢A。ファーム書き換えだけで全台の設定を変更できる）
- **フレーム形式**: `[0xAA][LEN][MSDペイロード19B][チェックサム]`。MSDペイロードはBLEのMSD形式からCompany ID(2B)を除いた部分と同一レイアウトにし、Gateway側のパース処理を再利用する
- **⚠️ 暫定値・要確認**: `LORA_CFG_REG0`〜`REG3`のビット配置、`LORA_MODE_SWITCH_DELAY_MS`、UART起動待ち時間は E220-900T22S(JP) 公式データシート未確認の暫定値。実機到着後にオシロ/ロジアナで実測・確定すること

## 依存

- **HX711** … `lib/HX711` に **bogde/HX711 0.7.5** を同梱
- **Adafruit TinyUSB** … USB CDC（`Serial`）。**nRF52 Arduino コア同梱**
- **TCA9534** … 本プロジェクトでは **外部ライブラリなし**（`Wire` でレジスタ Read/Modify/Write）
- **E220** … 外部ライブラリなし（`Serial1` で直接コマンド送受信）

## メモ

- **`tca9534Configure()`** … `3V3_SW` 復帰後の **loop 先頭**でも再実行。
- **スリープ中（`__WFI`）**は D0 ポーリングが進まない。GPIOTE 起床や操作無効ポリシーは未実装。
