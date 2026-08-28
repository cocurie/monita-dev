---
title: 機器識別体系の再設計（Gateway個体識別 + 群分離）改修計画
domain: iot_device
tags: [MonitaOne, MonitaFlex, Gateway, DeviceID, LoRa, GAS, 設計, 改修計画]
updated: 2026-08-26
status: 第4版（Codexレビュー2回反映済み）／実装未着手
---

# 機器識別体系の再設計（Gateway個体識別 + 群分離）改修計画

> **版の経緯**
> - **第1版**：DeviceIDの上位4bitをGateway群に使う案のみ。→ Codexレビューで3つの重大な欠落が判明
> - **第2版**：Gateway個体識別・GAS製品台帳・ダウンリンク群別配信を追加。→ 再レビューで移行順序の危険性が判明
> - **第3版**：段階リリース構成に再編。ダウンリンクがテスト段階で実害がないという前提を反映し、
>   第1段階をアップリンクのみに絞った。BLEは後回しと決定。
> - **第4版（本版）**：**ビット分割を 4bit/4bit → 3bit/5bit に変更**（2026-08-26決定）。
>   1Gatewayあたり15台では想定台数（20台）に届かないため、**8群 × 31台**へ拡張した。
>
> レビュー指摘の反映位置は「★指摘N」として本文中に示す。

---

## 0. 決定事項（2026-08-26）

| 項目 | 決定 |
|---|---|
| **ダウンリンクの現況** | **全てテスト段階。現場実装なし。何をしても実害はない** |
| 実装順序 | **アップリンクを先に実装**（段階リリース） |
| Monita One の構成 | **標準センサ版とPIR版の両方を想定** → `ONE_SENSOR` / `ONE_PIR` 両プロファイルが必要 |
| BLE の群分離 | **後回し。** LoRaがあるためBLEの使用機会は今後激減する見込み |
| `GW_DEVICE_ID` | **XIAO固有ID 16桁をそのまま使う**（下位8桁への切り詰めはしない） |
| **ビット分割** | **上位3bit=Gateway群（8群）／下位5bit=機器番号（1〜31）**。1Gatewayあたり最大31台 |

---

## 1. 背景と課題

1つの現場に **Gateway 2台、各Gatewayに Flex 3台 + One 3台** という構成を予定している。
現在の設計ではこれが成立せず、**課題は独立した3層**に分かれる。

### 層1：無線受信の分離

E220の設定は One・Gateway とも `{ADDH=0x00, ADDL=0x00, REG0=0x68, REG1=0x01, REG2=0x00(CH0), REG3=0x80}` で
全機共通。Gatewayの受信フィルタは `isAllowedFlexPacket(pktType, deviceId)` によるIDホワイトリストのみ。
ペイロード19バイトにGateway識別フィールドもない。

→ **2台のGatewayが同じ子機を二重受信する。**

### 層2：Gateway個体のクラウド識別（★第1版で欠落）

```c
// gateway_v1.20/src/main.cpp:1076
static char const* GW_DEVICE_ID = "gateway_v11_test";   // ← 固定文字列
```

L1117（status_report）、L1163（log_dump）、L1446（check_cmd）、L1483（ack_cmd）で使用。
2台が同じIDでGASに接続すると、コマンド予約の取得競合、status/logの混在が起きる。

**さらに、`GW_DEVICE_ID` を一意化しても不十分**（★2版レビュー重大2）。
GASの `status_report` 処理は `device_id` を受け取っているのに保存していない。

```javascript
// gateway_common/Code.gs:1247-1258
statusSheet.appendRow([
  new Date(),
  'GW',        // ← B列は固定文字列。p.device_id を捨てている
  ...
```

→ **GAS側の保存改修もセットで必要。**
なお `log_dump`（`Code.gs:1266-1275`）とリモートコマンド（`pending_cmd_<deviceId>`）は
`deviceId` を使っているため、一意化の効果がある。

### 層3：クラウド側のデータ解釈（★第1版で欠落）

製品種別は**完全な2桁hex**をキーにしている。

```javascript
// gateway_common/Code.gs:896-903
var productKey = DEVICE_PRODUCT_REGISTRY[deviceIdHex_(deviceId)] || DEFAULT_PRODUCT_TYPE;
```

**現状の実装状況（★2版レビューで訂正）**

| ファイル | 状態 |
|---|---|
| `gateway_common/Code.gs:635-650` | **空のテンプレート**。Oneプロファイル自体が無い |
| `gas/one/Code.gs:633-664` | `PRODUCT_PROFILES` に **`ONE_PIR` のみ**。`ONE_SENSOR` は**存在しない** |
| `gas/one/Code.gs:470` `CMD_STATUS_CHILD_IDS` | `'01'`〜`'0F'` の15個。**`'0F'` は登録済み** |
| `gateway_common/Code.gs:473` 同上 | `'01'`〜`'0E'` の14個。**`'0F'` が欠落**（テンプレート側のバグ） |

→ **One標準センサ版のプロファイル（`ONE_SENSOR`）を新規作成する必要がある。**
未登録IDは既定の `FLEX` プロファイルが適用され、列名・単位・スケール・アラート条件がすべてFlex用になる。

また `CMD_STATUS_CHILD_IDS` は表示用ではなく、**`child_XX` シートへの自動振分けの許可リスト**でもある
（`Code.gs:930-958`）。登録漏れのIDは `databox` に落ちる。

---

## 2. 設計方針：3つの識別子を分離する

**第1版の最大の誤りは、DeviceID 1つで全てを担わせようとしたこと。**

| 識別子 | 役割 | 決まり方 | 使う場所 |
|---|---|---|---|
| **`GW_DEVICE_ID`** | GAS上でGateway**個体**を識別 | **XIAO固有ID 16桁** | コマンド予約キー、status、log |
| **`GATEWAY_GROUP_ID`** | 無線受信の**対象群**を決める | ビルド時指定 **0〜7** | 受信フィルタ、ダウンリンク配信 |
| **`DEVICE_ID`** | 子機の識別（群+機器番号） | **上位3bit=群 / 下位5bit=1〜31** | ペイロード、宛先、製品台帳 |

### 2-1. DeviceIDのビット割当

```
DEVICE_ID (1バイト)
 ┌────────┬──────────┐
 │ 上位3bit │  下位5bit  │
 └────────┴──────────┘
     ↑           ↑
     │           └─ 機器番号 1〜31（0は無効値）
     └───────────── Gateway群 0〜7
```

**収容能力：8群 × 31台 = 248 ID。1Gatewayあたり最大31台。**

**★4bit/4bitから変更した理由（2026-08-26決定）**
1Gatewayあたりの想定設置台数が最大20台のため、4bit分割（15台）では不足する。
3bit/5bitなら31台まで対応でき、5割の余裕がある。群は8つあれば当面十分。

**群の境界**

| 群 | DeviceIDの範囲 |
|---|---|
| 0 | `0x01`〜`0x1F` |
| 1 | `0x21`〜`0x3F` |
| 2 | `0x41`〜`0x5F` |
| … | （群N の開始は `N × 0x20 + 1`） |
| 7 | `0xE1`〜`0xFF` |

**既存機器との互換性**：現在稼働中の `0x01`〜`0x0F` はすべて**群0のまま**。変更不要。

```
0x01 = 0b000_00001 → 群0・機器1
0x0F = 0b000_01111 → 群0・機器15
```

**判定式**

```c
group   = deviceId >> 5;          // 上位3bit
localNo = deviceId & 0x1F;        // 下位5bit（1〜31、0は無効）
```

**割り当て例**

| 群 | Gateway | 子機のDeviceID |
|---|---|---|
| 0 | Gateway1 | Flex: `0x01` `0x02` `0x03` ／ One: `0x04` `0x05` `0x06` |
| 1 | Gateway2 | Flex: `0x21` `0x22` `0x23` ／ One: `0x24` `0x25` `0x26` |

> **★要確定**：One 6台それぞれが `ONE_SENSOR` か `ONE_PIR` か。製品台帳の登録に必要。

### 2-1-2. 配列サイズの拡張（★3bit/5bit化に伴う）

| 定数 | 現在 | 変更後 | RAM増分 |
|---|---|---|---|
| `MAX_PENDING_CHILDREN` | 15 | **31** | `PendingDownlink` 約20B × 16 ≈ 0.3KB |
| `MAX_DEVICES` | 20 | **32** | `FlexRecord` 約48B × 12 × 2配列 ≈ 1.1KB |
| （`s_lastSent[]`） | 15 | **31** | `SentDownlink` 約12B × 16 ≈ 0.2KB |

**合計増分 約1.6KB。** 現在のRAM使用率 7.8%（18.6KB / 237KB）→ **約8.5%**。問題なし（実測値）。

> **URL 512バイト制限**：`CLOUD_FMT_V2` で1台28hex文字のため**1リクエスト14台**。
> 31台なら3バッチに分割される。**既存の分割ロジックがそのまま機能するため追加実装は不要**だが、
> 送信回数が増える分だけ通信時間が延びる。

### 2-2. `GW_DEVICE_ID`：XIAO固有IDをそのまま使う

Gatewayファームには取得関数が実装済み（`gateway_v1.20/src/main.cpp:2806-2812`、`NRF_FICR->DEVICEID` 64bit）。
info行の `xiao=<ID>` として送信もしている。

**`gw_<FICR 16桁hex>` を採用する。** 下位8桁への切り詰めはしない（★2版レビュー中1：識別空間を
32bitに縮める合理的理由がない）。

**実装上の注意（★2版レビュー中1）**

- 現在の `static char const* GW_DEVICE_ID` を、**一時 `String` の `c_str()` に向けてはならない**
  （関数終了後にポインタが無効になる）
- **固定長のグローバル `char GW_DEVICE_ID[24]`** を用意し、起動初期化時に `snprintf` で生成する
- **`checkRemoteCmd()` より前に必ず初期化**する
- 生成結果を起動ログとinfo行へ出力する

**運用上の注意（★2版レビュー中2）**

XIAO交換 = Gateway ID変更になる。**論理名（現場名）はファームに焼かず、GAS台帳で
`論理名 ↔ XIAO固有ID` を対応付ける。** MCU交換時は台帳の更新と、旧IDに残った
`pending_cmd_<old-id>` の整理が必要。

---

## 3. 段階リリース計画（★2版レビュー観点7）

**第1段階だけで「2台構成でアップリンクが正しく分離される」という主目的を達成できる。**
最も複雑なダウンリンクAPI設計を後回しにできる。

| 段階 | 内容 | 目的 |
|---|---|---|
| **第1段階** | アップリンクの群分離＋Gateway個体識別 | **2台構成の成立** |
| **第2段階** | 群別ダウンリンク、ACK所有権検証 | 設定変更の分離 |
| **第3段階（将来）** | BLEの群分離、pseudoMacのtransport分離、送信ジッタ | 品質向上 |

### ダウンリンクを後回しにできる根拠

**ダウンリンクは全てテスト段階で、現場実装がない**（2026-08-26確認）。
したがって第1段階では、**群1のGatewayでダウンリンク機能を明示的に無効化**しておけば実害がない。

> **第2版で懸念した移行リスクは、この前提により大幅に緩和される。**
> 第2版では「現行 `gateway_v1.20` が常に `dl=1` を送るため（`main.cpp:1444-1451`）、
> GASに群1 IDを追加すると旧ファームも群1予約を取得してしまう」ことを重大リスクとしていたが、
> **そもそも現場でダウンリンクを使っていないため、テスト環境で完結する。**

---

## 4. 第1段階の改修対象

### 4-1. Gateway（`case02_Gateway/firmware/gateway_v1.20/`）

| # | 箇所 | 変更内容 |
|---|---|---|
| G1 | `src/main.cpp:1076` `GW_DEVICE_ID` | **固定長グローバルバッファ化＋XIAO固有IDから生成**。`checkRemoteCmd()` 前に初期化 |
| G2 | 新規 | `GATEWAY_GROUP_ID` をビルド時指定（既定0）＋`static_assert(GATEWAY_GROUP_ID <= 0x07)` |
| G3 | `src/main.cpp:236` | **`isAllowedLoRaPacket()` を新設**（群判定）。**`isAllowedFlexPacket()` はBLE用に現状維持** ★下記参照 |
| G4 | `src/main.cpp:2283-2300` LoRa受信 | `isAllowedLoRaPacket()` を呼ぶよう変更 |
| G5 | `src/main.cpp:1685-1701` BLE受信 | **変更しない**（第3段階まで現状維持） |
| G6 | `src/main.cpp:1361-1386` `applyDownlinkCache()` | 群検証を追加（数行。安いので第1段階で入れる） |
| G7 | `src/main.cpp:2263-2271` ACK受信 | **群1Gatewayではダウンリンクを無効化**する分岐を追加 |
| G8 | 起動ログ・info行 | `GW_DEVICE_ID`・群番号・XIAO固有IDを出力 |
| G9 | 受信拒否カウンタ | 理由別に分ける（PktType／群不一致／下位5bit=0／長さ／checksum）。**BLEとLoRaも別カウンタ** |
| G10 | `platformio.ini` | `GATEWAY_GROUP_ID` の指定方法を記載 |
| G11 | `README.md:88-93` | `0x01〜0x0F` 固定記述を更新 |
| G12 | `GATEWAY_FW_VERSION` | +1（CLAUDE.md §6） |
| **G13** | **`MAX_PENDING_CHILDREN` 15→31、`MAX_DEVICES` 20→32** | **★3bit/5bit化に伴う配列拡張。RAM増分 約1.6KB** |

**★G3：判定関数を分割する理由**

`isAllowedFlexPacket()` はBLE（`:1685-1701`）とLoRa（`:2283-2300`）の**共通関数**。
群方式へ置換するとBLEも群分離されてしまう。**BLEは後回しと決定した**ため、
**LoRa用の判定関数を新設し、BLE用は現状維持**とする。

```c
// 新設（LoRa用）
bool isAllowedLoRaPacket(uint8_t pktType, uint8_t deviceId) {
  if (pktType != EXPECTED_PKT_TYPE) return false;
  if ((deviceId & 0x1F) == 0) return false;              // 下位5bit 0は無効値
  return (deviceId >> 5) == GATEWAY_GROUP_ID;            // 上位3bitが群一致
}

// 既存（BLE用）はそのまま残す
bool isAllowedFlexPacket(uint8_t pktType, uint8_t deviceId) { ... }
```

**確認済み**：`s_pending[]` `s_lastSent[]` `records[]` はDeviceIDを配列添字に使わず線形探索のため、
値域を 0x01〜0xFF に広げても構造上の破綻はない。

### 4-2. Monita Flex（`case01_Flex/v3.20/`）

| # | 箇所 | 変更内容 |
|---|---|---|
| F1 | `src/main.cpp:102`（BLE） | `#ifndef DEVICE_ID / #define DEVICE_ID 0x01 / #endif` |
| F2 | `src/main.cpp:120`（LoRa） | 同上（既定 `0x0E`） |
| F3 | 新規 | `static_assert` で値域検証（下記） |
| F4 | 起動ログ | DeviceID・群番号・群内番号を表示 |
| F5 | `FW_VERSION` | +1（モードごとの定数。CLAUDE.md §6） |
| F6 | `platformio.ini` | 機体別ビルドの手順を記載 |

**★F3：静的検証の具体条件（★2版レビュー中3）**

```c
static_assert((DEVICE_ID) >= 0x01 && (DEVICE_ID) <= 0xFF,
              "DEVICE_ID must be 0x01..0xFF");
static_assert(((DEVICE_ID) & 0x1F) != 0,
              "DEVICE_ID low 5 bits must be 1..31 (0 is reserved)");
```

マクロに負数や256以上が渡された場合、`uint8_t` へ暗黙切り詰めされる前に検出できる書き方にする。

**用途確認済み**：`msd[1] = DEVICE_ID;`、`Serial.print(DEVICE_ID, HEX)`、`snprintf(bleName, ...)` 等、
いずれもマクロ化して問題ない。

### 4-3. Monita One（`case03_One/v1.00/`）

| # | 箇所 | 変更内容 |
|---|---|---|
| O1 | `src/app_sensor.cpp:21-23` / `app_pir.cpp:19-21` | `static_assert` を追加（上書き機構は既存） |
| O2 | 起動ログ | DeviceIDを表示（現在 `FW`/`CH_ASSIGN`/`sleepMin` のみ） |
| O3 | `platformio.ini:17-21` | 許可範囲の記述を新体系へ |
| O4 | `README.md:74-88` | 同上 |
| — | `src/one_hal.cpp:20` `LORA_CONFIG` | **変更不要**（チャネルを変えない方式のため） |

### 4-4. GAS

| # | 箇所 | 変更内容 |
|---|---|---|
| S1 | `gateway_common/Code.gs:473` | **`'0F'` 欠落を修正**（テンプレート側のバグ）＋群1のIDを追加 |
| S2 | `gateway_common/Code.gs:463-467` `CMD_STATUS_GATEWAY_IDS` | 2台目のGateway固有IDを追加 |
| S3 | `gateway_common/Code.gs:1247-1258` `status_report` | **B列を `p.device_id` にする**（現在は固定 `'GW'`）★重大2 |
| S4 | **`PRODUCT_PROFILES` に `ONE_SENSOR` を新規作成** | **現在 `ONE_PIR` しか存在しない** ★重大3 |
| S5 | `DEVICE_PRODUCT_REGISTRY` | 群0・群1の全実IDを製品種別付きで登録 |
| S6 | `gas/one/Code.gs:470` | 群1のIDを追加（`'0F'` は登録済み） |
| S7 | `gateway_common/README.md:43,73` | 旧範囲記述を更新 |
| S8 | info行の保存 | Gateway ID・群番号の列を追加（`Code.gs:1279-1300`） |

> **★配備先の確認が必要**：テンプレート（`gateway_common`）を修正しても**配備済みGASには反映されない**。
> どのデプロイがどの現場に紐づくかを先に棚卸しすること。
> `gateway_common` は `SPREADSHEET_ID='REPLACE_WITH_SPREADSHEET_ID'` のテンプレートであり、実運用中のコードとは限らない。

### 4-5. テスト

| 対象 | 内容 |
|---|---|
| `case02_Gateway/tests/` | 群0の `0x01`/`0x1F`、群1の `0x21`/`0x3F`、無効値 `0x00`/`0x20`（下位5bit=0）、群不一致、`ONE_SENSOR`/`ONE_PIR` の製品プロファイル、`child_XX` 振分け |

### 4-6. 案件別Gatewayファーム（現状維持）

**`gateway_v1.20` を変更しても自動波及しない。独立したソースコピーである。**

| 案件 | ファイル | 現在の許可範囲 |
|---|---|---|
| 有野川 | `project07_NEXCO/firmware_gateway/src/main.cpp:213-228` | `0x01`〜`0x0D` |
| 横河 | `project06_yokogawa/gateway_v1.1/src/main.cpp:224-240` | `0x01`〜`0x0D` |

**方針：現状維持。** 変更しない限り既存現場の挙動は変わらない。
ただし**群1の子機をこれらのGatewayで受けることはできない**。

> **要確認**：リポジトリ上のコードが現場実機に書かれているかは未確認。
> 実機のFWバージョン・起動ログ・info行との照合が必要。

---

## 5. 第1段階の実装順序

| 順 | 作業 | 影響 |
|---|---|---|
| 1 | **ID割当台帳を作る**（現場・Gateway・群・子機・**製品種別**・設置位置） | なし |
| 2 | GAS：`'0F'` 欠落修正、`ONE_SENSOR` プロファイル新規作成、製品台帳登録 | 要回帰試験 |
| 3 | GAS：`status_report` のB列を `device_id` へ | 要回帰試験 |
| 4 | Gateway：`GW_DEVICE_ID` 一意化＋`isAllowedLoRaPacket()` 新設（既定群0） | 群0＝現行と等価 |
| 5 | Flex：マクロ化＋`static_assert`（既定値維持） | 挙動不変 |
| 6 | One：`static_assert`＋起動ログ | 挙動不変 |
| 7 | 群0で回帰試験 | — |
| 8 | 2台目Gatewayを群1で書込、子機を `0x21`〜 で書込 | 新規機器のみ |
| 9 | 照合（Gatewayログ・GAS保存先・製品プロファイル） | — |

> **★「影響なし」と断定しない**（★2版レビュー中8）。
> `CMD_STATUS_CHILD_IDS` の変更はデータ振分け先を変え、`DEVICE_PRODUCT_REGISTRY` の変更は
> 列名・単位・変換・アラートを変える。**既存入力に対する出力が変わらないことを回帰試験で確認してから**進める。

### 既存子機を群0から群1へ移す場合は「無停止」にならない

- Gatewayを先に群1へ変えると、旧 `0x01`〜`0x0F` の子機を受信しなくなる
- 子機を先に `0x21`〜 へ変えると、群0 Gatewayから受信されなくなる

**新設機器だけを群1にする限り、この問題は発生しない。**

---

## 6. 第2段階（ダウンリンクの群分離）

**第1段階の完了後に着手する。** 現時点では設計のみ記載。

| 項目 | 内容 |
|---|---|
| GAS API | `check_cmd` に `group=<0..15>` と `gw_id=<GW_DEVICE_ID>` を追加 |
| GAS応答 | `group` 指定時は上位nibbleが一致する予約だけ返す。**未指定の旧ファームには群0のみ** |
| 報告系 | `downlink_sent` / `downlink_result` に `gw_id` と `group` を付与 |
| 検証 | GAS側で「報告元の群とchild ID上位nibbleの一致」を検証 |
| ACK | 長さ・下位5bit非0・群一致・`s_lastSent` の存在を検証してから `onDownlinkAckReceived()` |
| 解禁条件 | 全Gatewayの群対応確認後に群1予約を有効化 |

---

## 7. 第3段階（将来の品質向上）

| 項目 | 内容 | 根拠 |
|---|---|---|
| BLEの群分離 | `isAllowedFlexPacket()` も群方式へ | LoRa移行が進んだ段階で判断 |
| `pseudoMac` のtransport分離 | `FlexRecord` に transport 種別を追加し `(transport, identity)` で同一判定 | ★軽微1。BLE実MACとの衝突は現実的には極めて低いが構造上は可能 |
| 送信ジッタ | 子機ごと・周期ごとのランダム遅延、再送間隔のランダム化 | ★中4。同期起床による局所的衝突対策 |
| 通常計測データへのGateway ID付与 | `g=0` 程度の短い識別子 | ★中4。どちらのGatewayが転送したかを監査できない。512バイト制限に注意 |

---

## 8. 運用ルール

### 8-1. ID割当台帳（正本）

現場ID / Gateway論理名 / `GW_DEVICE_ID`(XIAO固有ID) / IMEI / Gateway群 / 子機シリアル /
**製品種別（FLEX / ONE_SENSOR / ONE_PIR）** / 完全DeviceID / 群内番号 / 設置位置 /
FWバージョン / ビルドhash / 書込日時・担当

**手入力コマンドを標準運用にせず、台帳からビルドフラグ・ラベルを生成する。**

### 8-2. 起動ログに識別子を出す

| 機器 | 表示すべき項目 |
|---|---|
| 子機（Flex/One） | 完全DeviceID、群番号、群内番号、製品型、FWバージョン |
| Gateway | `GW_DEVICE_ID`、群番号、FWバージョン、XIAO固有ID |

### 8-3. 焼き分けコマンド

```sh
# Monita One（群1・機器番号4 → 0x24）
PLATFORMIO_BUILD_FLAGS="-D DEVICE_ID=0x24" pio run -e one_sensor_lora -t upload

# Monita Flex（群1・機器番号1 → 0x21）
PLATFORMIO_BUILD_FLAGS="-D DEVICE_ID=0x21" pio run -t upload

# Gateway（群1）
PLATFORMIO_BUILD_FLAGS="-D GATEWAY_GROUP_ID=1" pio run -t upload
```

### 8-4. 群番号の採番規則（要決定）

現場ごとに0から振るか、全社で一意にするか。**少なくとも同一電波到達範囲と同一保守作業単位では
重複させない。** 予備Gateway・持込み試験機の扱いも定義する。

---

## 9. リスクと限界

| リスク | 評価 |
|---|---|
| 電波は物理的に混ざる | 全Gatewayが全パケットを受信しソフトで捨てる。GatewayはAC電源のため実害小 |
| **周期同期による局所的衝突** | 総エアタイムは小さいが、全機が同じRTC基準で起床すると送信が集中する。**第3段階で送信ジッタを導入** |
| ID重複 | 疑似MACも同一になり後着データが上書きする。台帳と機体ラベルで防ぐ |
| 群番号の焼き間違い | 電波は見えるがデータが保存されず、現場では故障と見分けにくい |
| **XIAO交換でGateway IDが変わる** | GAS台帳の更新と旧IDの予約整理が必要 |
| 製品台帳の登録漏れ | データは届くがOneがFlexとして表示される |
| 台数が数十台規模 | エアタイムが逼迫。**チャネル分離（案A）を再検討** |

### 9-1. エアタイムの検算

標準LoRa式（SF7 / BW125kHz / CR4:5 / preamble 8 / explicit header / CRC on）：

```
Tsym      = 2^7 / 125000 = 1.024ms
Tpreamble = (8 + 4.25) × 1.024 = 12.544ms
19バイト: Npayload = 38 symbols → Tpacket = 51.456ms
22バイト（SYNC+LEN+checksum込み）: Npayload = 43 symbols → Tpacket = 56.576ms
```

12台×2回：**約1.24〜1.36秒/時**。総量としては十分小さい。

> **未確定**：E220の実際のcoding rate、preamble長、header方式、CRC有無、独自オーバーヘッドは
> リポジトリ内から確定できない。上記は標準式による推定値であり実機の確定値ではない。

---

## 10. 工数の概算（★2版レビュー観点6）

### 第1段階のみ

| 区分 | ファイル数 | 変更行数の目安 |
|---|---:|---:|
| Gatewayファーム＋platformio＋README | 3 | 80〜160 |
| Flexファーム＋platformio | 2 | 20〜60 |
| Oneファーム2種＋platformio＋README | 4 | 15〜40 |
| GAS共通＋配備案件GAS | 2〜4 | 100〜220 |
| テスト | 2〜5 | 150〜350 |
| **合計** | **13〜18** | **約365〜830** |

実装1〜2日、単体・組合せ試験2〜4日、実機2台＋12子機の総合試験1〜2日が最低線。
GAS配備先の棚卸しや現場実機照合は別枠。

---

## 11. 未確定・要判断

- [ ] **One 6台それぞれが `ONE_SENSOR` か `ONE_PIR` か**（製品台帳登録に必須）
- [ ] `ONE_SENSOR` プロファイルの列構成・単位・スケール・アラート条件
- [ ] 群番号の採番規則（現場ごとに0から／全社で一意）
- [ ] 配備済みGASがどのコピーか（デプロイ台帳との照合）
- [ ] 現場実機のファームがリポジトリ最新と一致しているか
- [ ] ID割当台帳の管理方法（GASコード内配列／スプレッドシート台帳）

---

## 12. 関連

- `【7】Monita/開発/開発メモ/20260826_MonitaOne_v1.00_実機立ち上げ記録.md`
- `共有ドライブ/AVL/バックオフィス/コクリエ管理Claude引き継ぎ/20260821_MonitaOne開発_ピンアサイン確定_セッション引き継ぎ.md` §6-4
- コミット `9264e37`（One の DeviceID をビルド時上書き可能にした）
- Codexレビュー（第1版・第2版に対して各1回実施。本版に反映済み）
