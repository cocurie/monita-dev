---
title: Monita Flex v3.02 — Sigfox 計測スケッチ（PlatformIO）
domain: monita_dev
tags:
  - Monita Flex
  - v3.02
  - HX711
  - Sigfox
  - TCA9546A
  - TCA9534
  - XIAO nRF52840
  - Wire
updated: 2026-05-11
---

# Monita_Flex_v3.02_Sigfox_measure

Monita Flex **v3.02** 向けファームの置き場。v3.01 スケッチ（[Monita_Flex_v3.01_Sigfox_measure](../Monita_Flex_v3.01_Sigfox_measure/)）をベースに、**4052 の A/B を TCA9534（I²C）で駆動**し、**D0/D1 を MCU 入力**として使う変更を入れた版です。

## 対象ハード

**Monita Flex v3.02 基板のみ**（または同等に TCA9534 が実装され 4052 A/B が MCU D0/D1 から切り離された試作）。

**v3.01 基板にこのビルドを書き込まないでください。** D0/D1 は v3.01 では 4052 用出力のため、挙動が一致せず、I²C に TCA9534 が無い場合は HX711 経路が成立しません。

## ハード・ピン（正本）

- **v3.02 設計案・全体**: [Monita_Flex_構成_v3.02.md](../../Monita_Flex_構成_v3.02.md)
- **v3.01 からの差分・議論**: [Monita_Flex_v3.01_to_v302_基板修正事項.md](../../revisions/Monita_Flex_v3.01_to_v302_基板修正事項.md)
- **継承ブロックの確定値**（D6/D7・D8/D9・電源など）: [Monita_Flex_構成_v3.01.md](../../Monita_Flex_構成_v3.01.md)

本スケッチ内の定数は **v3.02 正本に追従**すること。特に **`TCA9534_ADDR`（0x27 はプレースホルダ）** は回路図の A0〜A2 に合わせて変更してください。

## ビルド

```bash
cd Monita_Flex_v3.02_Sigfox_measure
pio run
pio run -t upload
```

## v3.01 スケッチとの主な差分（実装）

| 項目 | v3.01 | v3.02（本フォルダ） |
|------|-------|---------------------|
| 4052 A/B | `digitalWrite` on D1/D（MUX_A/B） | **TCA9534** 出力レジスタ（`Wire`、アドレス `TCA9534_ADDR`） |
| D0 / D1 | MUX 用出力 | **D0** ユーザボタン（`INPUT_PULLUP`）、**D1** 予備入力 |
| D0 長押し tare / 短押しリセット | なし | **`pollUserButtonStub()`** のみ（DEBUG 時ログ）。本番ポリシーは正本 §6 準拠で今後実装 |

## 依存

- **HX711** … `lib/HX711` に **bogde/HX711 0.7.5** を同梱（レジストリ未接続でもビルド可能にするため。更新する場合は v3.01 側で `pio pkg install` 後に差し替え可）
- **Adafruit TinyUSB** … USB CDC（`Serial`）。**nRF52 Arduino コア同梱**
- **TCA9534** … 本プロジェクトでは **外部ライブラリなし**（`Wire` でレジスタ Read/Modify/Write）

## メモ

- **`tca9534Configure()`** … `3V3_SW` 復帰後の **loop 先頭**でも再実行（レール OFF で TCA が落ちる前提の試作に備える）。常時 `3V3` のみの回路なら冗長だが害はない。
- **スリープ中（`__WFI`）**は D0 ポーリングが進まない。正本どおり **GPIOTE 起床**や操作無効ポリシーは未実装。
