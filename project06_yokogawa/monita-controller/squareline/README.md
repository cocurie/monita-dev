# Monita Controller — SquareLine Studio プロジェクト

コクリエ製コントローラーのUI(タッチ画面)は、ここに入っている`Monita_Controller.spj`をSquareLine Studioで開いて編集します。

## 前提バージョン

- SquareLine Studio **1.6.1** (それ以降の互換バージョンでも動作するはずですが、作成時と揃えるのが安全です)
- LVGL **8.3.11**
- Board設定: **Eclipse with SDL for development on PC**(実機ボード非依存。実機への組み込みは別途 `monita-controller/src/src/ui/src/` に生成コードを手動で統合します)

## 開き方

1. SquareLine Studioを起動し、`Open` からこのフォルダ内の `Monita_Controller.spj` を選択
2. 画像アセット(`assets/Monita_log (1).png`)は同じ相対位置にあるので、開くと自動的に読み込まれます

## ⚠️ 開いたら最初にやること: Export先パスの再設定

`Monita_Controller.slp` に、**このプロジェクトを最初に作った人のPC上の絶対パス**が保存されています(`uiExportFolderPath`)。他の人のPCではそのパスが存在しないため、Export前に必ず以下を確認・変更してください。

- メニューの `Export > Export Options`(または Project Settings)で、出力先フォルダを自分のPC上の任意の場所に変更する
- 例: `~/Downloads/monita_export` など、一時的な書き出し場所でOK

## 画面構成

| 画面名 | 役割 |
|---|---|
| `Initial` | 起動直後のロゴ画面。「Scan start」ボタンでBLEスキャン開始 |
| `DviceList` | スキャン結果一覧(Dropdown) + Connect/Re-scanボタン |
| `Measure` | 接続後の操作画面。Sleep間隔設定(Spinbox+Apply)・Tare・Disconnect・デバッグ表示(BLE状態/直近の送受信内容) |

## Export後の取り込み手順

1. 上記の出力先フォルダにExportする
2. 出力された `screens/`, `components/`, `images/`, `fonts/`, `ui.c`, `ui.h`, `ui_events.c`, `ui_events.h`, `ui_helpers.c`, `ui_helpers.h` を `monita-controller/src/src/ui/src/` に上書きコピーする
3. **`ui.h` の `#include "lvgl/lvgl.h"` を `#include "lvgl.h"` に書き換える**(このプロジェクトのビルド環境の都合上、必須の修正です。詳細はコミット履歴・会話ログ参照)
4. `ui_events.c` は毎回スタブで上書きされるため、`ble_client.h` のinclude追加と各関数の中身(`ble_scan_and_populate()`など、`main.cpp`で実装済みの関数呼び出し)を書き戻す
5. `pio run -e esp32-wrover-e-8m` でビルド確認

## 画像アセットについて

画像を追加・差し替える際は、**実際に表示するピクセルサイズまで縮小してから**アセットに追加してください。SquareLine上の「Scale」プロパティは見た目だけの縮小で、LVGLは非圧縮の生ピクセルデータをそのままFlashに埋め込むため、大きな画像(例: 1414×2000px)をそのまま使うとFlash容量オーバーでビルドが失敗します(実際に一度発生した問題です)。目安: 表示幅100〜150px程度の画像なら数百KB程度に収まります。
