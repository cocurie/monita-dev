# Monita One v1.1 firmware

Seeed XIAO nRF52840 Senseを搭載したMonita One **v1.1基板**用PlatformIOプロジェクトです。標準1CHセンサ版とPIR連動人数推定版を、LoRa / Sigfoxのビルド時排他で提供します。`setup()` / `loop()`は`build_src_filter`で別ファイルを選び、実行時`#ifdef`でアプリを分岐していません。

v1.00基板用は `case03_One/v1.00`（FW 0x04）です。**v1.1ファームはv1.00基板に書き込まないでください**（D2のAUXが未配線のため、LoRa版で毎回AUX待ちのタイムアウトが発生し起床時間が延びます）。

## v1.00 からの変更点

基板の変更点の正本: `【7】Monita/01_開発/MonitaOne基板/Monita_One_v1.00_to_v1.1_diff.md`

| # | 変更 | 根拠 |
|---|---|---|
| 1 | `VBAT_ENABLE`(P0.14)を常時LOW。読取後にHIGHへ戻さない。`halBegin()`を`setup()`の先頭で呼ぶ | v1.01 MD §D-6。ポゴピンで電池をつなぐ前に必須。**★コアの`initVariant()`が`setup()`前にHIGHにするため、起動直後の短時間はHIGHになる（未解決・v1.1 MD §4-1 F1）** |
| 2 | 起動時、出力ラッチを書いてから`pinMode(OUTPUT)`（`MOSFET_GATE` / `LORA_MODE`） | v1.01 MD §D-5。起動直後の3V3_SW ONグリッチ防止 |
| 3 | `setPeripheralPower(false)`で、3V3_SWを切る前にPD_SCK / DOUT / I2C / UART / AUXを切断（Hi-Z） | v1.01 MD §D-4。保護ダイオード経由の逆給電防止 |
| 4 | E220 `AUX`(D2)でモード切替・設定書込み・送信の完了を待つ。v1.00の固定待ちは下限として残す | v1.1基板で配線（§3 #10） |
| 5 | LoRaフレームと設定コマンドを1回のブロック書込みで送る | 開発メモ 20260807（E220送信の落とし穴②） |
| 6 | 充電モジュールの`CHRG`(D0) / `DONE`(D1)を読み、ログと**ペイロードのCH4**へ載せる（下記「ペイロード」） | v1.1基板で配線（§5 #4）。載せ方は2026-09-16決定 |

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

- `lib/STM32duino VL53L4CD`: 上流1.0.5に改変あり。`src/platform.cpp` の `VL53L4CD_I2CRead()` の
  無制限再試行を3回までに制限（センサ未接続で戻らなくなるため）。上流へ差し替えたら当て直すこと
- `src/one_hal.*`: ピン、反転MOSFET電源と逆給電対策、専用I2C、VBAT/CPU温度、充電状態、E220（AUX待ち）、Sigfox UART、WDT、InternalFS/CRC
- `src/one_status.h`: 充電状態の判定（Arduino非依存の純粋関数）
- `src/one_payload.*`: LoRa 19B / Sigfox 12B生成と、丸め・飽和・電池圧縮の純粋関数
- `src/app_sensor.cpp`: Flex v3.20由来の1CHセンサ計測サイクル
- `src/app_pir.cpp`: PORT/SENSE起床、BLEスキャン、集計、定時送信を行う単一タスク状態機械
- `test/host_payload_test.cpp`: ペイロード仕様GV-1〜GV-8と充電状態判定のホスト試験（GAS側の同ベクタは`case02_Gateway/tests/monita_one_payload_vectors.test.js`）

ホスト試験:

```sh
c++ -std=c++11 -Wall -Wextra -pedantic \
  test/host_payload_test.cpp src/one_payload.cpp \
  -o /tmp/monita_one_payload_test
/tmp/monita_one_payload_test
```

## ハードウェア上の注意

ピンは`one_hal.h`先頭の1ブロックに集約しています。

| ピン | 信号 | 状態 |
|---|---|---|
| D0 | `ST_CHRG`（充電モジュール、LOW=充電中） | v1.1追加・実機未検証 |
| D1 | `ST_DONE`（充電モジュール、LOW=満充電） | v1.1追加・実機未検証 |
| D2 | `LORA_AUX`（E220、LOW=処理中） | v1.1追加・実機未検証。Sigfox実装機では未接続 |
| D3 | `PD_SCK`（HX711 / DS18B20兼用） | v1.00で実機確認済み |
| D4 | `DOUT`（HX711 / PIR OUT兼用） | v1.00で実機確認済み |
| D5 / D6 | `I2C_SDA` / `I2C_SCL`（専用`SensorWire`） | v1.00から変更なし |
| D7 | `LORA_MODE`（E220 M0/M1共通） | v1.00から変更なし |
| D8 / D9 | `UART_TX` / `UART_RX` | v1.00から変更なし |
| D10 | `MOSFET_GATE`（LOW=ON） | v1.00で実機確認済み |

`setPeripheralPower()`だけがMOSFETゲートと逆給電対策を扱います。電源断では、LORA_MODEをLOWへ戻し、I2CとUARTを閉じ、OFFになる回路へつながる信号をすべて切断してからゲートをHIGHにします。PIR版だけはPIR給電のため3V3_SWを常時ONとし、LoRa待機中はE220をMode 3へ置きます。

VBATはAdafruit variantの`VBAT_ENABLE`（LOW=測定有効）と`PIN_VBAT=P0.31`を使用します。ADC既定レンジ3.6Vと、XIAO内蔵の分圧（上1MΩ／下510kΩ、比1510/510）を整数演算で復元します。**2026-09-16以前のビルドは比を2020/510で計算しており、電池電圧を約1.34倍高く読んでいました。**

### 充電状態（CHRG / DONE）

充電モジュール側でショットキーダイオードにより分離されており、One側にプルアップはありません。`readChargeState()`は**読む瞬間だけ内部プルアップを有効**にし、読み終えたら切断します。日照がなくCN3063のVINが0Vの間に、プルアップからCHRG/DONE端子へ電流が流れ続けるのを避けるためです（回り込みの有無は要実測）。

| CHRG | DONE | ログ表示 | 意味 |
|---|---|---|---|
| H | H | `idle` | 日照なし、またはモジュール未接続 |
| L | H | `charging` | 充電中 |
| H | L | `done` | 満充電 |
| L | L | `invalid` | 通常は起きない。夜間の回り込み等を疑う |

### E220 AUX

AUXは通電中だけプル無し入力にします。`waitLoRaIdle()`はAUXのHIGHを2ms間隔で2回確認して抜けます。上限（モード切替1秒、送信3秒）に達するとログを出して続行するため、AUXが来なくてもv1.00と同じ固定待ちで動きます。Sigfoxビルドでは AUX を一切読みません。

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

送信できなかった期間の集計は捨てずに次の期間へ合算します（FW 6〜。LoRaは設定確認の失敗、Sigfoxは送信失敗を「送れなかった」とみなす。AUXの完了待ちは実機未検証のため判定に使わない）。LoRa版は、設定確認に2回続けて失敗すると3V3_SWを1秒切ってE220を電源から再起動します。同じレールのPIRも落ちるため、入れ直し後は`pirHoldoffSec`のあいだPIR通知を抑止します（FW 6〜）。

BLEはpassive、interval/window=150/100ms、初期スキャン30秒、RSSI=-65、MIN_HITS=10、merge gap=3、最大64台です。MACはスキャンRAMだけに保持し、停止直後に全領域をゼロクリアします。flash、Serial、ペイロードには出力しません。

## ペイロード

**v1.1でCH4に充電状態を追加しました（2026-09-16）。** Gatewayは改修不要（CH1〜4はそのままGASへ届く）で、GASは`case02_Gateway/gas/one`のv11が復号します。

| 版 | CH4 |
|---|---|
| 標準センサ版 | 充電状態 `0`=日照なし/未接続、`1`=充電中、`2`=満充電、`3`=異常（v1.00基板用FW 0x04以前は`-1`） |
| PIR版 | スキャン回数（bit0〜12、**8191で飽和**）＋充電状態（bit13〜14） |

PIR版のスキャン回数は1報告あたり最大1440回（報告間隔1440分×毎分1回）なので13bitで足ります。値は最大32767でint16の正の範囲に収まり、欠測値`-1`と衝突しません。Sigfox 12Bも同じCH4位置です。

LoRaはPkt type `0x04`の19B固定です。PIR版はCH1=最大人数、CH2=平均人数×10、CH3=PIR確定イベント、CH4=スキャン回数＋充電状態です。スキャン0回はCH1/CH2を`-1`とし、0人と未計測を区別します。全集計値はint16へ飽和させ、中間演算は64bitでoverflowを防ぎます。

Sigfox 12B契約は正本でも未確定です。本実装では既存Flex互換の`CH1..4 + temperature×10 + battery mV`を暫定採用し、PIRのCH意味・欠測・飽和はLoRaと揃えています。backend確定時に双方を同時更新してください。

`FW_VERSION`は`platformio.ini`で定義します。ファーム更新コミットではインクリメントしてください。版数は10進で書きます（4まではv1.00基板用、5以降がv1.1基板用）。

### DEVICE_ID（子機ID）の設定

1台のGatewayに複数のFlex / Oneをぶら下げるため、**機体ごとに固有のIDを割り当てます**。

DeviceIDは1バイトを2つのフィールドに分割しています。

```
DEVICE_ID (1バイト)
  上位3bit = Gateway群 (0〜7)       → group   = DEVICE_ID >> 5
  下位5bit = 群内の機器番号 (1〜31)  → localNo = DEVICE_ID & 0x1F   ※0は無効値
```

- 群の境界は 群0 = `0x01`〜`0x1F`、群1 = `0x21`〜`0x3F`、群N の開始は `N × 0x20 + 1`
- 1つの現場に複数のGatewayを置く場合、Gatewayは自分の `GATEWAY_GROUP_ID` と上位3bitが一致する
  パケットだけをLoRaで受信します（二重受信の防止）
- 下位5bitが0のID（`0x00` `0x20` `0x40` …）は無効値です。`static_assert` でビルドが失敗します
- 既定値 `0x0F`（群0・機器15）は `src/app_sensor.cpp` / `src/app_pir.cpp` の `#ifndef DEVICE_ID` で定義
- ビルド時に上書きできます

```sh
PLATFORMIO_BUILD_FLAGS="-D DEVICE_ID=0x24" pio run -e one_sensor_lora -t upload
```

（`0x24` = 群1・機器番号4）

`platformio.ini`側では定義していません。両方で定義すると`redefined`警告が出るためです。
書き込んだIDは起動ログ（`[BOOT] ... DEVICE_ID=0x24 group=1 localNo=4`）で確認できますが、
**機体に貼るなどして管理してください**（ID重複はGateway側でデータが混ざる原因になります）。

## 実機検証の状況

v1.00基板で確認済み（2026-08-26。詳細は `【7】Monita/01_開発/開発メモ/20260826_MonitaOne_v1.00_実機立ち上げ記録.md`）:

- ピン割当（MOSFET_GATE=D10 / PD_SCK=D3 / DOUT=D4）
- MOSFET極性が LOW=ON であること
- HX711（CH_ASSIGN=1）での計測（ok=1 / range=1 / errors=0x0）
- E220の設定読み書きとLoRa送信

**v1.1ファームは4環境のビルドとホスト試験のみ確認。実機は未検証**（v1.1基板が未製造）。v1.1基板で確認すること:

- 起動直後に3V3_SWがONにならないこと（オシロで MOSFET_GATE / 3V3_SW）
- 3V3_SW OFF中の3V3_SW電位とPD_SCK / I2C電位（逆給電がないこと）とスリープ電流
- AUX待ちでタイムアウトが出ないこと、送信・ダウンリンク受信が v1.00 と同等に成功すること
- CHRG / DONE の表示が充電LEDと一致すること。夜間（VIN=0V）に `invalid` / `charging` と誤表示しないか
- ポゴピン経由のVBAT ADC実測校正
- PIR版でE220のUARTを外して設定確認を2回失敗させ、3V3_SWの入れ直し後に送信が復帰し、PIRの誤検知が抑止されること（FW 6）

v1.00から引き続き未確認:

- Gateway側での受信（One用スプレッドシートとGASのID差し替えが未了）
- E220 Mode 3電流、R7=100kΩの電流
- STRAIN_SCALE の校正（現在100。Flex系は1110）
- PIRモード: PPI/EGU3による起動時HIGH・約2秒HIGH・張り付き・チャタリング
- PIR→SCAN→TX/RX→idleを1000回以上反復した電力・欠測・WDT評価
- Sigfoxモジュールの待機モードと最終12B payload契約
