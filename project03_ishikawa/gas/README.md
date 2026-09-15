---
title: project03_ishikawa GAS受信スクリプト（Sigfox Callback）
domain: monita_dev
tags: [GAS, project03_ishikawa, Sigfox, callback]
updated: 2026-09-15
---

# project03_ishikawa — GAS受信スクリプト

Monita Flex（[ishikawa_sigfox_range](../ishikawa_sigfox_range)、Sigfox、12時間に1回送信）のデータを
Sigfox Backend の Callback で受け取り、スプレッドシートへの記録とアラートメール送信を行う Google Apps Script。

対象スプレッドシート:
https://docs.google.com/spreadsheets/d/1oDW2gQEYT1UWmT2zH7mRP8UuSdIHeM6hL3M1zPC2EZ8/

## このリポジトリのコードとGASエディタの同期について

`Code.gs` はGASエディタ上の本体の**バックアップ・レビュー用のコピー**。
GASエディタで編集した内容は、このファイルにも手動で反映すること（clasp等の自動同期は未導入）。

## デプロイID（Webhook URL）はリポジトリに書かない

このリポジトリは公開されている。デプロイURLが分かると、誰でも偽データを書き込んだり
アラートメールを大量に送らせたりできるため、デプロイIDはコミットしない。
Sigfox Backend の Callback 設定画面と、社内の管理表だけに置く。

## Sigfox Backend の Callback 設定

- Type: DATA / UPLINK、Channel: URL、Method: POST、Content type: `application/json`
- URL: `https://script.google.com/macros/s/<デプロイID>/exec`
- Custom payload config（**`int`＝符号付き。`uint` にするとマイナスのひずみ・氷点下が65535付近になる**）:

```
data1::int:16:little-endian data2::int:16:little-endian data3::int:16:little-endian data4::int:16:little-endian data5::int:16:little-endian data6::int:16:little-endian
```

- Body:

```json
{ "device": "{device}", "time": {time},
  "data1": {customData#data1}, "data2": {customData#data2}, "data3": {customData#data3},
  "data4": {customData#data4}, "data5": {customData#data5}, "data6": {customData#data6},
  "lqi": "{lqi}" }
```

（GAS側でも32767超の値は負値へ戻すので、`uint` 設定でも記録値は正しくなるが、Backend画面の表示は崩れる）

## 値の意味（シート・アラート設定時の注意）

| キー | 内容 | 単位 | 注意 |
|------|------|------|------|
| d1〜d4 | CH1〜CH4 ひずみ | µε | `STRAIN_SCALE`（ファーム側）は要校正 |
| d5 | CH1 レンジ（max-min） | µε | 計測1回（約2.5秒）の中のブレ幅。**12時間の変動幅ではない** |
| d6 | 基板温度（DS3231） | ℃×10 | **外気温ではない**。シートでは /10。アラート閾値も10倍で書く |
| d7〜d12 | シートの数式列（J〜O列） | — | 前の行の数式をコピーして計算 |

## シート側で確認すること

- 「シート名編集」シート: 3行目以降の A列=Sigfox デバイスID（16進）、B列=シート名
- デバイスシートの最初のデータ行の1行上（J〜AM列）に数式の雛形があること。
  GASは「直前の行」の数式を新しい行へコピーするため、雛形が無いと d7〜d12 が空になる。
  アラート設定行（3〜14行目）の直下にデータが続く配置だと、14行目（設定行）の J列以降が
  コピーされてしまうので注意。
- 未受信チェック `checkNoDataAlert` はトリガー（1時間ごと）を設定したときだけ動く。
