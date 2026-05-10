---
title: Monita Flex v3.01 — Sigfox 計測スケッチ（PlatformIO）
domain: monita_dev
tags:
  - Monita Flex
  - v3.01
  - HX711
  - Sigfox
  - TCA9546A
  - XIAO nRF52840
  - Wire
updated: 2026-05-08
---

# Monita_Flex_v3.01_Sigfox_measure

Monita Flex **v3.01** 向けファームの置き場（**2026-05-08** 更新: **Sigfox `sendAT`**・**USB Serial に Adafruit TinyUSB**）。

## ハード・ピン対応

正本: [Monita_Flex_構成_v3.01.md](../../Monita_Flex_構成_v3.01.md)（D0/D1=4052 A/B、D2=温度、D3=電池、D4/D5=I2C、D6/D7=HX711、D8/D9=Sigfox UART、D10=`MOSFET_GATE`）

## ビルド

```bash
cd Monita_Flex_v3.01_Sigfox_measure
pio run
pio run -t upload
```

## 依存

- **HX711** … `bogde/HX711`
- **Adafruit TinyUSB** … USB CDC（`Serial`）用。**ヘッダは nRF52 Arduino コア同梱**（`lib_deps` では入れていない）
- **InternalFileSystem** … Adafruit nRF52 コア同梱（インクルードのみ・未使用でも可）

## メモ

- `deepSleep` … **`3V3_SW` OFF 後、内蔵 RTC2（LFCLK）で CC[0] 比較 → `__WFI()` 待機**（RTC1 は FreeRTOS 用のため未使用）。最大約 **24 日**相当まで1回で指定可（カウンタ 24bit・8 tick/s 名義）。**System OFF** は別途。
- `CH_ASSIGN` … `1`=HX711、`2`=I2C 上 MPU（TCA9546A で CH 選択）。
- **Sigfox**: `AT$SF=` 後に **`sendAT` で応答を最大 10 s 待機**し、`OK` で成功判定。起動後 **3 s** 待ちを setup/loop の両方に入れている。
