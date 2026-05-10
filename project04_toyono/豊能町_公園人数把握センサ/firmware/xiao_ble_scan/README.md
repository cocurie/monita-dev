# xiao_ble_scan — BLE アドバタイジング シリアルログ

公園人流測定向けの第一歩として、XIAO nRF52840 Sense が **Central で Active Scan** し、周辺の ADV / Scan Response をシリアルへ出します。

## 前提

- [要件定義（親フォルダ）](../ble_people_count_requirements.md)
- PlatformIO + Seeed `platform-seeedboards`（`platformio.ini` 参照）
- ボード: **Seeed XIAO nRF52840 Sense**（USB 記述子が Sense の場合）

## ビルド・書き込み・モニタ

```bash
cd "/Users/shuichi/Library/CloudStorage/GoogleDrive-cocurie.kanri@gmail.com/マイドライブ/【7】Monita/開発/BLE公園人流測定/firmware/xiao_ble_scan"
pio run
pio device list    # /dev/cu.usbmodem... を確認
# platformio.ini の upload_port を必要なら有効化してから:
pio run -t upload
pio device monitor -b 115200
```

Mac で **Bluetooth-Incoming-Port** に誤爆する場合は、`platformio.ini` の **`upload_port`** を実機の `usbmodem` に指定すること。

## ログ形式

`ms MAC_RSSI [ADV|SR] payload(hex)`

詳細名・Manufacturer Data を増やす場合は `src/main.cpp` 先頭の **`DEBUG_VERBOSE` を `1`** に変更して再ビルド。

## 注意

- **`while (!Serial)`** は USB 接続待ち。バッテリー単体では進まないため、将来はタイムアウト付きに変更する想定。
- ログには **MAC や AD 生データ**が含まれる。本番の匿名化・送信は要件書 §3.3 に従い別実装する。
- 公園等での実験は **プライバシー・同意**を要件・自治体ルールと整合させること。
