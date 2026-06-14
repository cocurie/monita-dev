---
title: iPEC サーバー通信検証
domain: case_other
tags: [ipec, lte-m, http-post, sim7080g, json]
updated: 2026-06-14
---

# iPEC サーバー通信検証

## 背景

アイペック社がコクリエの Monita デバイスに関心を持っており、
**iPEC 社のクラウドサーバーへ直接 POST できるか** を検証する。

打ち合わせログ: `【1】個別案件/その他/ipec/20260610_打ち合わせログ.md`

## 検証ステップ

| Step | フォルダ | 内容 | 状態 |
|------|---------|------|------|
| 1 | `01_http_post/` | LTE-M → iPEC HTTPS POST（JSON） | 🔲 未検証 |

## エンドポイント

```
https://22uzcg15xg.execute-api.ap-northeast-1.amazonaws.com/dev
```

- Method: POST
- Content-Type: application/json
- x-api-key: iPEC から取得（`IPEC_API_KEY` に設定）

## JSON ボディ（暫定）

```json
{
  "device_id": "monita-flex-001",
  "device_time": "2026-06-14T10:00:00",
  "channel_1": 1234,
  "channel_2": 5678
}
```

キー名は iPEC 側スキーマに合わせて調整する。

## 事前準備

1. iPEC から API キーを取得
2. `01_http_post/src/main.cpp` の `IPEC_API_KEY` に設定
3. JSON スキーマ（キー名）を iPEC と確認

## ハード構成

- Monita Flex v3.02 基板
- XIAO nRF52840
- SIM7080G (M5STAMP CatM)
- 1NCE IoT SIM (APN: iot.1nce.net)
