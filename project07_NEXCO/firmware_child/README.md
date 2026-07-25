---
title: project07_NEXCO — 剥落検知センサ ファームウェア
domain: monita_dev
tags:
  - NEXCO
  - 剥落検知
  - HX711
  - BLE
  - XIAO nRF52840
  - Monita Flex v3.03
updated: 2026-07-25
---

# project07_NEXCO firmware

中国自動車道有野川付近RC床版 コンクリート剥落モニタリング向けファームウェア。

**ベース**: `case01_Flex/v3.03_sigfox`（BLE モードで運用）

---

## 対象ハード

- **基板**: Monita Flex **v3.03**
- **MCU**: XIAO nRF52840
- **センサ**: HX711（24bit ADC）× 4ch — ひずみゲージ（片持ち梁・SUS304 t=1mm）
- **通信**: BLE アドバタイズ → Gateway 経由でクラウド送信

---

## センサ構成（剥落検知）

| ch | 役割 |
|----|------|
| ch1 | 剥落エリア 左 |
| ch2 | 剥落エリア 中央 |
| ch3 | 剥落エリア 右 |
| ch4 | **参照点**（剥落なしエリア・温度補償基準） |

ひずみゲージ: 2ゲージ法（ハーフブリッジ）/ 片持ち梁表裏各1枚  
設計値: 0.1mm 変位 → 約 50 μS / HX711 出力 約 5 μV

---

## ビルド

```bash
cd project07_NEXCO/firmware
pio run
pio run -t upload
```

通信モードは `platformio.ini` の `build_flags` で選択（現在: `COMM_MODE_BLE`）。

---

## バージョン管理

`src/main.cpp` を変更して commit するたびに `FW_VERSION` を +1 すること（CLAUDE.md §6 ルール準拠）。

| 定数 | 場所 |
|------|------|
| `FW_VERSION` | `src/main.cpp`（COMM_MODE_BLE ブロック内） |

---

## 関連ドキュメント

- 設計メモ: `GoogleDrive/【7】Monita/開発/案件/project07_NEXCO/剥落検知センサ設計メモ.md`
- 基板正本: `GoogleDrive/【7】Monita/開発/Flex基板/Monita_Flex_構成_v3.03.md`
