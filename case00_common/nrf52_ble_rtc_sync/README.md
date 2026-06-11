---
title: nRF52840 + DS3231 BLE 時刻同期 検証スケッチ
domain: iot_device
tags: [nRF52840, DS3231, BLE, RTC, time-sync, GATT, XIAO]
updated: 2026-06-03
---

# nRF52840 + DS3231 BLE 時刻同期 検証

横河BHD など「停電時も正確なタイムスタンプが必要」な用途に向けた、
親機→子機 BLE 時刻配信の検証スケッチ群。

## ハード構成

```
親機: XIAO nRF52840 + DS3231（SDA=D4, SCL=D5）
子機: XIAO nRF52840 + DS3231（SDA=D4, SCL=D5）
```

DS3231 の CR2032 コイン電池が電源断時の時刻保持に必要。

## 検証ステップ一覧

| Step | フォルダ | 内容 | 使用スケッチ |
|------|---------|------|------------|
| 1 | `01_rtc_basic/` | DS3231 単体: 読み書き動作確認 | 両機共通 1スケッチ |
| 2 | `01_rtc_basic/` | DS3231 停電耐性: 電源断→復電で時刻保持確認 | Step1と同じ |
| 3 | `02_ble_adv_scan/` | BLE アドバタイズ/スキャン確認 | parent + child |
| 4 | `03_ble_gatt_connect/` | BLE GATT 接続＋文字列送受信 | parent + child |
| 5 | `04_ble_time_sync/` | BLE で時刻転送→子機DS3231に書込み | parent + child |
| 6 | `04_ble_time_sync/` | 停電→復電後もタイムスタンプが正確か | Step5と同じ |
| 7 | `05_ble_auto_sync/` | サイクル起床→自動同期→模擬計測→待機 | 04_parent + child |

## 合格基準

### Step1
- シリアルに時刻が毎秒進む
- `S` 送信でコンパイル時刻にセットされる

### Step2
- 電源断→復電後も時刻が継続している（ずれ ≦ 数秒）

### Step3
- 子機シリアルに `"TimeParent"` と RSSI が表示される

### Step4
- 子機シリアルに `"Hello from Parent"` が表示される

### Step5
- 同期後、親機・子機の時刻が ±1秒以内で一致する

### Step6
- 子機 USB 断→数分後復電で、起動直後から正しい時刻が表示される

### Step7
- 毎サイクル、同期ログ＋タイムスタンプ付き計測ログが出力される
- 親機が不在でもスキャンタイムアウト後に計測が継続する

## ビルド方法

```bash
# 例: Step1
~/.platformio/penv/bin/pio run -t upload \
  --project-dir ~/Documents/Monita_dev/case00_common/nrf52_ble_rtc_sync/01_rtc_basic
```

## Step7 の注意

`05_ble_auto_sync/child` は `delay()` でサイクル待機を表現している（ブレッドボード検証用）。  
Flex v3.02 本番統合時は `deepSleep(RTC2)` に置き換える。  
親機スケッチは `04_ble_time_sync/parent` をそのまま流用する。
