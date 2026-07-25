---
title: project07_NEXCO GAS受信スクリプト
domain: monita_dev
tags: [GAS, project07_NEXCO, gateway, v3.03, firmware_versioning]
updated: 2026-07-15
---

# project07_NEXCO — GAS受信スクリプト

Monita Gateway（LTE-M経由）から送られてくる子機データを受信し、
スプレッドシートへの記録とアラートメール送信を行うGoogle Apps Script。

対象スプレッドシート:
https://docs.google.com/spreadsheets/d/1gWDPFg2qxtb61-lSDZO8KEF1y4V74aoDylNjspc0S78/

## このリポジトリのコードとGASエディタの同期について

GAS本体はGoogle側（スタンドアロンではなくスプレッドシート紐付け）で動作しており、
`Code.gs` はその**バックアップ・レビュー用のコピー**。GASエディタで編集した内容は、
このファイルにも手動で反映すること（clasp等の自動同期は未導入）。

## バージョン対応

`Code.gs` 冒頭のコメントに、GASスクリプトのバージョンと対応する
子機ファーム（[case01_Flex/v3.03_sigfox](../../../case01_Flex/v3.03_sigfox)）・
Gatewayファーム（[gateway_v1.1](../../firmware/gateway_v1.1)）のバージョン対応表を記載している。
ペイロード形式を変更した場合は、双方のコミット時にバージョンを+1し、
このREADMEおよびCode.gs冒頭のコメントも更新すること。
