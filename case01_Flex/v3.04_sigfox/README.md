---
title: Monita Flex v3.04 計測ファーム
domain: iot_device
tags: [monita, flex, v3.04, xiao-nrf52840, hx711, ds3231, sdcard, sigfox, ble]
updated: 2026-07-07
---

# Monita Flex v3.04 — 計測 + SDカード記録 + Sigfox/BLE 送信

XIAO nRF52840 + Monita Flex **v3.04 基板** 用の計測ファーム。
計測内容は v3.03 と同一（HX711×4ch ひずみ／DS3231 基板温度／電池電圧）だが、
基板改版に伴いファームを改修し、**SDカードへの CSV 記録**を追加した。

## v3.03 からのファーム差分（基板改版対応）

| 項目 | v3.03 | v3.04 |
|------|-------|-------|
| 3V3_SW（周辺電源）制御 | D10 直結 GPIO | **TCA9534 P2**（I2C経由） |
| D10 | MOSFET_GATE | **SD カード SPI SCK** |
| SD カード | なし | **J2 スロット追加**（D1=MOSI, D2=MISO, D10=SCK, CS=TCA9534 P3） |
| I2C | Wire (TWIM) | **GPIO ビットバン**（SDA=D4/SCL=D5） |
| 電池センス | A3 (=D3/P0.29) | A3（物理同一ピン） |
| データ記録 | 送信のみ | **毎サイクル SD へ CSV 追記** ＋ 送信 |

### なぜ I2C をビットバンにしたか
BLE モードでは SoftDevice が `Wire.begin()` より前に有効化され、TWIM の IRQ 優先度が
SoftDevice の予約帯と競合してハングし得る。`xiao_ble_sd_flex` で実証済みの
**GPIO ビットバン I2C** に統一し、Sigfox / BLE 両モードで安定動作させる。
アクティブな I2C デバイスは TCA9534(0x20) と DS3231(0x68) のみ。

### SD カード cold boot 初期化
`xiao_ble_sd_flex` で確立した対策を踏襲。`sd.begin()` の単純リトライでは
cold boot で失敗するが、**各リトライ前に `tca9534Init()` を再実行**して
P2(3V3_SW)/P3(CS) を再アサートすると安定する。

## XIAO ピン割当（v3.04）

| ピン | 用途 |
|------|------|
| D0 | タクトスイッチ（tare/reset） |
| D1 | SD MOSI |
| D2 | SD MISO |
| D3 (A3) | 電池電圧センス |
| D4 | I2C SDA（ビットバン） |
| D5 | I2C SCL（ビットバン） |
| D6 | HX711 PD_SCK（4052 MUX 経由） |
| D7 | HX711 DOUT（4052 MUX 経由） |
| D8 | Sigfox UART TX |
| D9 | Sigfox UART RX |
| D10 | SD SCK |

TCA9534(0x20) 出力: P0=4052 B, P1=4052 A, P2=3V3_SW, P3=SD CS

## ビルド / 書き込み

```bash
cd case01_Flex/v3.04_sigfox
pio run                 # ビルド
pio run -t upload       # 書き込み
pio device monitor      # シリアルモニタ（115200）
```

### 通信モード切替（platformio.ini の build_flags）
```ini
build_flags = -D COMM_MODE_SIGFOX   ; Sigfox 送信
; build_flags = -D COMM_MODE_BLE    ; BLE アドバタイズ
```

## デバッグフラグ運用（main.cpp 冒頭）

| フラグ | 計測デバッグ時 | 本番 |
|--------|--------------|------|
| `DEBUG_MODE` | 1 | **0** |
| `DEBUG_NO_SLEEP` | 1 | 0 |
| `DEBUG_NO_SIGFOX` | 必要に応じて1 | 0 |

**本番書き込み前は `DEBUG_MODE=0` にする**（他2つは `#if DEBUG_MODE` の内側）。

## DS3231 時刻設定

1. `DS3231_SET_TIME` を `1`、`DS3231_INIT_*` を現在時刻に設定してビルド・書き込み
2. 起動時にビルド時刻ではなく指定した定数時刻が書き込まれる
3. **書き込み後は `DS3231_SET_TIME` を `0` に戻して再ビルド・書き込み**（毎起動の上書き防止）

DS3231 は CR2032 バックアップで電源断後も時刻を保持する。

## SD カード CSV フォーマット

`/log.csv`:
```
datetime,ch1,ch2,ch3,ch4,temp,batt
2026-07-07 14:00:00,12,0,0,0,253,3812
```
- datetime: DS3231 現在時刻
- ch1..4: HX711 ひずみ値（με）
- temp: DS3231 温度 ×10（253 = 25.3℃）
- batt: 電池電圧 mV

シリアルコマンド（DEBUG_MODE=1）: `d`=ログ全件出力 / `e`=ログ削除

## 制限事項

- `CH_ASSIGN` の `2`(TCA9546A 経由 MPU) / `3`(DS18B20) 経路は v3.04 では未実装。
  使う場合はビットバン/OneWire で再実装し、`platformio.ini` の lib_deps を復活させること。
  現状は全ch HX711（`{1,1,1,1}`）。

## 関連

- SD 初期化の知見: memory `flex-sd-cold-boot-fix`
- v3.04 ネットリスト: memory `flex-v304-netlist`
- 実証元ファーム: `project04_toyono/firmware/xiao_ble_sd_flex`
