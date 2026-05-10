# XIAO nRF52840 IMU Comparison

- Internal: LSM6DS3TR-C
- External: LSM6DSO32
- 10-sample averaging
- Moving average filter
- Temperature comparison

# Monita-dev 運用ルール

## 作業動線
- 作業開始時は必ず `git pull` を実行する
- 作業終了時は必ず `git push` を実行する
- 動作確認が取れた単位でこまめに `git commit` する

## コミットメッセージ
- `feat:` 新機能追加
- `fix:` バグ修正
- `test:` 動作確認済みの記録
- `wip:` 作業途中
- `docs:` README等のドキュメント更新

## ライブラリ管理
- ライブラリを追加したら必ず `platformio.ini` の `lib_deps` に記載してcommitする
- 記載なしでpushすると相手のPCでビルドエラーになる

## フォルダ担当
- `case00_common/` は変更前にSlack等で一言声かけする
- `project01〜06/` は担当者が主に管理する

## 禁止事項
- VSCode GUIからのcommitは使わない（ターミナルから操作する）
- `git push --force` は使わない
- Google Drive内にGitリポジトリを置かない
- 動かないコードをpushしない（途中の場合は `wip:` をつける）

## フォルダ構成
- `case00_common/` 共通コード
- `case01_Flex/` Flex基板
- `project01_YCE/` YCE案件
- `project02_hanshin/` 阪神案件
- `project03_yokokawaBHD/` 横河ブリッジHD案件
- `project04_toyono/` 豊能町案件
- `project05_muramoto/` 村本建設案件
- `project06_ishikawa/` 石川先生案件