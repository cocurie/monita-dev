---
title: YCE向け Monita Flex Sigfox ファームウェア（ひずみ＋レンジペイロード）
domain: iot_device
tags: [YCE, 関門橋, Sigfox, HX711, Flex, v3.10, strain, range, experimental]
updated: 2026-09-02
---

# YCE向け Monita Flex Sigfox ファームウェア

## 目的

YCE（ワイ・シー・イー）関門橋案件向けに、ひずみ4CH＋CH1・CH3の変動レンジをSigfoxで送信するファームウェア。  
ベースはMonita Flex v3.02。対象基板: **Monita Flex v3.10**。

## ペイロード構成（12バイト）

| バイト | 内容 | 単位 |
|--------|------|------|
| 0-1 | CH1ひずみ | με |
| 2-3 | CH2ひずみ | με |
| 4-5 | CH3ひずみ | με |
| 6-7 | CH4ひずみ | με |
| 8-9 | CH1レンジ（max-min） | με |
| 10-11 | CH3レンジ（max-min） | με |

## 測定方式

- **1回の読み取り**: HX711から5サンプル取得しメジアン（`DATA_NUM=5`）
- **1サイクルの繰り返し**: 5回繰り返し（`REPEAT_NUM=5`）
- **ひずみ値**: 5回読み取りのメジアン ÷ STRAIN_SCALE（1110）
- **レンジ値**: 5回読み取りの（max-min） ÷ STRAIN_SCALE（1110）
- **送信間隔**: 120分（2時間）

## 主な変更点（v3.02比）

| 項目 | v3.02 | 本ファーム |
|------|-------|-----------|
| ペイロード | CH1-4 + 温度 + 電池電圧 | CH1-4 + CH1レンジ + CH3レンジ |
| SLEEP_MINUTES | 1（デバッグ用） | 120（2時間） |
| REPEAT_NUM | 3（未使用） | 5（レンジ計算に使用） |
| STRAIN_SCALE換算 | なし | あり（÷1110） |
| FW_VERSION | なし | 1 |

## ハード

- ボード: XIAO nRF52840（Monita Flex v3.10基板）
- HX711: 4CH（SN74LV4052 MUX経由、TCA9534でA/B制御）
- ひずみゲージ: 120Ω 3線式 × 4CH

## ビルド

```bash
pio run
pio run --target upload
```

HX711ライブラリは `lib/HX711/` に格納（v3.02からコピー）。

## 注意

- WDTは本ファームに未実装。SLEEP_MINUTES(120分)スリープ中は問題ないが、  
  計測処理がハングした場合の自動復旧は期待できない。本番前に要検討。
- STRAIN_SCALE=1110 はYCE関門橋案件のキャリブレーション係数。変更時は確認要。
- CH2・CH4のレンジは計算されるが送信しない（必要になればペイロード変更を検討）。

## 関連

- ベースファーム: `01_開発/Flex基板/スケッチ/Monita_Flex_v3.02_Sigfox_measure/`
- 案件フォルダ: `02_案件/project01_YCE/`
