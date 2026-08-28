# Monita One v1.00 firmware

Seeed XIAO nRF52840 Senseを搭載したMonita One v1.00用PlatformIOプロジェクトです。標準1CHセンサ版とPIR連動人数推定版を、LoRa / Sigfoxのビルド時排他で提供します。`setup()` / `loop()`は`build_src_filter`で別ファイルを選び、実行時`#ifdef`でアプリを分岐していません。

## ビルド

```sh
pio run
```

個別にビルドする場合:

```sh
pio run -e one_sensor_lora
pio run -e one_sensor_sigfox
pio run -e one_pir_lora
pio run -e one_pir_sigfox
```

依存ライブラリは`lib/`へ同梱しているため、取得済みのSeeed platform / toolchainがあればネットワークなしでビルドできます。

## 構成

- `src/one_hal.*`: ピン、反転MOSFET電源、専用I2C、VBAT/CPU温度、E220、Sigfox UART、WDT、InternalFS/CRC
- `src/one_payload.*`: LoRa 19B / Sigfox 12B生成と、丸め・飽和・電池圧縮の純粋関数
- `src/app_sensor.cpp`: Flex v3.20由来の1CHセンサ計測サイクル
- `src/app_pir.cpp`: PORT/SENSE起床、BLEスキャン、集計、定時送信を行う単一タスク状態機械
- `test/host_payload_test.cpp`: ペイロード仕様GV-1〜GV-6のホスト試験

ホスト試験:

```sh
c++ -std=c++11 -Wall -Wextra -pedantic \
  test/host_payload_test.cpp src/one_payload.cpp \
  -o /tmp/monita_one_payload_test
/tmp/monita_one_payload_test
```

## ハードウェア上の注意

ピンは`one_hal.h`先頭の1ブロックに集約しています。全ピンはネットリスト解析と実機検証（2026-08-26）により確定済みです。I2CはOne配線のD5/D6を使う専用`TwoWire SensorWire`であり、XIAO既定`Wire`（D4/D5）は使いません。

`setPeripheralPower()`だけがMOSFETゲートを操作します。OneではLOW=ON / HIGH=OFFです。電源断前にLORA_MODEをLOWへ戻し、UARTを`Serial1.end()`で閉じます。PIR版だけはPIR給電のため3V3_SWを常時ONとし、LoRa待機中はE220をMode 3へ置きます。

VBATはAdafruit variantの`VBAT_ENABLE`（LOW=測定有効）と`PIN_VBAT=P0.31`を使用します。ADC既定レンジ3.6Vと、1510kΩ/510kΩの分圧比を整数演算で復元します。

## 設定とダウンリンク

標準版は`/one_sensor_v1.bin`、PIR版は`/one_pir_settings_v1.bin`へ保存します。どちらもversion、値域検証、CRC32が一致した場合だけ採用します。PIR版は設定変更resetの直前に`/one_pir_runtime_v1.bin`へ集計、次回期限、holdoff、rolling windowを保存して一度だけ復元します。

標準LoRa版はFlex v3.20の15Bダウンリンクを継承します。時刻設定要求はDS3231非搭載のためACK status `2`（time unsupported）で拒否します。bit 3は再タレ要求として予約しています。

PIR LoRa版の15B設定payloadは次の割当です。flagsのbit 0〜7が各フィールドの適用有無に対応します。

```
C0 DE 81 DEVICE flags reportMin(2,BE) rssi minHits mergeGap calibration
holdoffSec(2,BE) maxScansPerHour scanDurationSec
```

## PIR実装

PIRはGPIOTE IN eventを割り当てません。GPIO `SENSE=HIGH`のPORTイベントをPPI channel 15でEGU3へ転送し、ISRではPORT/LATCHのclearとタスク通知だけを行います。タスク側でSENSEをdisarmし、PIRがLOWへ戻ってからrearmします。HIGHが10秒続いた場合は張り付きとしてquarantineし、LOW復帰時だけ解除します。

BLEはpassive、interval/window=150/100ms、初期スキャン30秒、RSSI=-65、MIN_HITS=10、merge gap=3、最大64台です。MACはスキャンRAMだけに保持し、停止直後に全領域をゼロクリアします。flash、Serial、ペイロードには出力しません。

## ペイロード

LoRaはPkt type `0x04`の19B固定です。PIR版はCH1=最大人数、CH2=平均人数×10、CH3=PIR確定イベント、CH4=スキャン回数です。スキャン0回はCH1/CH2を`-1`とし、0人と未計測を区別します。全集計値はint16へ飽和させ、中間演算は64bitでoverflowを防ぎます。

Sigfox 12B契約は正本でも未確定です。本実装では既存Flex互換の`CH1..4 + temperature×10 + battery mV`を暫定採用し、PIRのCH意味・欠測・飽和はLoRaと揃えています。backend確定時に双方を同時更新してください。

`FW_VERSION`は`platformio.ini`で定義します。ファーム更新コミットではインクリメントしてください。

### DEVICE_ID（子機ID）の設定

1台のGatewayに複数のFlex / Oneをぶら下げるため、**機体ごとに固有のIDを割り当てます**。

- 許可範囲は **0x01〜0x0F**（Gateway側 `ALLOWED_DEVICE_IDS[]` / `MAX_PENDING_CHILDREN=15` と対応）
- 既定値 `0x0F` は `src/app_sensor.cpp` / `src/app_pir.cpp` の `#ifndef DEVICE_ID` で定義
- ビルド時に上書きできます

```sh
PLATFORMIO_BUILD_FLAGS="-D DEVICE_ID=0x0E" pio run -e one_sensor_lora -t upload
```

`platformio.ini`側では定義していません。両方で定義すると`redefined`警告が出るためです。
書き込んだIDは起動ログでは出ないため、**機体に貼るなどして管理してください**（ID重複はGateway側で
データが混ざる原因になります）。

## 実機検証の状況

確認済み（2026-08-26。詳細は `【7】Monita/開発/開発メモ/20260826_MonitaOne_v1.00_実機立ち上げ記録.md`）:

- ピン割当（MOSFET_GATE=D10 / PD_SCK=D3 / DOUT=D4）
- MOSFET極性が LOW=ON であること
- D1をショートした状態で 3V3_SW = 3.3V を確保できること
- HX711（CH_ASSIGN=1）での計測（ok=1 / range=1 / errors=0x0）
- E220の設定読み書きとLoRa送信

未確認:

- Gateway側での受信（One用スプレッドシートとGASのID差し替えが未了）
- E220 Mode 3電流、R7=100kΩの電流（R7の差し替え自体が未実施）
- VBAT ADCの実測校正（バッテリー未接続のため batt_mV は未検証）
- STRAIN_SCALE の校正（現在100。Flex系は1110）
- PIRモード: PPI/EGU3による起動時HIGH・約2秒HIGH・張り付き・チャタリング
- PIR→SCAN→TX/RX→idleを1000回以上反復した電力・欠測・WDT評価
- Sigfoxモジュールの待機モードと最終12B payload契約
