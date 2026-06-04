---
title: Monita Flex v3.03 — Sigfox 計測スケッチ（PlatformIO）
domain: monita_dev
tags:
  - Monita Flex
  - v3.03
  - HX711
  - Sigfox
  - TCA9546A
  - TCA9534
  - XIAO nRF52840
  - Wire
updated: 2026-06-03
---

# Monita_Flex_v3.03_Sigfox_measure

Monita Flex **v3.03** 向けファームの置き場。v3.02 スケッチをベースに、**SIM7080G（LTE-M）関連コードを削除**し Sigfox 専用構成に整理した版です。

## 対象ハード

**Monita Flex v3.03 基板のみ**（SIM7080G 未実装・Sigfox 専用構成）。

**v3.02 基板にも書き込み可能**（ピン割当・I²C 構成は同一）。ただし v3.01 基板には書き込まないこと（D0/D1 の役割が異なる）。

## ハード・ピン（正本）

- **v3.03 設計案・全体**: [Monita_Flex_構成_v3.03.md](../../Monita_Flex_構成_v3.03.md)
- **v3.02 設計案（前版）**: [Monita_Flex_構成_v3.02.md](../../Monita_Flex_構成_v3.02.md)

本スケッチ内の定数は **v3.03 正本に追従**すること。特に **`TCA9534_ADDR`（0x27 はプレースホルダ）** は回路図の A0〜A2 に合わせて変更してください。

## ビルド

```bash
cd v3.03_sigfox
pio run
pio run -t upload
```

## v3.02 スケッチとの主な差分（実装）

| 項目 | v3.02 | v3.03（本フォルダ） |
|------|-------|---------------------|
| SIM7080G（LTE-M） | `MODULE_LTE_M` フラグで切替可 | **削除**。Sigfox 固定 |
| モジュール切替フラグ | `#define MODULE_TYPE SIGFOX / LTE_M` | **廃止**（または `SIGFOX` 固定） |
| D8/D9（UART） | Sigfox / LTE-M 共用 | **Sigfox 専用** |
| その他（I²C・HX711・D0） | v3.02 と同一 | 変更なし |

## 依存

- **HX711** … `lib/HX711` に **bogde/HX711 0.7.5** を同梱
- **Adafruit TinyUSB** … USB CDC（`Serial`）。**nRF52 Arduino コア同梱**
- **TCA9534** … 本プロジェクトでは **外部ライブラリなし**（`Wire` でレジスタ Read/Modify/Write）

## メモ

- **`tca9534Configure()`** … `3V3_SW` 復帰後の **loop 先頭**でも再実行。
- **スリープ中（`__WFI`）**は D0 ポーリングが進まない。GPIOTE 起床や操作無効ポリシーは未実装。
