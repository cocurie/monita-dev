---
title: Monita Flex v3.02 検証スケッチ一覧
domain: monita_dev
tags: [Flex, v3.02, test, HX711, TCA9534, TCA9546A, MPU6050, XIAO nRF52840]
updated: 2026-06-03
---

# Monita Flex v3.02 — 検証スケッチ

v3.02基板の動作確認用。**必ずステップ順に実施すること。**

| Step | フォルダ | 確認内容 | 前提 |
|------|----------|----------|------|
| 1 | `01_power_led/` | 電源ON、RGB LED 3色点灯、3V3_SW ON/OFF | XIAOのみ |
| 2 | `02_i2c_scan/` | I2Cスキャン（TCA9534: 0x20、TCA9546A: 0x70） | 基板実装済み |
| 3 | `03_tca9534/` | TCA9534でMUX A/B切り替え | Step2 OK |
| 4 | `04_hx711_1ch/` | HX711 CH1固定で読み取り | Step3 OK、JP1に接続 |
| 5 | `05_hx711_4ch/` | HX711 4ch MUX切り替え読み取り | Step4 OK |
| 6 | `06_tca9546a/` | TCA9546A CH0〜CH3 切り替え＋I2Cスキャン | Step2 OK |
| 7 | `07_mpu6050_4ch/` | TCA9546A経由 MPU6050 4ch読み取り | Step6 OK、各CHにMPU6050接続 |
| 13 | `13_strain_cal/` | HX711 4ch ひずみ補正係数確認（STRAIN_SCALE調整用） | Step5 OK、ひずみ発生装置接続 |

## アップロード方法

```bash
# 例: Step1をアップロード
~/.platformio/penv/bin/pio run -t upload \
  --project-dir ~/Documents/Monita_dev/case01_Flex/test_sketches/01_power_led
```

VSCodeなら各フォルダを開いてPlatformIO → Upload。

## 各Stepの合格基準

### Step1
- LEDが RED → GREEN → BLUE → OFF の順に光る
- シリアルに `[3V3_SW] HIGH` / `LOW` が交互に出る

### Step2
- `0x20: TCA9534` と `0x70: TCA9546A` が両方表示される

### Step3
- CH1〜CH4の切り替えログが出て `OK` が並ぶ
- テスターがあれば4052のA/Bピン電圧がログと一致する

### Step4
- `[HX711 CH1] raw: XXXXXX` が1秒ごとに出る
- ゲージに荷重をかけると値が変化する

### Step5
- CH1〜CH4それぞれの raw値が出る
- 接続していないスロットは `TIMEOUT` でOK

### Step6
- CH0〜CH3 それぞれのスキャン結果が表示される
- MPU6050を接続したCHは `0x68: MPU6050/MPU9250` が表示される
- 未接続のCHは `検出: 0個` でOK

### Step7
- CH0〜CH3 それぞれの加速度・ジャイロ・温度が3秒ごとに出る
- 接続していないCHは `応答なし` でOK
- 基板を傾けると az/ax/ay の値が変化することを確認

## Step5 完了後（HX711系）

`case01_Flex/v3.02_sigfox/src/main.cpp` の本番ファームに移行。

## Step7 完了後（MPU6050系）

本番ファームの `CH_ASSIGN[]` を `2`（MPU6050）に設定して統合テストへ。
