---
title: Monita Gateway v1.21 ファームウェア（FW 99 / Deck子機の受信＋ダウンリンク対応）
domain: iot_device
tags: [gateway, nRF52840, SIM7080G, BLE, LoRa, E220-900T22S, LTE-M, DS3231, SD, GAS, DIP, downlink]
updated: 2026-09-12
---

# gateway_v1.21 — 基板 ver1.21 用（FW 99）

## v1.20 との関係【最初に読むこと】

| | gateway_v1.20 | **gateway_v1.21（ここ）** |
|---|---|---|
| 対応基板 | ver1.20 | **ver1.21**（ver1.20 から**部品配置だけ**変更。回路・ピン割当は同一） |
| FW版数 | **97 で凍結** | **99**（以降ここで上げる） |
| Deck 子機 | **非対応** | **対応**（受信 FW 98 ＋ ダウンリンク FW 99） |
| 位置づけ | 稼働中の Flex 現場。触らない | **今後の開発はこちら** |

**回路が同一なので、このファームは v1.20 基板でもそのまま動く。**
それでもディレクトリを分けたのは、**稼働中の Flex 現場（v1.20 基板・FW 97）に手を入れないため**である。

> **★ディレクトリを分けてよいのは「基板が変わったとき」だけ。**
> ソフトの版は `GATEWAY_FW_VERSION`（… 97 → 98 → **99**）で表す。
> 機能を足すたびにディレクトリを増やすと、同じ修正を両方へ入れ続けることになる
> （実例：`project06_yokogawa/gateway_v1.2` は FW 101、`gateway_v1.20` は FW 97 まで乖離した）。

> **★v1.20 と v1.21 は今まさに乖離している。**FW 97 に共通の不具合が見つかったら、
> **両方に入れるか、v1.20 を捨てるかを必ずその場で決めること。**片方だけ直すと上の実例を繰り返す。

## FW 99（2026-09-12）— Deck 子機のダウンリンク対応

正本: [Deck Gateway改修範囲](../../../【7】Monita/01_開発/Deck基板/20260909_Monita_Deck_Gateway改修範囲.md)

## 設定の場所（製造・現地設置の担当者向け）

**`src/main.cpp` の冒頭に「設定早見表」がある。**ソースを追う前にまずそこを見る。
各設定には `★【設定N】` の目印が付いているので、エディタで `【設定` を検索すれば飛べる。

| # | 設定 | 場所 |
|---|---|---|
| 1 | **受信する子機の型** | `src/main.cpp` の `SENSOR_FRAME_TYPES[]`。**新しい子機は1行足すだけ** |
| 2 | Gateway 群 | ビルド時 `-D GATEWAY_GROUP_ID=n`（省略時 群0） |
| 3 | GAS の宛先 | `src/main.cpp` の `GAS_SCRIPT_ID` |
| 4 | SIM / APN | `src/main.cpp` の `SIM_1NCE` / `SIM_PLAND` |
| 5 | LoRa / BLE / クラウド形式 | `platformio.ini` の `build_flags` |
| 6 | BLE 子機の一覧 | `src/main.cpp` の `ALLOWED_DEVICE_IDS[]`（BLE のみ。LoRa は不要） |
| 7 | 送信間隔 | 基板上の DIP スイッチ（ソース変更不要） |

### ★書き込んだら必ずシリアルを見ること

起動時に**実際に効いている設定が全部出る**（`printConfigSummary()`）。

```
---- 設定サマリ ----------------------
[設定5] 通信モード : LoRa (E220-900T22S)
[設定2] Gateway群  : 0  → 受け付ける子機 DeviceID: 0x1 〜 0x1F
[設定4] SIM / APN  : 1NCE / iot.1nce.net
[設定3] GAS宛先    : AKfycbzKVvW6...
        クラウド形式: 既定（13B/台）
[設定1] 受信する子機の型:
          pktType 0x04  19B  &pt=なし(従来形式)  Flex/One センサ
          pktType 0x06  21B  &pt=06            Deck 静的(6CH変位)
          pktType 0x07  22B  &pt=07            Deck イベント統計
--------------------------------------
```

**群の焼き間違い・GAS宛先の貼り忘れ・子機型の追加漏れ**は、いずれもここで気づける。

### 使えない pktType

`0x05`（Flex用ACK）／`0x81`（Flex用ダウンリンク）／`0x82`（Deck用ダウンリンク）／`0x83`（Deck用ACK）。
いずれも **FW 99 時点で実際に使用中**である。特に **0x05 と 0x83 は長さ検証より前に ACK として
処理される**ため、センサ型に使うとデータが消える。

## FW 98 で入れた変更（Deck Gateway改修範囲 G-0〜G-4）

| # | 内容 |
|---|---|
| **G-0/G-1** | pktType の単一値判定を `SENSOR_FRAME_TYPES[]` の型テーブルへ置換。0x04(19B) / 0x06(21B) / 0x07(22B) を型ごとに厳密長で検証 |
| **G-2** | pseudoMac の5バイト目に pktType を入れ、同一 DeviceID の静的(0x06)とイベント統計(0x07)が上書きし合うのを防ぐ。**Flex(0x04) は従来どおり 0 のまま**（0x04 を入れると SD ログの mac 列が現場で見えている値から変わる） |
| **G-3** | `buildBatchQuery()` を型別バッチ化。Deck は `&pt=06/07` を付けて `Epoch+payload` をそのまま16進で送る。Gateway はレコードの中身を解釈しないので、**子機側の定義を変えても Gateway の改修は要らない** |
| **G-4** | `count==0` で `start` が進まない無限ループを修正。既存の潜在不具合だが、型が混ざる Deck 対応で現実に踏みうる経路になった |

**Flex しかいない現場では FW 97 と挙動が完全に同一**になるよう作ってある（`&pt=` を付けない・pseudoMac も従来どおり）。

## FW 99 で入れた変更（G-5 と、GAS 側 A-1〜A-3）

| # | 内容 |
|---|---|
| **G-5** | Deck用ダウンリンク（`0x82` / **24B**）と ACK（`0x83` / 17B）。既存の `0x81`/15B はそのまま温存 |
| **5.2** | Flex用ACK(`0x05`)の長さ検証を `len >= 7` から **7 または 9 の厳密一致**へ（改修範囲メモ §5.2 の指摘） |
| **A-1** | GAS：`&pt=06/07` を見て Deck 形式をパースし、`deck_<HEX>` / `deck_<HEX>_event` シートへ振り分ける |
| **A-2** | GAS：予約に `kind:'deck'` と `trigHex` を追加。予約行の**8番目のフィールド**（16進26文字）で運ぶ |
| **A-3** | GAS：`deck_trigger` シートと［Deck操作］メニュー |

### Deck のダウンリンクを 17B → 24B に変えた理由

改修範囲メモの当初案は `CompanyID2 + PktType1 + DeviceID1 + トリガ13 = 17B` だった。
実装時に **Flags 1B と 時刻 6B を足して 24B** にしてある。

- **時刻6B**：要件 F-23。**Deck には RTC のバックアップ電池が無い**（§11.4 で BT1 廃止）ので、
  停電から復帰すると子機は自力で時刻を取り戻せない。親機が配り続けるのが唯一の手段である。
  ダウンリンクは既に「子機が起きた瞬間に届く」経路なので、ここに相乗りさせるのが一番安い。
- **Flags 1B**：Flex の `0x81` と構造を揃えるため。`DL_FLAG_TIME` / `DECK_FLAG_TRIG` で
  「時刻だけ配る」「設定だけ変える」を後から足せる。
- 32B の送信バッファに対して 24B なので余裕がある。**子機ファームを書く前に形式を凍結できる
  今のうちに広げておく**方が、あとで両側とGASを直すより安い。

### 予約行を8フィールドにした理由

Deck 専用の取得経路を別に作らず、Flex と同じ予約キャッシュ・同じ再送・同じ報告に相乗りさせた。
`HEX2:sleep:avg:median:attempts:seq:mode` の後ろに **トリガ設定の16進26文字を足すだけ**である。

- 8番目が無ければ Flex、有れば Deck。**GAS が古いままでも Flex は従来どおり動く。**
- 再送回数・`seq` の取り違え・キュー満杯時の扱いといった、実機で潰した不具合を2度踏まずに済む。

トリガ設定13Bのバイト配置は、子機側の `case04_Deck/v1.00/lib/DeckMeasure/DeckMeasure.cpp` の
`TriggerConfig::fromBytes()` と**必ず一致させること**。片方だけ直すと静かにずれる。
実装時に GAS のエンコーダと C++ の `fromBytes()` を実際に突き合わせて一致を確認してある
（既定値・上限値・下限値の3組で往復一致、および不正値6種が両側で拒否されること）。

## 現地での使い方（Deck）

1. `Code.gs` の `DECK_CHILD_IDS` に Deck の DeviceID（16進2桁）を足す
2. スプレッドシートで［Deck操作］→［トリガ設定シートを準備］
3. `deck_trigger` シートの行を埋める（各列のヘッダーにマウスを乗せると意味と範囲が出る）
4. ［Deck操作］→［トリガ設定を送信］
5. 子機が次に電波を出したとき（**Deck は60分周期なので最大1時間**）に届き、
   `deck_trigger` シートの「状態」列に子機が実際に適用した値が返る

**送った値ではなく、返ってきた値を見ること。**子機が丸めたり拒否したりした場合はここに出る。

---

# gateway_v1.21（基板 ver1.21 / 回路は v1.1・v1.20 と同一）

**v1.1 をベースに、LoRaダウンリンク（スプレッドシート → GAS → Gateway → 子機の設定変更）を追加する版。**
基板（回路）はv1.1と同一で、変更はファームウェアのみ。ダウンリンクの設計・実機検証の経緯は
`case02_Gateway/test_sketches/03_lora_downlink_sender`（Gateway側の実験台）と
`case01_Flex/test_sketches/25_lora_downlink_child`（子機側の実験台）を参照。

対応する子機ファーム: `case01_Flex/v3.20`

Monita Flex（子機）から **BLE アドバタイジング、または LoRa（E220-900T22S(JP)）** で受信したセンサデータを LTE-M 経由で GAS（Google Apps Script）に送信する Gateway ファームウェア。
**電源は XIAO nRF52840 の Type-C 給電（AC電源）、全部品 DIP 対応。** BLE / LoRa は `platformio.ini` の `build_flags` でビルド時選択（`COMM_MODE_BLE` / `COMM_MODE_LORA`、Flex側の切替と同じ考え方）。

**★2026-07-17: 電源方式をLiPoバッテリー駆動からAC電源（Type-C給電）に変更。** それに伴いTCA9534・AO3401・MMBT3904・TPS61232・TPS22965・RC遅延回路一式を削除し、SIM7080Gの電源はXIAOの5Vに直結（v1.0と同じ方式）に戻した。旧バッテリー駆動設計は `gateway_v1.10_ARCHIVE_battery_TCA9534_design.md` にアーカイブ済み（将来復活の可能性あり）。

要件定義: `【7】Monita/開発/Gatway基板/gateway_requirements_v1.10.md`（「LoRa受信対応」章に詳細）
v1.0からの差分: `【7】Monita/開発/Gatway基板/gateway_v1.00_to_v1.10_diff.md`
対応するFlex側: `【7】Monita/開発/Flex基板/Monita_Flex_構成_v3.10.md`
バッテリー駆動設計アーカイブ: `【7】Monita/開発/Gatway基板/gateway_v1.10_ARCHIVE_battery_TCA9534_design.md`

## ハードウェア構成

| 役割 | 部品 |
|------|------|
| MCU | Seeed XIAO nRF52840 |
| 通信 | M5Stamp CAT-M（SIM7080G）、（LoRaビルドのみ）E220-900T22S(JP)-EV2 |
| RTC | DS3231 |
| ストレージ | microSD（SPI） |
| 電源 | XIAO nRF52840 Type-C給電（AC/USBアダプタ）。全部品DIP |

## 配線（v1.1、AC電源版）

| 信号 | XIAO ピン | 接続先 |
|------|-----------|--------|
| UART TX | D6 | SIM7080G RX |
| UART RX | D7 | SIM7080G TX |
| I2C SDA | D4 | DS3231 SDA |
| I2C SCL | D5 | DS3231 SCL |
| SPI SCK | D8 | SD CLK |
| SPI MISO | D9 | SD DAT0 |
| SPI MOSI | D10 | SD CMD |
| SD CS | D3 | SD CS（直結、net N$6） |
| LoRa RX（LoRaビルドのみ） | D0 | E220 TXD（net UART_RX_2） |
| LoRa TX（LoRaビルドのみ） | D1 | E220 RXD（net UART_TX_2） |
| LoRa M0/M1（LoRaビルドのみ） | D2 | E220 M0・M1（基板側で両ピンを短絡し1本のGPIOで共通駆動、net LORA_SETTING） |
| 5V | 5V | SIM7080G 5V（Type-C給電時のみ通電、v1.0と同じ直結） |
| 3V3 | 3V3 | DS3231 VCC / SD VDD / E220 VCC |

**★2026-07-19**: 回路図 `ver1.10.sch`（netlist_gateway_1）に合わせてピン割当を確定。E220のM0/M1は基板上で短絡済み（LORA_SETTINGネット）。

**⚠️ 実機未検証**: UARTE1経由のLoRa受信は基板完成前のため実機での動作確認が済んでいない。

## SIM 切り替え

`src/main.cpp` の冒頭の define を切り替える：

```cpp
#define SIM_1NCE    // 1NCE SIM を使う場合
// #define SIM_PLAN_D  // Plan-D SIM を使う場合
```

## GAS 設定

`GAS_SCRIPT_ID` にデプロイ URL の ID 部分（`AKfycb...`）を設定する。

GAS 側の `doGet(e)` は「子機データ行」と「起動確認用の設定情報行」の2種類を受ける：

### 子機データ（通常送信・複数台バッチ対応）

| パラメータ | 内容 |
|-----------|------|
| `ts` | タイムスタンプ |
| `sim` | 使用 SIM 名 |
| `csq` | SIM7080G 自身のセルラー受信電波強度（0-31, 99=圏外） |
| `n` | このリクエストに含まれる Flex 台数 |
| `m{i}` | i番目の Flex の MAC アドレス |
| `p{i}` | i番目の Manufacturer Data ペイロード（HEX文字列） |
| `r{i}` | i番目の BLE RSSI (dBm) |

payload は GAS 側で PktType・DeviceID・CH1〜CH6・FlexHour/Min に汎用パースする（バイト長からチャンネル数を自動判定。将来12バイト/6チャンネル拡張に対応）。

### Monita One / クラウド形式V2

Oneを接続するGatewayでは`platformio.ini`の
コメント例に従って`CLOUD_FMT_V2`を有効にする。既定（フラグなし）は旧GAS互換の
13バイト/台（26 hex）のままで、V2時だけ電池電圧を末尾へ追加した14バイト/台
（28 hex）になる。対向GASには`gas/one/Code.gs`を使用する。

### 起動確認情報行（`row_type=info`）

| パラメータ | 内容 |
|-----------|------|
| `xiao_id` | XIAO nRF52840 固有ID（FICR DEVICEID） |
| `sim_imei` | SIM7080G の IMEI |
| `sd` | SDカード記録の有無（0/1） |
| `interval_min` | 定期送信インターバル（分） |
| `devcount` | 起動時点で受信済みの子機台数 |
| `gw_fw` | Gatewayファームのバージョン |
| `gw_id` | Gateway個体ID（`gw_<XIAO固有ID16桁>`。GAS上の個体識別キー） |
| `group` | このGatewayが受信する子機の群番号（0〜7） |

## 子機DeviceIDと群（GATEWAY_GROUP_ID）

1つの現場にGatewayを複数台置くと、E220の設定が全機共通のため両方が同じ子機を
二重受信してしまう。無線層（チャネル）は変えず、DeviceIDを分割してソフトフィルタで
受信を分離する。

```
DEVICE_ID (1バイト)
  上位3bit = Gateway群 (0〜7)       → group   = deviceId >> 5
  下位5bit = 群内の機器番号 (1〜31)  → localNo = deviceId & 0x1F   ※0は無効値
```

- 群0 = `0x01`〜`0x1F`、群1 = `0x21`〜`0x3F`、群N の開始は `N × 0x20 + 1`
- 収容能力は8群 × 31台。1Gatewayあたり最大31台（`MAX_DEVICES=32` / `MAX_PENDING_CHILDREN=31`）
- 既存の `0x01`〜`0x0F` はすべて群0に収まるため、稼働中の機器の焼き直しは不要
- **LoRa受信のみ**この方式（`isAllowedLoRaPacket()`）。BLE受信は従来の
  `ALLOWED_DEVICE_IDS[]` ホワイトリスト（`0x01`〜`0x0F`）のままで、群分離は第3段階
- 全群でダウンリンクを利用できる。Gatewayは`check_cmd`へ`group`を付け、GASは
  `childId >> 5`が一致する予約だけを返す（Gateway側でも取込時に群を再検証する）

2台目以降のGatewayはビルド時に群を指定して焼く。

```sh
PLATFORMIO_BUILD_FLAGS="-D GATEWAY_GROUP_ID=1" pio run -t upload
```

Gateway個体を識別する `GW_DEVICE_ID` は、XIAO固有ID（FICR DEVICEID）から
`gw_<16桁hex>` を自動生成する。GAS上のリモートコマンド予約キー
（`pending_cmd_<deviceId>`）・`status_report`・`log_dump` に使うため、
Gateway 2台が同じIDになると予約の取得競合や記録の混在が起きる。
XIAO交換＝Gateway ID変更になるので、現場名（論理名）はファームに焼かず、
GAS台帳で「論理名 ↔ XIAO固有ID」を対応付けること。

## ビルド

`platformio.ini` 末尾の `build_flags` で `COMM_MODE_BLE` / `COMM_MODE_LORA` のいずれか1つのコメントを外して選択する（既定はBLE）。

```bash
cd firmware/gateway_v1.1
pio run
pio run --target upload
```

## 動作フロー

1. 起動時に DS3231・SD・BLE・SIM7080G を初期化
2. SIM7080G 起動待ち完了直後から BLE スキャン開始（ネットワーク初期化と並行）
3. ネットワーク接続完了後、起動確認情報行＋受信済み子機データを送信
4. 以降は一定間隔（`SEND_INTERVAL_MS`）でバッファをフラッシュし、複数 Flex をまとめて1回の GET で GAS に送信
5. 送信失敗時は再送キューに保持し、次回サイクルでライブデータとマージして再送
6. 送信前に SD カード（`gateway.csv`）にバックアップ記録
7. ウォッチドッグタイマー（120秒）により無人運用中のハングから自動復旧

## v1.0 からの主な変更点

詳細は `gateway_v1.00_to_v1.10_diff.md` を参照。要点：

- ファームウェアは v1.0 で確立した通信安定化策一式（WDT・再送キュー・BLEフィルタ・BLEスキャン前倒し）をベースラインとして継承
- BLE / LoRa（E220-900T22S(JP)）をビルド時選択（`COMM_MODE_BLE` / `COMM_MODE_LORA`）で追加
- 電源はAC電源（XIAO Type-C給電）、全部品DIP。SIM7080GはXIAOの5Vに直結（v1.0と同じ方式）
- （旧検討）LiPoバッテリー駆動＋TCA9534によるSMD化は `gateway_v1.10_ARCHIVE_battery_TCA9534_design.md` にアーカイブ済み
- **★2026-07-18**: DS3231の網時刻自動設定（`AT+CCLK?`）、BLE MSD解析の境界チェック、LoRa送信中取りこぼし対策、LoRa設定書込検証、送信バッチの8台分割を追加
- **★2026-07-18**: コントローラー連携（BLE設定）をLoRaビルドに追加。送信間隔変更・コマンド（即時送信/NW再登録/起動確認/リセット）・ステータス通知をBLE GATTで提供。詳細は要件定義「コントローラー連携（BLE設定）」章
- **★2026-07-19**: 回路図 ver1.10.sch に合わせてピン割当を確定。SD CS=D3、LoRa RX=D0/TX=D1/M0M1=D2
- **★2026-07-21**: アプリ層ウォッチドッグを追加。30分間GAS送信成功が無ければ `NVIC_SystemReset` で強制再起動。ハードWDTでは捕捉できない「モデム接続維持のまま送信だけ失敗し続けるソフトハング」への対策（有野川現場2026-07-17停止の教訓）
- **★2026-07-21**: 段階的復旧を追加。送信が3サイクル連続で全滅したら、全再起動の前に**モデムのソフトリセット（`CFUN=0/1`＋再接続）**を先に試す（`modemSoftReset()`）。SSL/HTTPスタック固着への軽量・高速な一段目の復旧手段
- **★2026-07-21**: `sendAT()` の応答バッファに上限（2048バイト、`SENDAT_MAX_RESPONSE_LEN`）を追加（`GATEWAY_FW_VERSION` 7）。配線ノイズ等でRX1にゴミデータが流れ込み続けた場合のヒープ枯渇を防止。電波状況良好な卓上テストでも発生した突然停止の根本原因と推定

## 関連タスク

- `tsuruta_tasks.md` — Monita Gateway 開発タスク
- `【7】Monita/開発/Gatway基板/gateway_requirements_v1.10.md` — 要件定義（v1.1）
- `【7】Monita/開発/Gatway基板/gateway_requirements_v1.00.md` — 要件定義（v1.0、旧版）
