---
title: Monita One向けGateway GAS
domain: monita_dev
tags: [GAS, gateway, monita_one, one_pir, battery]
updated: 2026-08-28
---

# one — Monita One向けGateway GAS

`../gateway_common/Code.gs`を基に、Monita One固有の製品プロファイルを追加した
Google Apps Script。Gatewayは`CLOUD_FMT_V2`を有効にし、1台14バイトのクラウド形式で送る。

DeviceIDは上位3bitをGateway群、下位5bitを機器番号（1〜31）に使う。現在の許可範囲は
群0=`0x01`〜`0x1F`で、群1以降は実機の群番号確定後に追加する。

## デプロイ前の必須設定

1. `Code.gs`の`SPREADSHEET_ID = 'REPLACE_WITH_ONE_SPREADSHEET_ID'`を、One用に新規作成したスプレッドシートIDへ置き換える。
2. メール通知を使う場合は`ADMIN_MAILS`へ通知先を設定する。
3. Gatewayの`platformio.ini`で`CLOUD_FMT_V2`付きのビルドフラグを有効にする。
4. GAS Web Appを再デプロイし、そのデプロイIDをGatewayの`GAS_SCRIPT_ID`へ設定する。

プレースホルダのままでは動作しない。実際のスプレッドシートIDや現場名は
`gateway_common`へ書き戻さず、このフォルダ側だけで管理する。

## One-PIR設定

- DeviceID `0x0F`を`One-PIR`として`DEVICE_PRODUCT_REGISTRY`へ登録。
- `CMD_STATUS_CHILD_IDS`に`'0F'`を登録し、データを`child_0F`へ自動振り分け。
- CH1: 最大人数（人）。`-1`は空欄、`0`は有効値。
- CH2: 平均人数（CH2生値÷10、人）。`-1`は空欄。
- CH3: PIRイベント数（回）。`32767`到達時は`profile_alerts`へ故障疑いを記録し、通知先設定済みならメール送信。
- CH4: スキャン回数（回）。`0`は未計測を示すが、PIRイベント数を残すため行自体は保持。
- 電池電圧: `(3000 + encoded * 5) / 1000` V。`255`は空欄。

CH1が`-1`の行はCH1/CH2を空欄にするため、人数の集計・グラフには入りにくい形で保存される。
CH1が`0`の行は「スキャンしたが0人」の有効な観測として数値0を保存する。

## One標準センサ設定

`ONE_SENSOR`プロファイルを定義しているが、実機種別が未確定のため`DEVICE_PRODUCT_REGISTRY`には
登録していない。CH1はひずみ（με）の実測値、未使用のCH2〜CH4は`-1`を欠測値として扱う。

## 受信形式

GASは移行時の保険として旧26 hexと新28 hexの両方を受理する。`d.length / n`が
26/28以外、割り切れない、または非hexの場合は、`invalid_payload_log`と実行ログへ記録して
そのリクエスト全体を破棄する。
