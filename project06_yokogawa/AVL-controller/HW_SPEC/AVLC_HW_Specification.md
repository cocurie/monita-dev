# 汎用 BLE / Wi-Fi コントローラー ハードウェア仕様書

**ボード名**: main_bord Ver1 Rev1  
**作成日**: 2026-05-22  
**主要 MCU**: ESP32-WROVER  
**参照回路図**: main_bord_Ver1_Rev1.sch

---

## 1. 概要

本ボードは ESP32-WROVER を搭載した汎用 BLE / Wi-Fi コントローラーです。  
320×240 カラータッチ液晶・SD カードストレージ・USB-C 充電・LiPo バッテリー管理を一体化しており、独自ファームウェアを書き込んで各種 BLE・Wi-Fi アプリケーションに転用できます。

### ブロック図

```
┌─────────────────────────────────────────────────────────────┐
│                         電源システム                         │
│  USB-C ──► BQ24075 ──► LiPo 充電                           │
│  LiPo ──► LTC2954 ──► TPS22965 ──► TPS62091 ──► 3.3V      │
└─────────────────────────────────────────────────────────────┘
         ┌──────────────────────────────────┐
         │         ESP32-WROVER             │
         │  BLE 5.0 / Wi-Fi 2.4GHz         │
         │  Xtensa dual-core 240 MHz        │
         │  16 MB Flash / 4 or 8 MB PSRAM  │
         └─┬──────┬──────┬──────┬──────┬──┘
           │SPI   │I²C   │UART  │ADC   │GPIO
    ┌──────┴┐ ┌───┴──┐ ┌─┴───┐ ┌┴────┐ ┌┴──────────┐
    │TFT    │ │MAX   │ │UART │ │電源  │ │LED / SW×4 │
    │Touch  │ │17048 │ │Debug│ │OFF  │ │           │
    │SD Card│ │(燃料 │ │Port │ │検出  │ └───────────┘
    └───────┘ │ゲージ│ └─────┘ └─────┘
              └──────┘
```

---

## 2. 主要コンポーネント一覧

| Reference | 部品番号 | 機能 |
|-----------|---------|------|
| U1 | ESP32-WROVER | メイン MCU（BLE/Wi-Fi/CPU） |
| IC1 | MAX17048G+ | LiPo 燃料ゲージ（I²C） |
| IC2 | TPS62091RGTT | 降圧 DC-DC コンバータ（→3.3V） |
| IC3 | BQ24075RGTR | USB 対応 Li-Ion バッテリーチャージャー |
| U2 | TPS22965DSGR | 負荷スイッチ（電源制御） |
| U4 | LTC2954-1/TRMPBF | プッシュボタン電源コントローラー |
| J1 | UJC-HP-3-SMT-TR | USB Type-C コネクター（充電用） |
| SW_1〜4 | B3U-1000P ×4 | タクトスイッチ（電源・リセット・ユーザー×2） |
| BAT | S2B-PH-K-S | LiPo バッテリーコネクター（2ピン JST PH 2.0mm） |
| UART | BM05B-SRSS-TB | UART デバッグ/書込みコネクター（5ピン JST SH 1.0mm） |
| SDCARD0/1 | — | SD カードスロット（SPI） |
| DISPLAY/TOUCH | — | 14ピン ディスプレイ+タッチパネルコネクター |

---

## 3. 電源システム

### 3.1 電源ブロック

```
USB-C (5V)
    │
    ▼
BQ24075RGTR (IC3)
  充電 IC
  ├─ BAT 端子 ──► LiPo バッテリー (B+3V7)
  └─ OUT 端子 ──► システム電源ライン
                        │
                        ▼
               LTC2954-1 (U4) ── 電源ボタン (SW_PW)
               プッシュボタン電源コントローラー
               *PB ← 電源ボタン
               *INT → SENSOR_VP (IO36) ─ ESP32 へ割込み通知
               *KILL ← SW_OFF (IO25) ─ ESP32 からソフト電源断
                        │ EN
                        ▼
               TPS22965DSGR (U2)
               負荷スイッチ
                        │ VOUT
                        ▼
               TPS62091RGTT (IC2)
               同期整流降圧コンバータ
               VIN = B+3V7 (2.3V〜5.5V)
               VOUT = 3.3V (3V3+)
```

### 3.2 電源レール

| レール名 | 電圧 | 供給元 | 用途 |
|---------|------|-------|------|
| B+5V / P+5V | 5V | USB-C（J1） | BQ24075 入力・USB 電源 |
| B+3V7 | 3.0〜4.2V | LiPo バッテリー | メイン電源入力 |
| 3V3+ | 3.3V | TPS62091（IC2） | ESP32・周辺 IC 全般 |
| GND | 0V | 共通グランド | — |

### 3.3 バッテリーチャージャー（BQ24075RGTR）

| パラメータ | 値 | 備考 |
|-----------|---|------|
| 入力電圧 | 4.35V〜6.45V | USB-C 5V 入力 |
| バッテリー | 1セル Li-Ion/LiPo | 4.2V フル充電 |
| 充電電流制限 | ILIM ピン抵抗で設定 | R17 = 1.18kΩ実装 |
| ILIM 設定（参考値） | 約 800〜950 mA | BQ24075 データシート参照 |
| 充電制御 | CE ピン | LOW = 充電有効 |
| 充電状態出力 | CHG ピン（オープンドレイン） | LOW = 充電中 |
| 熱調整 | TS ピン（NTC連携） | 実装による |

> **注意**: BF ピン抵抗（R18 = 46.4kΩ）により充電終止電圧・電流を設定。正確な値は BQ24075 データシートのグラフを参照。

### 3.4 プッシュボタン電源コントローラー（LTC2954-1）

| ピン | 接続先 | 機能 |
|-----|-------|------|
| \*PB (2) | 電源ボタン (SW_PW) | 長押しで電源ON/OFF |
| VIN (3) | B+3V7 | 電源入力 |
| \*KILL (4) | SW_OFF → IO25 | ESP32 からのソフト電源断 |
| EN/\*EN (6) | TPS22965 ON ピン | 負荷スイッチ制御 |
| \*INT (7) | S_VP → IO36 | 電源ボタン押下を ESP32 へ通知 |

**ESP32 側の電源OFF処理**:
- GPIO 36（ADC）で `*INT` 信号を監視（LOW = 電源ボタン押下）
- GPIO 25（`SW_OFF`）を制御して `*KILL` をアサートし、ソフト的に電源断

---

## 4. MCU（ESP32-WROVER）

| 項目 | 値 |
|------|---|
| チップ | ESP32-D0WD-V3 |
| CPU | Xtensa LX6 デュアルコア 240 MHz |
| フラッシュ | 16 MB（QIO / 80 MHz） |
| PSRAM | 4 MB または 8 MB（WROVER に依存） |
| BLE | Bluetooth 4.2 / 5.0 |
| Wi-Fi | IEEE 802.11 b/g/n（2.4 GHz） |
| 動作電圧 | 3.0V〜3.6V（3.3V 推奨） |

---

## 5. GPIO ピンアサイン（回路図より）

### 5.1 全ピン一覧

| GPIO | ネット名 | 方向 | 機能 | 備考 |
|------|---------|------|------|------|
| IO0 | UART_IO0 | I/O | ブートモード / UART | 起動時 LOW = DL モード |
| IO4 | LED | OUTPUT | LED インジケーター | 220Ω 直列抵抗あり |
| IO5 | SD_CS | OUTPUT | SD カード CS | SPI LOW アクティブ |
| IO13 | TOUCH_IRQ | INPUT | タッチパネル割込み | LOW アクティブ |
| IO14 | TFT_BL | OUTPUT | LCD バックライト | HIGH = 点灯 |
| IO18 | SCK | OUTPUT | SPI クロック（共有） | TFT / Touch / SD |
| IO19 | MISO | INPUT | SPI MISO（共有） | TFT / Touch / SD |
| IO21 | I2C_SDA | I/O | I²C データ | MAX17048 接続 |
| IO22 | I2C_SCL | OUTPUT | I²C クロック | MAX17048 接続 |
| IO23 | MOSI | OUTPUT | SPI MOSI（共有） | TFT / Touch / SD |
| IO25 | SW_OFF | OUTPUT | ソフト電源断 | LTC2954 \*KILL へ |
| IO26 | TFT_DC | OUTPUT | TFT データ/コマンド | HIGH = Data |
| IO27 | TFT_CS | OUTPUT | TFT チップセレクト | LOW アクティブ |
| IO32 | TFT_RST | OUTPUT | TFT リセット | LOW アクティブ |
| IO33 | TOUCH_CS | OUTPUT | タッチ CS | LOW アクティブ |
| IO34 | SWITCH_A | INPUT | ユーザーボタン A | 10kΩ プルアップ |
| IO35 | SWITCH_B | INPUT | ユーザーボタン B | 10kΩ プルアップ |
| IO36 | S_VP (SENSOR_VP) | INPUT | ADC 入力 / 電源ボタン検出 | LTC2954 \*INT 接続 |
| IO39 | S_VN (SENSOR_VN) | INPUT | ADC 入力 | 入力専用 GPIO |
| RXD0 | UART_RX | INPUT | UART0 受信 | デバッグ / プログラム |
| TXD0 | UART_TX | OUTPUT | UART0 送信 | デバッグ / プログラム |
| EN | — | INPUT | ESP32 リセット | リセットボタン（RST）接続 |

> **IO36 / IO39**: 入力専用ピン。内部プルアップ/プルダウン非対応。外部回路により電圧が決まる。

### 5.2 ピン制約まとめ

| ピン | 制約 |
|-----|------|
| IO34, IO35, IO36, IO39 | 入力専用（OUTPUT 設定不可） |
| IO6〜IO11 | 内蔵フラッシュ専用（使用不可） |
| IO0 | 起動時 HIGH 必須（通常動作） |
| IO12 | 起動時 LOW 推奨（フラッシュ電圧設定） |

---

## 6. SPI バス（VSPI、3デバイス共有）

| 信号 | GPIO | 接続先デバイス |
|------|------|--------------|
| SCK | IO18 | TFT / タッチパネル / SD カード |
| MOSI | IO23 | TFT / タッチパネル / SD カード |
| MISO | IO19 | TFT / タッチパネル / SD カード |
| TFT_CS | IO27 | TFT ディスプレイのみ |
| TOUCH_CS | IO33 | タッチパネルのみ |
| SD_CS | IO5 | SD カードのみ |

各 CS を排他制御することで同一 SPI バスで 3 デバイスを共有しています。

---

## 7. ディスプレイ

### 7.1 TFT LCD（ILI9341 互換）

| 項目 | 値 |
|------|---|
| ドライバ IC | ILI9341（ILI9341_2_DRIVER） |
| 解像度 | 320 × 240 px（横向き使用） |
| 色深度 | 16-bit RGB565 |
| インターフェース | SPI（SCK/MOSI/MISO/CS/DC/RST） |
| SPI 書込み速度 | 40 MHz |
| SPI 読出し速度 | 6 MHz |
| バックライト | GPIO IO14（HIGH = 点灯） |

| 信号 | GPIO |
|------|------|
| TFT_CS | IO27 |
| TFT_DC | IO26 |
| TFT_RST | IO32 |
| TFT_BL（バックライト） | IO14 |

### 7.2 タッチパネル（XPT2046）

| 項目 | 値 |
|------|---|
| コントローラー | XPT2046 |
| インターフェース | SPI（SCK/MOSI/MISO/CS + IRQ） |
| SPI 速度 | 2.5 MHz |
| 分解能 | 12-bit（0〜4095） |
| タッチ検出 | TOUCH_IRQ（IO13、LOW アクティブ） |

### 7.3 ディスプレイ/タッチ コネクター（14ピン）

本ボードは 14ピンコネクターで TFT + タッチパネルを接続します。  
推定ピン配置（回路図より）:

| ピン番号 | 信号 | 方向 |
|---------|------|------|
| 1 | GND | — |
| 2 | 3V3+ | 電源 |
| 3 | TFT_CS | OUTPUT |
| 4 | TFT_RST | OUTPUT |
| 5 | TFT_DC | OUTPUT |
| 6 | SCK | OUTPUT |
| 7 | MOSI | OUTPUT |
| 8 | MISO | INPUT |
| 9 | TFT_BL | OUTPUT |
| 10 | TOUCH_CS | OUTPUT |
| 11 | TOUCH_IRQ | INPUT |
| 12 | SD_CS | OUTPUT |
| 13 | GND | — |
| 14 | 3V3+ | 電源 |

> ピン配置は搭載するディスプレイモジュールに合わせて変更される場合があります。

---

## 8. SD カード

| 項目 | 値 |
|------|---|
| インターフェース | SPI（SHARED_SPI モード） |
| CS ピン | IO5 |
| SPI クロック | 12 MHz |
| ファイルシステム | FAT32（SdFat32 / FFat） |
| コネクター | SDCARD0 / SDCARD1（基板上スロット） |

---

## 9. I²C バス（バッテリーゲージ）

| 信号 | GPIO | プルアップ |
|------|------|---------|
| SDA | IO21 | 2.2kΩ → 3V3+ |
| SCL | IO22 | 2.2kΩ → 3V3+ |

### 9.1 MAX17048G+（燃料ゲージ IC）

| 項目 | 値 |
|------|---|
| I²C アドレス | 0x36（7-bit） |
| 電源電圧 | 2.5V〜5.5V |
| セル構成 | 単セル（1S LiPo） |

#### 主要レジスタ

| レジスタ | アドレス | 説明 | 換算式 |
|---------|---------|------|--------|
| VCELL | 0x02 | セル電圧 | 電圧[V] = 値 × 78.125 μV |
| SOC | 0x04 | 充電残量 | 残量[%] = 上位バイト |
| MODE | 0x06 | クイックスタート / スリープ | — |
| CONFIG | 0x0C | RCOMP / アラート | — |
| VALRT | 0x14 | 電圧アラート閾値 | 閾値[V] = 値 × 0.02 |
| CRATE | 0x16 | 充放電レート | レート[%/h] = 値 × 0.208 |
| STATUS | 0x1A | アラートフラグ | — |

---

## 10. ADC 入力

| GPIO | ネット名 | 分解能 | 減衰設定 | 最大入力 | 用途 |
|------|---------|--------|---------|---------|------|
| IO36 | S_VP / SENSOR_VP | 12-bit | 11 dB | 約 3.1V | 電源ボタン検出 / 汎用 ADC |
| IO39 | S_VN / SENSOR_VN | 12-bit | 11 dB | 約 3.1V | 汎用 ADC |

**換算式**:
```
電圧 [V] = ADC値 / 4095 × 3.1
```

**IO36 電源ボタン検出ロジック**（LTC2954 \*INT 接続時）:
| S_VP 電圧 | 判定 |
|-----------|------|
| ≤ 1.4V | 電源ボタン押下（電源OFF処理開始） |
| ≥ 1.5V | 通常状態（ヒステリシス） |

---

## 11. スイッチ / ボタン

4個の B3U-1000P タクトスイッチを搭載。

| 名称 | 接続先 | 機能 |
|------|-------|------|
| PW（電源） | LTC2954 \*PB → IO36 | 長押しで電源ON/OFF |
| RST（リセット） | ESP32 EN ピン | ハードウェアリセット |
| SW_A（ユーザー A） | IO34（10kΩ プルアップ） | ユーザー定義 |
| SW_B（ユーザー B） | IO35（10kΩ プルアップ） | ユーザー定義 |

> SW_A / SW_B は通常 HIGH、押下時 LOW（アクティブ LOW、プルアップ済み）。

---

## 12. LED インジケーター

| 項目 | 値 |
|------|---|
| GPIO | IO4 |
| 極性 | HIGH = 点灯 |
| 直列抵抗 | 220Ω |
| 制御 | GPIO OUTPUT によるソフト制御 |

---

## 13. UART デバッグ / 書込みコネクター

**コネクター**: BM05B-SRSS-TB（5ピン JST SH 1.0mm ピッチ）

| ピン | 信号 | 説明 |
|-----|------|------|
| 1 | GND | グランド |
| 2 | UART_RX | ESP32 受信（TXD0 / IO1） |
| 3 | UART_TX | ESP32 送信（RXD0 / IO3） |
| 4 | UART_IO0 | IO0（LOW = ダウンロードモード） |
| 5 | UART_EN | ESP32 EN（リセット制御） |

- ボーレート: 115200 bps（デフォルト）
- UART-USB ブリッジ（CP2102 / CH340 等）を外付けして PC 接続
- ファームウェア書き込み: IO0 を LOW にした状態で EN をリセット

---

## 14. USB Type-C コネクター

**コネクター**: UJC-HP-3-SMT-TR

| ピン | 信号 | 説明 |
|-----|------|------|
| A5, B5 | VBUS_1, VBUS_2 | +5V 入力（充電用） |
| A9, B9 | GND_1, GND_2 | グランド |
| A1, B1 | CC1, CC2 | USB-C Configuration Channel |
| MH1〜MH4 | — | メカニカル固定 |

> **注意**: 本コネクターは充電専用です。USB データ（D+/D-）は接続されていません。ファームウェア書込みは UART コネクターを使用してください。

---

## 15. バッテリーコネクター

**コネクター**: S2B-PH-K-S（2ピン JST PH 2.0mm ピッチ）

| ピン | 信号 | 説明 |
|-----|------|------|
| 1 | BAT+ | LiPo バッテリー正極（B+3V7） |
| 2 | BAT- / GND | バッテリー負極 |

- 対応バッテリー: 1セル LiPo（3.7V 公称、4.2V フル充電）
- 逆接防止回路の有無はバッテリー側に依存（要注意）

---

## 16. フラッシュ パーティション構成

ファームウェア開発時は以下のパーティション CSV を使用することを推奨します（`partitions_ffat.csv`）。

| 名前 | タイプ | サブタイプ | オフセット | サイズ | 用途 |
|------|--------|-----------|-----------|--------|------|
| nvs | data | nvs | 0x009000 | 20 KB | 設定・キャリブレーション等 |
| otadata | data | ota | 0x00E000 | 8 KB | OTA 管理 |
| app0 | app | ota_0 | 0x010000 | 2 MB | アプリケーション |
| ffat | data | fat | 0x210000 | 13.8 MB | FAT ファイルシステム（ログ等） |
| phy_init | data | phy | 0xFE0000 | 4 KB | RF キャリブレーション |
| coredump | data | coredump | 0xFF0000 | 64 KB | クラッシュダンプ |

> NVS はキャリブレーションデータ・暗号鍵・BLE アドレス等の永続化に使用できます。

---

## 17. NVS（不揮発性ストレージ）活用例

namespace `storage` を使った永続化の例:

| キー | 型 | 用途例 |
|------|---|-------|
| `wifi_ssid` | string | Wi-Fi SSID |
| `wifi_pass` | string | Wi-Fi パスワード |
| `ble_last_addr` | string | 前回接続 BLE アドレス |
| `touch_cal` | blob | タッチキャリブレーション係数 |
| `aead_key` | blob (32B) | データ暗号化鍵 |
| `user_config` | blob | アプリ固有設定 |

---

## 18. タッチキャリブレーション（アフィン変換）

タッチパネルは XPT2046 の生座標をアフィン変換で画面座標に変換します。

```
x_screen = a × x_raw + b × y_raw + c
y_screen = d × x_raw + e × y_raw + f
```

係数 a〜f は 5点タップ校正（最小二乗法）で算出し、NVS に保存します。  
独自ファームウェアで本ボードのタッチを利用する場合は同等のキャリブレーションを実装してください。

---

## 19. FreeRTOS タスク設計指針

ESP32-WROVER はデュアルコアのため、以下の分担が推奨されます。

| コア | 用途 | 注意 |
|------|------|------|
| Core 0 | BLE / Wi-Fi 通信タスク | Arduino `loop()` はデフォルト Core 1 |
| Core 1 | UI 描画・センサー処理・ログ | `lv_task_handler()` は Core 1 で実行 |

タスク作成例（Arduino / ESP-IDF 共通）:
```c
xTaskCreatePinnedToCore(ble_task,  "BLE",  8192, NULL, 3, NULL, 0); // Core 0
xTaskCreatePinnedToCore(ui_task,   "UI",   8192, NULL, 2, NULL, 1); // Core 1
```

---

## 20. 電源シーケンス

```
1. USB-C 接続 or 電源ボタン (PW) 長押し
   ↓
2. LTC2954 が TPS22965 の ON ピンをアサート
   ↓
3. TPS22965 が ON → TPS62091 に電力供給
   ↓
4. TPS62091 が 3.3V 出力開始
   ↓
5. ESP32-WROVER 起動 → ファームウェア実行
   ↓
6. GPIO IO14 HIGH → LCD バックライト点灯
   GPIO IO4 HIGH  → LED 点灯

--- 電源OFF シーケンス ---
A. 電源ボタン長押し → LTC2954 が *INT を LOW
   ↓ IO36 で検出
B. ESP32 がシャットダウン処理（ログ保存等）
   ↓
C. ESP32 が IO25 (SW_OFF) を制御 → LTC2954 *KILL アサート
   ↓
D. LTC2954 が TPS22965 をオフ → システム電断
```

---

## 21. 開発・書込み手順

### 必要なもの
- USB-UART ブリッジ（例: CP2102, CH340, FT232）
- JST SH 1.0mm 5ピンケーブル（UART コネクター用）
- Arduino IDE または PlatformIO

### 書込み手順
1. UART コネクターに USB-UART ブリッジを接続  
   `GND↔GND、TX↔UART_RX、RX↔UART_TX、IO0↔IO0、EN↔EN`
2. IO0 を GND に接続（LOW）
3. EN ピンを一瞬 GND → 解放（リセット）
4. ESP32 がダウンロードモードで起動
5. ファームウェアを書込み（921600 bps 推奨）
6. IO0 を解放、再度 EN をリセット → 通常起動

### PlatformIO 設定例（`platformio.ini`）
```ini
[env:avlc_board]
platform = espressif32
board = upesy_wrover
framework = arduino
board_build.flash_mode = qio
board_build.f_flash    = 80000000L
board_build.flash_size = 16MB
board_upload.flash_size = 16MB
board_build.psram = enabled
board_build.partitions = partitions_ffat.csv
upload_speed = 921600
```

---

## 22. 注意事項・設計制約

| 項目 | 内容 |
|------|------|
| GPIO 36 / 39 | 入力専用。内部プルアップ使用不可 |
| SPI 共有 | TFT / タッチ / SD は CS を排他制御すること |
| IO0 | 起動時は HIGH（プルアップ）。誤って LOW に固定しないこと |
| バッテリー逆接 | コネクター極性を必ず確認。IC 損傷の恐れあり |
| USB-C | データ線未接続。充電専用 |
| 最大電流 | TPS62091 出力電流: データシート参照（最大 1A 程度） |
| 動作温度 | ESP32-WROVER: -40℃〜+85℃ |

---

## 付録A. ピン配置サマリー（クイックリファレンス）

```
┌─────────────────────────────────────┐
│         ESP32-WROVER ピン割り当て    │
├────────┬──────────────┬────────────┤
│ GPIO   │ ネット        │ 機能       │
├────────┼──────────────┼────────────┤
│ IO4    │ LED          │ LED 出力   │
│ IO5    │ SD_CS        │ SD カード CS│
│ IO13   │ TOUCH_IRQ    │ タッチ IRQ │
│ IO14   │ TFT_BL       │ バックライト│
│ IO18   │ SCK          │ SPI クロック│
│ IO19   │ MISO         │ SPI MISO  │
│ IO21   │ I2C_SDA      │ I²C SDA   │
│ IO22   │ I2C_SCL      │ I²C SCL   │
│ IO23   │ MOSI         │ SPI MOSI  │
│ IO25   │ SW_OFF       │ 電源断制御 │
│ IO26   │ TFT_DC       │ TFT DC    │
│ IO27   │ TFT_CS       │ TFT CS    │
│ IO32   │ TFT_RST      │ TFT リセット│
│ IO33   │ TOUCH_CS     │ タッチ CS  │
│ IO34   │ SWITCH_A     │ ボタン A   │
│ IO35   │ SWITCH_B     │ ボタン B   │
│ IO36   │ S_VP         │ ADC/電源ボタン検出│
│ IO39   │ S_VN         │ ADC 入力  │
│ RXD0   │ UART_RX      │ UART 受信  │
│ TXD0   │ UART_TX      │ UART 送信  │
└────────┴──────────────┴────────────┘
```

---

## 付録B. 関連ライブラリ推奨バージョン

| ライブラリ | 推奨バージョン | 用途 |
|-----------|-------------|------|
| Arduino ESP32 | 2.0.17 | ベースフレームワーク |
| lvgl | 8.4.0 | GUI |
| TFT_eSPI | 2.5.43 | TFT ドライバ |
| NimBLE-Arduino | 1.4.2 | BLE |
| SdFat | 2.x | SD カード |
| XPT2046_Touchscreen | 1.4.x | タッチパネル |

---

*本仕様書は回路図 main_bord_Ver1_Rev1.sch および FW ソースコード（main.cpp v1.41）に基づいて作成されました。*  
*回路図: /Users/koderayuuya/Documents/EAGLE/projects/AVLC/main_bord_Ver1_Rev1.pdf*
