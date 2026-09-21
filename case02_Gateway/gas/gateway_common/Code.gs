// ╔════════════════════════════════════════════════════════════════════════╗
// ║  ★★★ 最初にここを書き換える（必須） ★★★                              ║
// ║                                                                        ║
// ║  別のスプレッドシート／案件へコピーしたら、必ず下の SPREADSHEET_ID を   ║
// ║  そのスプレッドシートのIDへ書き換えること。                            ║
// ║                                                                        ║
// ║  IDはスプレッドシートのURLの /d/ と /edit の間:                        ║
// ║    https://docs.google.com/spreadsheets/d/【ここ】/edit                 ║
// ║                                                                        ║
// ║  【書き換えを忘れるとどうなるか】                                      ║
// ║  getSpreadsheet() の SpreadsheetApp.openById() が例外を投げる。GASは    ║
// ║  例外時に HTTP 200 で HTMLエラーページ(約5.5KB)を返すため、Gateway側は  ║
// ║  ステータス200だけを見て「✓ GAS 送信成功！」と表示してしまう。         ║
// ║  シリアルログ上は正常に見えるのにシートには1行も入らない、という        ║
// ║  非常に気づきにくい壊れ方をする（2026-08-30に実際に発生）。            ║
// ║  シリアルの len= が数KBなら、まずここを疑うこと。                      ║
// ╚════════════════════════════════════════════════════════════════════════╝
const SPREADSHEET_ID = '14g7rMQIT0tHwrYcripV84in_ME7B9yodOC7SDqFz17w';  // 「リモート操作テスト」

// ╔════════════════════════════════════════════════════════════════════════╗
// ║  ★★★ ダウンリンクを使う群ごとに、Driveファイルを用意して登録する ★★★  ║
// ║                                                                        ║
// ║  【なぜDrive経由なのか】GAS Web Appの応答は Transfer-Encoding: chunked  ║
// ║  で Content-Length が付かない。SIM7080Gの AT+HTTPTOFS はこれを          ║
// ║  ダウンロードできず、本文0バイトになる（2026-09-07に実機で確定。       ║
// ║  ヘッダー長は無関係で、Content-Lengthの有無だけが成否を分けた）。       ║
// ║  そこでGatewayが読む本文だけをDriveのファイルへ逃がす。Driveは          ║
// ║  Content-Length付き・キャッシュ無しで配信され、実機で取得できている。   ║
// ║                                                                        ║
// ║  【用意のしかた】群ごとにテキストファイルを1つDriveに作り、共有を       ║
// ║  「リンクを知る全員／閲覧者」にして、URLの /d/ と /view の間のIDを      ║
// ║  下に登録する。中身は空でよい（GASが上書きする）。                     ║
// ║                                                                        ║
// ║  【★スコープ追加が必須】DriveAppを使うため、appsscript.json の         ║
// ║  oauthScopes に "https://www.googleapis.com/auth/drive" が要る。        ║
// ║  これが無いと承認ダイアログすら出ず、実行時に                           ║
// ║  「You do not have permission to call DriveApp.getFileById」で失敗する。║
// ║  oauthScopes を明示していないプロジェクトでは自動検出が働かないことが    ║
// ║  あるため、下記を appsscript.json に明記すること（2026-09-07に遭遇）:   ║
// ║    "oauthScopes": [                                                    ║
// ║      "https://www.googleapis.com/auth/spreadsheets",                   ║
// ║      "https://www.googleapis.com/auth/script.container.ui",            ║
// ║      "https://www.googleapis.com/auth/script.send_mail",               ║
// ║      "https://www.googleapis.com/auth/drive"                           ║
// ║    ]                                                                   ║
// ║  書き換え後は testWriteDownlinkFile() を手動実行して承認を通すこと。    ║
// ║                                                                        ║
// ║  【★デプロイ更新も必須】エディタで保存しただけではGatewayに反映され    ║
// ║  ない。「デプロイを管理」から既存デプロイのバージョンを更新すること     ║
// ║  （「新しいデプロイ」はURLが変わるので不可）。                          ║
// ║                                                                        ║
// ║  【登録し忘れるとどうなるか】その群のGatewayはダウンリンク予約を        ║
// ║  受け取れない。テレメトリ送信は影響を受けず動き続けるので、            ║
// ║  「データは来るのに設定変更だけ効かない」という形で現れる。            ║
// ║  実行ログに [DOWNLINK] file id 未登録 が出ていればこれ。               ║
// ╚════════════════════════════════════════════════════════════════════════╝
const DOWNLINK_DRIVE_FILE_IDS = {
  0: '1bulwGZOSK5iBiw1pv88JEOHuryeomme0',  // 群0
  // 1: 'ここに群1用のファイルID',
};

// ★エディタから手動実行して、Drive書き込みだけを単独で確認するための関数。
//   Gatewayを動かさずに「GASがDriveへ書けるか」だけを切り分けられる。
//   初回実行時はDriveへのアクセス承認ダイアログが出るので許可すること
//   （DriveAppを使い始めたぶんの権限追加。承認しないと書き込みは失敗し続ける）。
function testWriteDownlinkFile() {
  var group = 0;
  var nonce = 'TEST' + Math.floor(Math.random() * 10000);
  var ok = writeDownlinkFile_(group, nonce, 'none');
  if (!ok) {
    console.log('✗ 書き込み失敗。上に出ている [DOWNLINK] のログを見ること');
    return;
  }
  var content = DriveApp.getFileById(DOWNLINK_DRIVE_FILE_IDS[group]).getBlob().getDataAsString();
  console.log('✓ 書き込み成功。ファイルの現在の内容:');
  console.log(content);
  console.log('先頭行が ' + nonce + ' になっていればGateway側のnonce検証を通る');
}

// Gatewayが読む本文をDriveのファイルへ書く（方式A・2段目の取得元）。
// 先頭行はGatewayが送ってきたワンタイム値(nonce)。Gatewayはこれが自分の送った値と
// 一致しなければ内容を捨てるので、「GASが失敗して古いファイルが残っていたのに
// 新鮮だと思い込む」事故と、複数Gatewayが同じファイルを取り合う事故を検出できる。
function writeDownlinkFile_(group, nonce, body) {
  var fileId = DOWNLINK_DRIVE_FILE_IDS[group];
  if (!fileId) {
    console.log('[DOWNLINK] file id 未登録: group=' + group + ' → この群はダウンリンク不可');
    return false;
  }
  try {
    DriveApp.getFileById(fileId).setContent(nonce + '\n' + body);
    return true;
  } catch (e) {
    console.log('[DOWNLINK] Drive書き込み失敗 group=' + group + ' id=' + fileId + ' : ' + e);
    return false;
  }
}

// ================================
// Monita Gateway (LTE-M/LoRa) → GAS 汎用バックエンド
//
// ★このファイルの位置づけ: 各案件へコピーする「元ファイル（テンプレート）」。
//   特定の現場・本番監視に紐づくものではない。案件ごとに本ファイルをコピーして
//   それぞれ独立したApps Scriptプロジェクト／スプレッドシートを立てて運用する。
//   各案件で加えた個別の変更は、その案件のフォルダ側で管理すること。
//
// ★2026-08-30: SPREADSHEET_IDだけは例外として、この共通ファイルにも実IDを置く方針へ
//   変更した（検証用「リモート操作テスト」のID）。プレースホルダのままコピー＆デプロイ
//   して丸一日ハマったため。書き換え忘れが即座に分かるよう、定義はファイル最上部の
//   警告ブロック内に置いてある。他案件へコピーする際は必ずそこを書き換えること。
//
// ================================
// バージョン対応表
// ================================
//   GASスクリプトバージョン: 10
//     - v1〜3: 旧フォーマット（case02_Gateway/firmware/gateway_v1.1 対応）
//     - v4: project07_NEXCO 専用フォーマットに対応（2026-07-25）
//       ペイロード: 26B/台 = 52 hex文字
//       DeviceID(1B) + Temp(1B) + CH1-4メジアン(8B) + CH1-4 Max/Min(16B, チャンネルごとに隣接)
//     - v5: LoRaダウンリンク（Class A + 確認応答方式）のリモート制御機能を追加（2026-08-09〜10）
//       check_downlinks / downlink_sent / downlink_result アクション、cmd_status・downlink_logシート、
//       Gateway操作/Flex操作メニュー
//     - v6: check_cmd に dl=1 パラメータを追加（2026-08-10）。本番Gateway(v1.20)への統合用。
//       dl=1 のときだけ応答を複数行化し、2行目以降にダウンリンク予約を返す。
//       dl未指定なら従来とまったく同じ1行応答（gateway_v1.1はこちら）。
//       check_downlinks は検証用テストスケッチが使い続けるため残してある。
//     - v7: 子機データ用シート(child_XX)を自動作成するようにした（2026-08-10）。
//       従来はシートが無いとデータを黙って捨てていた（送信側は成功に見えるため気づけない）
//     - v8: ダウンリンク予約の read-modify-write（queueDownlink_ / downlink_result /
//       downlink_sent）にLockServiceを適用（2026-08-11）。並行リクエストでの更新消失を防止
//     - v9: Gatewayの旧13B/新14Bレコードを自動判別し、電池電圧・Gateway epoch・
//       DEVICE_ID台帳による製品別の列名/単位/スケール/アラート定義へ対応（2026-08-16）
//     - v10: check_cmdの群別ダウンリンク配信と、送信・結果報告のACK所有権検証に対応（2026-08-28）
//     - v11: MONITA Deck 子機に対応（2026-09-12）。Gateway FW99 と組で使う。
//            ・受信: &pt=06（静的6CH変位・48hex）/ &pt=07（イベント統計・50hex）を
//              deck_<HEX> / deck_<HEX>_event シートへ振り分ける
//            ・送信: トリガ設定13B を予約行の8番目のフィールド（16進26文字）で運ぶ
//            ・deck_trigger シートと［Deck操作］メニューを追加
//            ★&pt= が無いリクエストは従来と1行も変わらない挙動になる（既存Flex現場は無影響）
//     - v12: Codexレビュー対応（2026-09-13）。
//            ・受信データの書込み失敗（ロック待ち・シート例外）を黙って 'OK' にしていたのをやめ、
//              Script Properties に退避して再処理する（receiveTelemetry_）。★Gateway は応答本文を
//              読まないので、GAS 側で持たないと消える。設置時に［【初回のみ】受信データ再処理の
//              自動実行を設定］を1回実行すること
//            ・DECK_CHILD_IDS 未登録の Deck はシートを自動作成せず invalid_payload_log へ
//            ・群1以降の Deck にも予約を配信（downlinkChildIds_）
//            ・確定済み・取り消し済みを downlink_sent で sent に戻さない
//            ・予約の取り消しをロック付き・state='cancelled' に（削除すると seq が振り直される）
//            ・deck_trigger の状態列を seq で行特定。子機の適用値を子機と同じ規則で検査
//     - v13: 週次レビュー対応（2026-09-22）。Gateway FW100 と組で使うと効果が出る（片方だけでも無害）。
//            ・downlink_result / downlink_sent のロック待ちタイムアウトで例外を投げ、Google の
//              エラーページを返す（waitLockOrThrowRetryable_）。FW100 はこれを失敗として再送する。
//              以前は HTTP 200 の 'error: lock timeout' を Gateway が成功と区別できず報告を捨てていた（A14）
//            ・Flex 平均/メジアン変更メニューのメジアン上限を 255→20（子機の MEASURE_COUNT_MAX）に（B15）
//
//   対応する子機ファーム:
//     - project07_NEXCO/firmware/src/main.cpp（COMM_MODE_BLE、本番項目用）
//       SAMPLES_PER_AVG=5, MEASURE_COUNT=10
//     - case01_Flex/test_sketches/25_lora_downlink_child（LoRaダウンリンク検証用）
//
//   対応するGatewayファーム:
//     - project07_NEXCO/firmware_gateway/src/main.cpp（本番項目用）
//     - case02_Gateway/test_sketches/03_lora_downlink_sender（LoRaダウンリンク検証用）
//
// ================================
// スプレッドシート列構成（databox シート）
// ================================
//   A: 計測日時（Gateway epoch）
//   B: DeviceID
//   C: 温度(℃)
//   D-G: 製品プロファイルで定義したCH1〜4（列名・単位・スケールを切替）
//   H: CH1 Max(με)
//   I: CH1 Min(με)
//   J: CH2 Max(με)
//   K: CH2 Min(με)
//   L: CH3 Max(με)
//   M: CH3 Min(με)
//   N: CH4 Max(με)
//   O: CH4 Min(με)
//   P: LTE-M RSSI(CSQ)
//   Q: 電池電圧(V)（旧13B形式または値255の場合は空欄）
// ================================


// ================================
// クールダウン手動リセット
// ================================
function resetAlertCooldown() {
  var props = PropertiesService.getScriptProperties();
  props.deleteAllProperties();
  console.log("クールダウン情報を全削除しました");
}


// ================================
// Gatewayリモートリセット（★2026-08-04追加、スプレッドシートのボタン用）
// ================================
// doGet()のaction=set_cmdと同じ処理を、HTTP経由ではなく直接呼び出す版。
// スプレッドシート上に図形（ボタン）を挿入し、この関数を割り当てて使う
// （挿入方法: 挿入 → 図形描画 でボタンを作成 → 右上の「⋮」→ スクリプトを割り当て →
//  "triggerGatewayReset" と入力）。
// 複数Gatewayを扱うようになったら、deviceIdをプロンプトで選ばせる形に拡張すること。
function triggerGatewayReset() {
  var ui = SpreadsheetApp.getUi();
  var response = ui.alert(
    'Gatewayをリセットしますか？',
    '次の送信サイクル（最大5分後）でGatewayが再起動します。',
    ui.ButtonSet.YES_NO
  );
  if (response !== ui.Button.YES) return;

  var deviceId = 'gateway_v11_test';
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + deviceId, 'reset');
  refreshCmdStatusSheet();
  ui.alert('予約しました。Gatewayが次にオンラインになったタイミング（最大5分後）で再起動します。');
}


// ================================
// Gateway送信制御（★2026-08-04追加、スプレッドシートのボタン用）
// ================================
// stop: BLE/LoRa受信・コマンド確認は継続したまま、GASへのデータ送信のみ一時停止する。
// start: 停止していた送信を再開する。
// send_now: 一時停止中でも、今持っているデータを次のサイクルで強制送信する。
function triggerGatewayStop() {
  var deviceId = 'gateway_v11_test';
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + deviceId, 'stop');
  refreshCmdStatusSheet();
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）でデータ送信を停止します。');
}

function triggerGatewayStart() {
  var deviceId = 'gateway_v11_test';
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + deviceId, 'start');
  refreshCmdStatusSheet();
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）でデータ送信を再開します。');
}

function triggerGatewaySendNow() {
  var deviceId = 'gateway_v11_test';
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + deviceId, 'send_now');
  refreshCmdStatusSheet();
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）で今持っているデータを送信します。');
}

// 送信間隔を変更する（1〜1440分の範囲。プロンプトで分数を入力させる）
function triggerGatewaySetInterval() {
  var ui = SpreadsheetApp.getUi();
  var response = ui.prompt(
    '送信間隔の変更',
    '新しい送信間隔を分単位で入力してください（1〜1440分）:',
    ui.ButtonSet.OK_CANCEL
  );
  if (response.getSelectedButton() !== ui.Button.OK) return;

  var minutes = parseInt(response.getResponseText(), 10);
  if (isNaN(minutes) || minutes < 1 || minutes > 1440) {
    ui.alert('1〜1440の整数を入力してください。');
    return;
  }

  var deviceId = 'gateway_v11_test';
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + deviceId, 'interval:' + minutes);
  refreshCmdStatusSheet();
  ui.alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）で送信間隔を' + minutes + '分に変更します。');
}

// ステータス確認（電波強度・稼働時間・空きヒープをdataboxシートへ次サイクルで報告させる）
function triggerGatewayStatusNow() {
  var deviceId = 'gateway_v11_test';
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + deviceId, 'status_now');
  refreshCmdStatusSheet();
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）でステータスをdataboxシートへ報告します。');
}

// RTC再同期（網時刻(AT+CCLK)での強制補正を今すぐ実行させる）
function triggerGatewayRtcResync() {
  var deviceId = 'gateway_v11_test';
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + deviceId, 'rtc_resync');
  refreshCmdStatusSheet();
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）でRTCを網時刻に再同期します。');
}

// 診断ログ吸い上げ（gwlog.csv末尾を次サイクルでgwlogシートへ送らせる）
function triggerGatewayLogDump() {
  var deviceId = 'gateway_v11_test';
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + deviceId, 'log_dump');
  refreshCmdStatusSheet();
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）で直近のログを"gwlog"シートへ送信します。');
}


// ================================
// LoRaダウンリンク（★2026-08-08追加 / 08-09に確認応答方式へ改訂）
// ================================
// 対象: case02_Gateway/test_sketches/03_lora_downlink_sender（GW_DEVICE_ID='lora_downlink_test'）が
// 予約をポーリングし、case01_Flex/test_sketches/25_lora_downlink_child宛てにLoRaダウンリンクで
// 送信頻度(sleepMinutes)・平均/メジアン回数を変更する。本番のgateway_v11_test/v3.10_loraとは
// 別デバイスIDなので、通常運用のGatewayには一切影響しない。
//
// 【方式: Class A + 確認応答（2026-08-09確定）】
//   子機は省電力のため常時受信できない。そこで「子機が自分のアップリンクを送った直後に
//   2秒だけ受信窓を開け、Gatewayがその場で即座に応答する」Class A方式を採る。
//   Gatewayは送信しただけでは予約を消さず、子機からの確認フレーム(PktType 0x05)を
//   受け取って初めて完了扱いにする。途中で取りこぼしても失われず、子機の次サイクルで
//   自動的に再試行される（fire-and-forgetをやめ、再試行前提の意味論にした）。
//
// 【予約の保持形式】Script Properties のキー `downlink_child_<HEX2>` にJSONで保持する。
//   { sleep, avg, median, state, attempts, status, appliedSleep, appliedAvg, appliedMedian, updated }
//   state: queued（予約直後）→ sent（Gatewayが送信し確認待ち）→ done / failed
// ★複数子機に同時に別々の予約を積める（子機IDごとに独立したキーのため）。

// ダウンリンク結果のステータスコード（子機ファーム・Gatewayファームと一致させること）
const DL_STATUS_OK          = 0;   // 要求通り適用
const DL_STATUS_RANGE_ERROR = 1;   // 値域エラー（子機が拒否）
const DL_STATUS_CLAMPED     = 2;   // クランプ適用（WDT制約等で要求と異なる値を適用）
const DL_STATUS_NO_ACK      = 99;  // 未達（Gatewayが規定回数送っても確認が返らなかった。Gateway自身が報告）

function dlKey_(childHex) { return 'downlink_child_' + childHex; }

function dlGet_(childHex) {
  var raw = PropertiesService.getScriptProperties().getProperty(dlKey_(childHex));
  if (!raw) return null;
  try { return JSON.parse(raw); } catch (e) { return null; }
}

function dlSet_(childHex, obj) {
  obj.updated = new Date().toISOString();
  PropertiesService.getScriptProperties().setProperty(dlKey_(childHex), JSON.stringify(obj));
}

// 未完了（queued / sent）の予約を、Gatewayが解釈する1行1件の形式で組み立てる。
// 形式（Flex）: HEX2:sleepMin:avg:median:attempts:seq:mode          … 7フィールド
// 形式（Deck）: HEX2:0:0:0:attempts:seq:0:TRIG26                    … 8フィールド
//   ★8番目（トリガ設定の16進26文字）の有無だけで Gateway が Flex / Deck を振り分ける。
//     Deck では sleep/avg/median を使わないので 0 を入れる（Gateway 側もこの3欄の
//     値域検査を飛ばす）。この形にしたのは、**予約キャッシュ・再送・報告の仕組みを
//     Flex と共有するため**である。Deck 専用の経路をもう1本作ると、再送や seq の
//     取り違えといった実機で潰した不具合を2度踏むことになる。
//   ★attemptsもseqもGAS側を正とする。Gateway側で数えると、再取得のたびにリセットされたり、
//     予約を入れ直しても古い試行回数を引き継いだりするため。
//   ★mode: 0=通常の設定変更 / 1=ステータス確認のみ（設定変更フラグを立てずに送る）
// ★GatewayはAT+HTTPTOFS方式でこの応答を取得する（HEADERLEN等の制限を受けない）ため、
//   件数上限はサイズ制約ではなく運用上の目安として1回にDOWNLINK_MAX_LINES件までとする。
//   14台×約20バイト＝約280バイトで、check_cmdのコマンド行と合わせても十分小さい。
// check_downlinks（テストスケッチ用）と check_cmd（本番Gateway用、2行目以降に相乗り）の
// 両方から呼ばれる。表現を1か所に集約し、片方だけ形式が変わる事故を防ぐ。
var DOWNLINK_MAX_LINES = 20;

// 予約を持ちうる子機IDの一覧（Flex の CMD_STATUS_CHILD_IDS ＋ Deck の DECK_CHILD_IDS）。
// ★以前は CMD_STATUS_CHILD_IDS だけを回していたため、そこに無い Deck（群1以降など）は
//   予約を積んでも**一度も Gateway へ配信されなかった**（Codexレビュー指摘）。
//   配信・状況表示の両方がここを通るようにして、片方だけ漏れる事故を防ぐ。
function downlinkChildIds_() {
  var ids = CMD_STATUS_CHILD_IDS.slice();
  DECK_CHILD_IDS.forEach(function (h) { if (ids.indexOf(h) < 0) ids.push(h); });
  return ids;
}

function buildDownlinkLines_(group) {
  var lines = [];
  var childIds = downlinkChildIds_();
  for (var ci = 0; ci < childIds.length && lines.length < DOWNLINK_MAX_LINES; ci++) {
    var hex = childIds[ci];
    if (group !== undefined && (parseInt(hex, 16) >> 5) !== group) continue;
    var d = dlGet_(hex);
    if (d && (d.state === 'queued' || d.state === 'sent')) {
      if (d.kind === 'deck') {
        lines.push(hex + ':0:0:0:' + (d.attempts || 0) + ':' + (d.seq || 0) + ':0:' + d.trigHex);
      } else {
        lines.push(hex + ':' + d.sleep + ':' + d.avg + ':' + d.median +
                   ':' + (d.attempts || 0) + ':' + (d.seq || 0) +
                   ':' + (d.mode === 'status' ? 1 : 0));
      }
    }
  }
  return lines;
}

// ステータスコードを日本語の結果表示に変換する
// ★2026-08-10追加: WDTタイムアウト表示。子機のWDT計算式（送信間隔+マージン）が
// 正しく機能しているかをスプレッドシート上で確認できるようにするための補足表示。
function dlWdtSuffix_(d) {
  return d.appliedWdtMin ? ('　WDT=' + d.appliedWdtMin + '分') : '';
}

function dlStatusLabel_(d) {
  if (!d) return '';
  if (d.state === 'queued') return '';
  if (d.state === 'sent')   return '送信済み（確認待ち）';
  if (d.state === 'cancelled') return '取り消し済み';
  // ★Deck は sleep/avg/median を持たない。適用後のトリガ設定で表示する。
  if (d.kind === 'deck') {
    // appliedCheck は「完了と応答したが中身がおかしい」ときだけ入る（deckAppliedCheck_）
    var warn = d.appliedCheck ? ('　' + d.appliedCheck) : '';
    if (d.status === DL_STATUS_OK)          return '完了: ' + deckTrigHexToText_(d.appliedTrigHex || '') + warn;
    if (d.status === DL_STATUS_CLAMPED)     return '完了（子機が値を丸めた）: ' + deckTrigHexToText_(d.appliedTrigHex || '') + warn;
    if (d.status === DL_STATUS_RANGE_ERROR) return '失敗（子機が値域エラーで拒否）';
    if (d.status === DL_STATUS_NO_ACK)      return '失敗（未達。' + (d.attempts || 0) + '回試行しても確認が返らず）';
    return '失敗（不明なステータス: ' + d.status + '）';
  }
  if (d.status === DL_STATUS_OK)          return '完了' + dlWdtSuffix_(d);
  if (d.status === DL_STATUS_CLAMPED)     return '完了（値を丸めた: 間隔=' + d.appliedSleep + '分, 平均=' + d.appliedAvg + ', メジアン=' + d.appliedMedian + '）' + dlWdtSuffix_(d);
  if (d.status === DL_STATUS_RANGE_ERROR) return '失敗（子機が値域エラーで拒否）';
  if (d.status === DL_STATUS_NO_ACK)      return '失敗（未達。' + (d.attempts || 0) + '回試行しても確認が返らず）';
  return '失敗（不明なステータス: ' + d.status + '）';
}

// ★2026-08-10追加: 子機DeviceID(16進2桁)をプロンプトで入力させる共通処理。
// 個別メニュー項目（送信間隔だけ変更、平均/メジアンだけ変更 等）から共通して使う。
function promptChildHex_(ui, title, message) {
  var r = ui.prompt(title, message, ui.ButtonSet.OK_CANCEL);
  if (r.getSelectedButton() !== ui.Button.OK) return null;
  var hex = r.getResponseText().trim().replace(/^0x/i, '');
  if (!/^[0-9a-fA-F]{1,2}$/.test(hex)) {
    ui.alert('DeviceIDは16進2桁で入力してください（例: 08）。');
    return null;
  }
  return ('0' + hex).slice(-2).toUpperCase();
}

// ★2026-08-10追加: 送信間隔・平均・メジアンを個別に変更できるようにするため、
// 「今その子機に指定されているはずの値」を推定する。ダウンリンクは3項目セットで
// 送る仕様（DL_FLAG_SLEEP_MIN・DL_FLAG_AVG_MEDIANを毎回両方立てる）なので、
// 片方だけ変更したい時も残りの項目には何かしら値を入れる必要がある。
// 優先順位: 直近に子機が確認した実際の適用値 → 直近の予約値（未確認でも） → 既定値。
function getKnownChildSettings_(childHex) {
  var d = dlGet_(childHex);
  if (d && (d.state === 'done') && d.appliedSleep) {
    return { sleep: d.appliedSleep, avg: d.appliedAvg, median: d.appliedMedian };
  }
  if (d && d.sleep) {
    return { sleep: d.sleep, avg: d.avg, median: d.median };
  }
  return { sleep: 60, avg: 5, median: 5};  // 子機ファームのDEFAULT_*と揃えた保険値
}

// ★予約ごとに通し番号(seq)を振って予約する共通処理。
// Gatewayからの報告にはこのseqを含めてもらい、一致しない報告は無視する。
// これが無いと、古い予約の「未達」報告が、その後に入れ直した新しい予約を
// 失敗扱いで上書きして消してしまう（実機で発生）。
//
// ★2026-08-10追加: mode='status'（ステータス確認）に対応。
// 設定変更をしない代わりに、Gatewayは子機へ「変更フラグを一切立てないダウンリンク
// (flags=0)」を送る。子機はapplyDownlinkPayload()の仕様上、変更が無くても確認応答
// （現在の送信間隔・平均・メジアン・WDTタイムアウト）を必ず返すため、この応答だけを
// 目的に使う。sleep/avg/medianは送信フレームには使われないダミー値でよい。
// Flex 子機が受け付けるメジアン回数の上限（case01_Flex/v3.20/src/main.cpp の MEASURE_COUNT_MAX）。
const FLEX_MEDIAN_MAX = 20;

function queueDownlink_(childHex, sleepMin, avg, median, sourceNote, mode) {
  // ★同時予約でseqの採番・予約更新が競合しないようにする。
  var lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);

    var prevRec = dlGet_(childHex);
    var nextSeq = (prevRec && prevRec.seq ? prevRec.seq : 0) + 1;

    dlSet_(childHex, {
      sleep: sleepMin, avg: avg, median: median,
      state: 'queued', attempts: 0, seq: nextSeq,
      mode: mode || 'set',
    });

    dlLog_(childHex, nextSeq, (mode === 'status') ? 'ステータス確認要求' : '予約',
           (mode === 'status') ? '（設定変更なし）' : ('間隔=' + sleepMin + '分 / 平均=' + avg + ' / メジアン=' + median),
           (prevRec && prevRec.state !== 'done' && prevRec.state !== 'failed' && prevRec.state !== 'cancelled')
             ? '★未完了の予約(seq=' + prevRec.seq + ')を上書きしました'
             : sourceNote);
  } catch (err) {
    console.log('Downlink reservation lock error: ' + err);
    return ContentService.createTextOutput('error: lock timeout');
  } finally {
    try { lock.releaseLock(); } catch (e2) {}
  }

  refreshCmdStatusSheet();
}

// 子機の現在の設定（送信間隔・平均・メジアン・WDTタイムアウト）を確認する。
// 設定は一切変更しない。子機は次にアップリンクを送ったタイミングで応答するため、
// 結果は"cmd_status"シート・"downlink_log"シートに反映される。
function triggerFlexStatusCheck() {
  var ui = SpreadsheetApp.getUi();
  var deviceIdHex = promptChildHex_(ui, 'Flex ステータス確認', '対象の子機DeviceIDを16進2桁で入力してください（例: 08）:');
  if (!deviceIdHex) return;

  queueDownlink_(deviceIdHex, 0, 0, 0, 'スプレッドシートのメニュー（ステータス確認）から予約', 'status');
  ui.alert('子機0x' + deviceIdHex + 'のステータス確認を予約しました。\n\n' +
           '子機は省電力のため常時受信していません。次にこの子機がアップリンクを送った' +
           'タイミング（最大で子機の送信間隔ぶん）で応答し、現在の送信間隔・平均/メジアン回数・' +
           'WDTタイムアウトが"cmd_status"シートに反映されます（設定は変更しません）。');
}

// 送信間隔だけを変更する（平均・メジアンは直近の既知値を引き継ぐ）
function triggerFlexSetInterval() {
  var ui = SpreadsheetApp.getUi();
  var deviceIdHex = promptChildHex_(ui, 'Flex 送信間隔の変更 (1/2)', '対象の子機DeviceIDを16進2桁で入力してください（例: 08）:');
  if (!deviceIdHex) return;

  var known = getKnownChildSettings_(deviceIdHex);
  var r2 = ui.prompt('Flex 送信間隔の変更 (2/2)',
    '新しい送信間隔を分単位で入力してください（1〜1440分）:\n' +
    '（平均=' + known.avg + ', メジアン=' + known.median + ' は変更せず維持します）',
    ui.ButtonSet.OK_CANCEL);
  if (r2.getSelectedButton() !== ui.Button.OK) return;
  var sleepMin = parseInt(r2.getResponseText(), 10);
  if (isNaN(sleepMin) || sleepMin < 1 || sleepMin > 1440) {
    ui.alert('送信間隔は1〜1440の整数で入力してください。');
    return;
  }

  queueDownlink_(deviceIdHex, sleepMin, known.avg, known.median,
                 'スプレッドシートのメニュー（送信間隔のみ変更）から予約');
  ui.alert('子機0x' + deviceIdHex + 'の送信間隔を' + sleepMin + '分に変更するよう予約しました。');
}

// 平均回数・メジアン回数だけを変更する（送信間隔は直近の既知値を引き継ぐ）
function triggerFlexSetAvgMedian() {
  var ui = SpreadsheetApp.getUi();
  var deviceIdHex = promptChildHex_(ui, 'Flex 平均/メジアン回数の変更 (1/2)', '対象の子機DeviceIDを16進2桁で入力してください（例: 08）:');
  if (!deviceIdHex) return;

  var known = getKnownChildSettings_(deviceIdHex);
  var r2 = ui.prompt('Flex 平均/メジアン回数の変更 (2/2)',
    '平均回数,メジアン回数をカンマ区切りで入力してください（例: 8,8）:\n' +
    '（送信間隔=' + known.sleep + '分 は変更せず維持します）',
    ui.ButtonSet.OK_CANCEL);
  if (r2.getSelectedButton() !== ui.Button.OK) return;
  var parts = r2.getResponseText().split(',');
  var avg = parseInt(parts[0], 10);
  var median = parseInt(parts[1], 10);
  // ★2026-09-22（週次レビュー B15）: メジアン回数の上限を子機に合わせる。
  //   以前は1〜255を受け付けていたが、Flex は MEASURE_COUNT_MAX（=20、配列サイズ）を
  //   超える値を値域エラーで拒否するため、21以上の予約は必ず「失敗」になっていた。
  //   子機側の上限を変えたらここも合わせること（case01_Flex/v3.20/src/main.cpp MEASURE_COUNT_MAX）。
  if (isNaN(avg) || isNaN(median) || avg < 1 || avg > 255 || median < 1 || median > FLEX_MEDIAN_MAX) {
    ui.alert('平均回数は1〜255、メジアン回数は1〜' + FLEX_MEDIAN_MAX + 'の整数で入力してください（例: 8,8）。');
    return;
  }

  queueDownlink_(deviceIdHex, known.sleep, avg, median,
                 'スプレッドシートのメニュー（平均/メジアンのみ変更）から予約');
  ui.alert('子機0x' + deviceIdHex + 'の平均回数=' + avg + ', メジアン回数=' + median + 'に変更するよう予約しました。');
}

// ================================
// 予約を手動で取り消す（★2026-08-10追加、スプレッドシートのボタン用）
// Gateway側のpending_cmd_<id>を取り消す。実機がオフラインで長時間放置される、
// 値を間違えて予約した等のケースで、次に実機がオンラインになるのを待たずに
// その場で取り消せるようにする。
function triggerCancelGatewayReservation() {
  var ui = SpreadsheetApp.getUi();

  var choices = CMD_STATUS_GATEWAY_IDS.map(function (dev) { return dev.label; });
  var r1 = ui.prompt(
    'Gateway予約の取り消し (1/2)',
    '取り消す対象の番号を入力してください:\n' +
      choices.map(function (c, i) { return (i + 1) + '. ' + c; }).join('\n'),
    ui.ButtonSet.OK_CANCEL
  );
  if (r1.getSelectedButton() !== ui.Button.OK) return;
  var idx = parseInt(r1.getResponseText(), 10) - 1;
  if (isNaN(idx) || idx < 0 || idx >= choices.length) {
    ui.alert('番号が不正です。');
    return;
  }

  var devId = CMD_STATUS_GATEWAY_IDS[idx].id;
  var label = choices[idx];
  var current = PropertiesService.getScriptProperties().getProperty('pending_cmd_' + devId) || 'none';
  if (current === 'none') {
    ui.alert(label + ' には現在予約がありません。');
    return;
  }

  var r2 = ui.alert(
    'Gateway予約の取り消し (2/2)',
    label + ' の以下の予約を取り消しますか？\n\n' + current,
    ui.ButtonSet.YES_NO
  );
  if (r2 !== ui.Button.YES) return;

  PropertiesService.getScriptProperties().deleteProperty('pending_cmd_' + devId);
  refreshCmdStatusSheet();
  ui.alert(label + ' の予約を取り消しました。');
}

// Flex子機宛てのLoRaダウンリンク予約(downlink_child_<HEX>)を取り消す。
function triggerCancelFlexReservation() {
  var ui = SpreadsheetApp.getUi();

  var deviceIdHex = promptChildHex_(ui, 'Flex予約の取り消し (1/2)', '対象の子機DeviceIDを16進2桁で入力してください（例: 08）:');
  if (!deviceIdHex) return;

  var d = dlGet_(deviceIdHex);
  if (!d || (d.state !== 'queued' && d.state !== 'sent')) {
    ui.alert('子機0x' + deviceIdHex + ' には未完了の予約がありません。');
    return;
  }
  var current = '間隔=' + d.sleep + '分, 平均=' + d.avg + ', メジアン=' + d.median +
                '（状態: ' + (d.state || '?') + '）';

  var r2 = ui.alert(
    'Flex予約の取り消し (2/2)',
    '子機0x' + deviceIdHex + ' の以下の予約を取り消しますか？\n\n' + current,
    ui.ButtonSet.YES_NO
  );
  if (r2 !== ui.Button.YES) return;

  var res = cancelDownlinkLocked_(deviceIdHex, d.seq);
  if (res !== 'ok') { ui.alert(res); return; }

  dlLog_(deviceIdHex, d.seq, '取消', current, 'スプレッドシートのメニューから手動取消');
  refreshCmdStatusSheet();
  ui.alert('子機0x' + deviceIdHex + ' の予約を取り消しました。');
}

// 予約の取り消し（Flex / Deck 共通）。成功なら 'ok'、できなければ利用者に見せる理由を返す。
//
// ★ロックを取り、**確認ダイアログを出す前に見た seq と今の seq が同じか**を確かめる
//   （Codexレビュー指摘）。ダイアログを出している数十秒の間に、Gateway の結果報告で
//   完了していたり、別の人が新しい予約を入れていたりすると、ロック無しでは
//   「取り消したつもりが新しい予約を消した」「完了した記録を消した」になる。
//
// ★予約は**削除せず state='cancelled' にする。**削除すると seq が 1 から振り直され、
//   取り消す前に Gateway が送っていた古い seq=1 の応答が、新しい seq=1 の予約を
//   完了扱いにしてしまう。seq は子機IDごとに単調増加でなければならない。
function cancelDownlinkLocked_(childHex, expectedSeq) {
  var lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);
  } catch (err) {
    return '他の処理が実行中のため取り消せませんでした。少し待ってからもう一度試してください。';
  }
  try {
    var now = dlGet_(childHex);
    if (!now || now.seq !== expectedSeq) {
      return '確認している間に新しい予約（seq=' + (now ? now.seq : '?') + '）が入ったため、取り消しを中止しました。\n' +
             'もう一度内容を確かめてから取り消してください。';
    }
    if (now.state !== 'queued' && now.state !== 'sent') {
      return '確認している間に予約が確定しました（' + (dlStatusLabel_(now) || now.state) + '）。取り消しは不要です。';
    }
    now.state = 'cancelled';
    dlSet_(childHex, now);
    return 'ok';
  } finally {
    try { lock.releaseLock(); } catch (e2) {}
  }
}


// ================================
// コマンド予約状況シート（★2026-08-08追加）
// ================================
// pending_cmd_<deviceId>（Script Properties）は値を見ただけでは現状が分からないため、
// databoxとは別の "cmd_status" シートに一覧表示する。予約・消化のたびに自動更新する
// （setProperty/deletePropertyを行う箇所すべてから refreshCmdStatusSheet() を呼ぶ）。
const CMD_STATUS_SHEET_NAME = 'cmd_status';

// 一覧表示するGateway側デバイスID（pending_cmd_<id>を実際に持つもの）
const CMD_STATUS_GATEWAY_IDS = [
  { id: 'gateway_v11_test',  label: 'Gateway（本番 gateway_v1.1）' },
  { id: 'lora_downlink_test', label: 'Gateway（LoRaダウンリンクテスト用 03_lora_downlink_sender）' },
];

// 一覧表示するFlex子機DeviceID（LoRaダウンリンクの宛先として使われうる値）。
// 子機自体はGASを直接ポーリングしないため、pending_cmd_<id>という個別の予約枠は無い。
// 現在lora_downlink_testに積まれているdownlinkコマンドの宛先と一致する行にだけ、
// その内容を表示する（一度に1台分しか予約できない設計のため）。
const CMD_STATUS_CHILD_IDS = [
  '01', '02', '03', '04', '05', '06', '07', '08', '09', '0A', '0B', '0C', '0D', '0E', '0F', '10',
  '11', '12', '13', '14', '15', '16', '17', '18', '19', '1A', '1B', '1C', '1D', '1E', '1F',
];
// 群1以降は実機の群番号確定後にここへ追加する。

// ★2026-08-10追加: MAC列。「シート名編集」タブを廃止し、LoRa子機のMAC↔シート名対応を
// cmd_statusだけで完結させるため（MAC自体はgetDeviceSheetNameByMac_の導出ロジックが
// 生成する固定形式で、ここは表示のみ。ユーザーが編集してもrefreshCmdStatusSheet()で
// 上書きされる点に注意）。
const CMD_STATUS_HEADER = ['デバイスID', '種別', 'MAC（データ振り分け用）', '現在の予約', '状態', '結果', '試行回数', '最終更新日時'];

function getCmdStatusSheet_() {
  var ss = getSpreadsheet();
  var sheet = ss.getSheetByName(CMD_STATUS_SHEET_NAME);
  if (!sheet) {
    sheet = ss.insertSheet(CMD_STATUS_SHEET_NAME);
    sheet.appendRow(CMD_STATUS_HEADER);
    sheet.setFrozenRows(1);
  } else if (sheet.getLastColumn() < CMD_STATUS_HEADER.length) {
    // 旧4列版から列を増やした場合にヘッダーを張り替える
    sheet.getRange(1, 1, 1, CMD_STATUS_HEADER.length).setValues([CMD_STATUS_HEADER]);
  }
  return sheet;
}

// ダウンリンク履歴シート（1イベントごとに追記。障害調査用）
//
// ★2026-08-09改訂: 従来は「最終結果」しか記録しておらず、しかもseq不一致の応答は
// 記録する前にreturnしていたため、予約を入れ直すと「実際に配信された事実」が
// 履歴から完全に消えていた。予約・送信・結果のすべての段階を残す。
const DOWNLINK_LOG_HEADER = ['日時', '子機ID', 'seq', '種別', '内容', '備考', 'Gateway群'];

function getDownlinkLogSheet_() {
  var ss = getSpreadsheet();
  var sheet = ss.getSheetByName('downlink_log');
  if (!sheet) {
    sheet = ss.insertSheet('downlink_log');
    sheet.appendRow(DOWNLINK_LOG_HEADER);
    sheet.setFrozenRows(1);
  } else if (sheet.getLastColumn() < DOWNLINK_LOG_HEADER.length) {
    sheet.getRange(1, 1, 1, DOWNLINK_LOG_HEADER.length).setValues([DOWNLINK_LOG_HEADER]);
  }
  return sheet;
}

// 履歴を1行追記する。どの経路からでも必ずここを通す（記録漏れを作らないため）。
function dlLog_(childHex, seq, kind, detail, note, group) {
  try {
    var logGroup = (group === undefined) ? (parseInt(childHex, 16) >> 5) : group;
    getDownlinkLogSheet_().appendRow([
      new Date(), '0x' + childHex, seq || '', kind, detail || '', note || '', logGroup,
    ]);
  } catch (e) {
    // 履歴の書き込み失敗で本体の処理を止めない
    console.error('downlink_log 追記に失敗: ' + e);
  }
}

// 予約の現在値を読み取り、cmd_status シートを丸ごと再構成する。
function refreshCmdStatusSheet() {
  var props = PropertiesService.getScriptProperties();
  var sheet = getCmdStatusSheet_();
  var now = new Date();
  var nCol = CMD_STATUS_HEADER.length;

  // ヘッダー行以外をクリアしてから書き直す
  var lastRow = sheet.getLastRow();
  if (lastRow > 1) sheet.getRange(2, 1, lastRow - 1, nCol).clearContent();

  var rows = [];

  // Gateway側（pending_cmd_<id> に単発コマンドを持つもの。MACは無いので空欄）
  CMD_STATUS_GATEWAY_IDS.forEach(function (dev) {
    var cmd = props.getProperty('pending_cmd_' + dev.id) || 'none';
    rows.push([dev.id, dev.label, '', cmd, cmd === 'none' ? '—' : '予約中', '', '', now]);
  });

  // Flex子機側（子機IDごとに独立した予約・状態を持つ）。
  // ★MAC列はgetDeviceSheetNameByMac_が実際に照合する形式と完全一致させる
  //   （"00-00-00-00-00-<HEX2>"）。ここが表示専用の別形式だと紛らわしいため。
  downlinkChildIds_().forEach(function (childHex) {
    var mac = '00-00-00-00-00-' + childHex;
    // ★DECK_CHILD_IDS に載っていれば Deck。載っていなければ従来どおり製品プロファイルで判定する
    var productLabel = (DECK_CHILD_IDS.indexOf(childHex) >= 0)
      ? 'Deck子機'
      : (getProductProfile_(parseInt(childHex, 16)).productType + '子機');
    var d = dlGet_(childHex);
    if (!d) {
      rows.push(['0x' + childHex, productLabel, mac, 'none', '—', '', '', now]);
      return;
    }
    var req = (d.kind === 'deck')
      ? ('トリガ設定: ' + deckTrigHexToText_(d.trigHex || ''))
      : ('間隔=' + d.sleep + '分, 平均=' + d.avg + ', メジアン=' + d.median);
    var stateLabel = { queued: '予約中', sent: '送信済み（確認待ち）', done: '完了', failed: '失敗', cancelled: '取り消し' }[d.state] || d.state;
    rows.push([
      '0x' + childHex, productLabel, mac, req, stateLabel, dlStatusLabel_(d),
      d.attempts || 0, d.updated ? new Date(d.updated) : now,
    ]);
  });

  sheet.getRange(2, 1, rows.length, nCol).setValues(rows);
}


// ================================
// カスタムメニュー（スプレッドシートを開いた時に自動実行）
// ================================
function onOpen() {
  SpreadsheetApp.getUi()
    .createMenu('Gateway操作')
    .addItem('リモートリセット', 'triggerGatewayReset')
    .addItem('データ送信を停止', 'triggerGatewayStop')
    .addItem('データ送信を再開', 'triggerGatewayStart')
    .addItem('今すぐ送信', 'triggerGatewaySendNow')
    .addItem('送信間隔を変更', 'triggerGatewaySetInterval')
    .addItem('ステータス確認', 'triggerGatewayStatusNow')
    .addItem('RTC再同期', 'triggerGatewayRtcResync')
    .addItem('診断ログを吸い上げ', 'triggerGatewayLogDump')
    .addSeparator()
    .addItem('予約を取り消す', 'triggerCancelGatewayReservation')
    .addSeparator()
    .addItem('予約状況を更新', 'refreshCmdStatusSheet')
    .addSeparator()
    // ★受信データの書込み失敗時の再処理（retryStashedBatches）。**設置時に一度だけ実行すること。**
    .addItem('【初回のみ】受信データ再処理の自動実行を設定', 'installRetryTrigger')
    .addItem('退避中の受信データを今すぐ再処理', 'triggerRetryStashedNow')
    .addToUi();

  // ★2026-08-10追加: Flex子機向けのLoRaダウンリンク操作をGateway操作から分離。
  // 今後、子機側の個別設定項目（送信頻度・平均/メジアン回数以外にも増える見込み）が
  // 増えてもGateway操作メニューを圧迫しないようにするため。
  SpreadsheetApp.getUi()
    .createMenu('Flex操作')
    .addItem('送信間隔を変更', 'triggerFlexSetInterval')
    .addItem('平均/メジアン回数を変更', 'triggerFlexSetAvgMedian')
    .addItem('ステータス確認', 'triggerFlexStatusCheck')
    .addSeparator()
    .addItem('予約を取り消す', 'triggerCancelFlexReservation')
    .addToUi();

  // ★2026-09-12追加: Deck 子機（6CH計測ユニット）の操作。
  //   Flex とは設定項目が全く違う（送信間隔ではなくトリガ条件）ため、メニューを分けた。
  SpreadsheetApp.getUi()
    .createMenu('Deck操作')
    .addItem('トリガ設定シートを準備', 'setupDeckTriggerSheet')
    .addItem('トリガ設定を送信', 'triggerDeckSendTrigger')
    .addSeparator()
    .addItem('予約を取り消す', 'triggerCancelDeckReservation')
    .addToUi();

  refreshCmdStatusSheet();
}


// ================================
// 【動作確認用・一時】メール送信権限テスト
// ================================
// エディタの関数選択プルダウンでこれを選んで「実行」を押す。
// 初回は認証ダイアログが出るはずなので許可する。
// 確認後はこの関数を削除してよい。
function testMailPermission() {
  if (ADMIN_MAILS.length === 0) {
    console.log('ADMIN_MAILSが未設定です');
    return;
  }
  MailApp.sendEmail(ADMIN_MAILS[0], "【テスト】GAS権限確認", "このメールが届けば、メール送信の認証は問題ありません。");
  console.log("テストメール送信を試みました → " + ADMIN_MAILS[0]);
}


// ================================
// 基本設定
// ================================

// ================================
// 製品種別プロファイル / DEVICE_ID台帳
// ================================
// channelDefsの順番はGatewayレコードのCH1〜CH4に対応する。
// scaleで表示値へ換算し、missingValuesに一致する値は空欄にする。
// alertを定義すると、閾値到達時にprofile_alertsシートへ記録する。
// DEVICE_PRODUCT_REGISTRYの中身は案件固有なので、この共通テンプレートでは空にする。
const PRODUCT_PROFILES = {
  FLEX: {
    productType: 'Flex',
    channelDefs: [
      { key: 'CH1', label: 'CH1 メジアン', unit: 'με', scale: 1 },
      { key: 'CH2', label: 'CH2 メジアン', unit: 'με', scale: 1 },
      { key: 'CH3', label: 'CH3 メジアン', unit: 'με', scale: 1 },
      { key: 'CH4', label: 'CH4 メジアン', unit: 'με', scale: 1 },
    ],
  },
};
const DEFAULT_PRODUCT_TYPE = 'FLEX';
const DEVICE_PRODUCT_REGISTRY = {
  // 例: 'AB': 'PRODUCT_KEY'（実際の台帳は案件側で定義する）
};

// ★2026-08-04追加: リモートコマンド（resetなど）用のトークン。
// GatewayファームのGW_CMD_TOKENと同じ値にすること。set_cmdの認証にのみ使う
// （check_cmdはGateway自身からの定期ポーリングなので認証不要）。
const CMD_TOKEN = 'monita-gw-cmd-2026';

// アラート設定が始まる行番号（各デバイスシートの何行目から読むか）
const ALERT_START_ROW = 3;

// アラート設定の行数（CH1〜CH12 の 12 行分）
const SETTING_ROWS = 12;

// アラート設定の開始列（P列=dateKey, Q=閾値, R=メールタイトル, S=メール本文, T〜X=mail-1〜5）
const ALERT_START_COL = 16;  // P列
const ALERT_COL_COUNT  = 9;  // dateKey,閾値,タイトル,本文,mail-1〜5 の9列

// データ列ヘッダーの行番号（この次の行から実データ）※要現物確認
const DATA_HEADER_ROW = 17;

// 加工用データ（CH7〜CH12＝数式列）の開始列と列数
// 受信日時(A)〜受信台数(O)の後、CH1〜CH6(P〜U)、CH7〜CH12(V〜AA)
const FORMULA_START_COL = 22; // V列 = CH7
const FORMULA_COL_COUNT = 6;  // CH7〜CH12

// ★v4: project07_NEXCO 専用フォーマットのため旧 PktType 定数は未使用
// const PKT_TYPE_V303 = 0x03;
// const PKT_TYPE_V310_LORA = 0x04;

// アラートのクールダウン時間
// 【検証用】0 にすると毎回送信される（動作確認時のみ一時的に使う）
// 【本番用】60分間隔で同一キーの再送を抑止
const ALERT_COOLDOWN_MS = 60 * 60 * 1000;
// const ALERT_COOLDOWN_MS = 0;

// データ未受信アラートの判定時間（60分）
const NO_DATA_LIMIT_MS = 60 * 60 * 1000;

// 管理者メールアドレス（案件側で設定する。共通テンプレートには現場値を置かない）
const ADMIN_MAILS = [
  // "monitor@example.com"
];


// ================================
// スプレッドシート取得
// ================================
function getSpreadsheet() {
  return SpreadsheetApp.openById(SPREADSHEET_ID);
}


// ================================
// ペイロード（HEX文字列）デコード（可変長・最大12CH）
// ================================
// 形式: [0]PktType [1]DeviceID [2..]CH値(int16 LE)×N [末尾から2バイト目]Hour [末尾]Min
// ※ PktType=0x03（Monita Flex v3.03）はこの汎用形式の前提と合わないため、
//    doGet 側で先に判定し parsePayloadV303() へ振り分ける。
function parsePayload(hex) {
  var bytes = [];
  for (var i = 0; i < hex.length; i += 2) {
    bytes.push(parseInt(hex.substr(i, 2), 16));
  }
  var totalBytes = bytes.length;
  var measBytes  = totalBytes - 4; // PktType(1) + DeviceID(1) + Hour(1) + Min(1) を除く
  var numCh      = Math.floor(measBytes / 2);

  var ch = [];
  for (var c = 0; c < 12; c++) {
    if (c < numCh) {
      var lo = bytes[2 + c * 2];
      var hi = bytes[2 + c * 2 + 1];
      var val = lo | (hi << 8);
      if (val > 32767) val -= 65536;
      ch.push(val);
    } else {
      ch.push('');
    }
  }

  return {
    pktType:  totalBytes > 0 ? bytes[0] : '',
    deviceId: totalBytes > 1 ? bytes[1] : '',
    fwVersion: '',
    ch:       ch,
    chRange:  ['', '', '', ''],
    hour:     totalBytes >= 2 ? bytes[totalBytes - 2] : '',
    minute:   totalBytes >= 1 ? bytes[totalBytes - 1] : '',
  };
}


// ================================
// Monita Flex v3.03/v3.10（PktType=0x03 BLE / 0x04 LoRa）共通パーサー
// ================================
// MSD レイアウト（Company ID を除いた本体、19バイト固定。BLE/LoRaで同一）:
//   [0]    PktType     0x03
//   [1]    DeviceID
//   [2]    FW Version  子機ファームのバージョン（コミットごとに+1）
//   [3-4]  CH1         int16 LE
//   [5-6]  CH2         int16 LE
//   [7-8]  CH3         int16 LE
//   [9-10] CH4         int16 LE
//   [11-12] BATT       uint16 LE（mV）
//   [13]   Hour        uint8
//   [14]   Min         uint8
//   [15]   CH1 Range   uint8（DATA_NUM回サンプリング中の最大-最小。0〜255）
//   [16]   CH2 Range   uint8
//   [17]   CH3 Range   uint8
//   [18]   CH4 Range   uint8
//
// 対応する子機ファーム: case01_Flex/v3.03_sigfox/src/main.cpp の FW_VERSION >= 1
function parsePayloadV303(hex) {
  var bytes = [];
  for (var i = 0; i < hex.length; i += 2) {
    bytes.push(parseInt(hex.substr(i, 2), 16));
  }

  function int16le(lo, hi) {
    var val = lo | (hi << 8);
    if (val > 32767) val -= 65536;
    return val;
  }

  var ch = [
    int16le(bytes[3], bytes[4]),
    int16le(bytes[5], bytes[6]),
    int16le(bytes[7], bytes[8]),
    int16le(bytes[9], bytes[10]),
  ];
  var battMv = (bytes[11] | (bytes[12] << 8));
  var chRange = [bytes[15], bytes[16], bytes[17], bytes[18]];

  return {
    pktType:   bytes[0],
    deviceId:  bytes[1],
    fwVersion: bytes[2],
    ch:        ch,
    battMv:    battMv,
    chRange:   chRange,
    hour:      bytes[13],
    minute:    bytes[14],
  };
}


// ================================
// project07_NEXCO 専用レコードデコード（26バイト/台 = 52 hex文字）
// ================================
// レイアウト:
//   [0]    DeviceID
//   [1]    温度(int8_t, 整数℃)
//   [2-3]  CH1 メジアン(int16 LE)
//   [4-5]  CH2 メジアン(int16 LE)
//   [6-7]  CH3 メジアン(int16 LE)
//   [8-9]  CH4 メジアン(int16 LE)
//   [10-11] CH1 Max(int16 LE)
//   [12-13] CH1 Min(int16 LE)
//   [14-15] CH2 Max(int16 LE)
//   [16-17] CH2 Min(int16 LE)
//   [18-19] CH3 Max(int16 LE)
//   [20-21] CH3 Min(int16 LE)
//   [22-23] CH4 Max(int16 LE)
//   [24-25] CH4 Min(int16 LE)
function parseNEXCORecord(hex52) {
  var bytes = [];
  for (var i = 0; i < hex52.length; i += 2) {
    bytes.push(parseInt(hex52.substr(i, 2), 16));
  }
  function int16le(lo, hi) {
    var val = lo | (hi << 8);
    if (val > 32767) val -= 65536;
    return val;
  }
  function int8(b) {
    return b > 127 ? b - 256 : b;
  }
  return {
    deviceId: bytes[0],
    temp:     int8(bytes[1]),
    chMed:  [ int16le(bytes[2],  bytes[3]),
               int16le(bytes[4],  bytes[5]),
               int16le(bytes[6],  bytes[7]),
               int16le(bytes[8],  bytes[9])  ],
    chMax:  [ int16le(bytes[10], bytes[11]),
               int16le(bytes[14], bytes[15]),
               int16le(bytes[18], bytes[19]),
               int16le(bytes[22], bytes[23]) ],
    chMin:  [ int16le(bytes[12], bytes[13]),
               int16le(bytes[16], bytes[17]),
               int16le(bytes[20], bytes[21]),
               int16le(bytes[24], bytes[25]) ],
  };
}


// ================================
// Gatewayクラウドレコードデコード（旧13B / 新14Bの両対応）
// ================================
// ★2026-08-04追加: project07_NEXCO用フォーマット(52 hex文字)とgateway_v1.1の実際の
// 送信フォーマット(26 hex文字)が食い違っていたため、doGet側をgateway_v1.1に合わせた。
// レイアウト:
//   [0-3]  Epoch (uint32 LE, Gateway RTCのUNIX時刻。実測タイミングの識別用)
//   [4]    DeviceID
//   [5-6]  CH1 (int16 LE)
//   [7-8]  CH2 (int16 LE)
//   [9-10] CH3 (int16 LE)
//   [11-12] CH4 (int16 LE)
//   [13]   BATT（新14B形式のみ。0〜254 → 3000 + value*5 mV、255 → 欠測）
// 温度・CH Max/MinはGateway側で送っていないため空欄になる。
function parseGatewayRecord(hex) {
  if (hex.length !== 26 && hex.length !== 28) {
    throw new Error('Gatewayレコード長が不正です: ' + hex.length + ' hex文字');
  }
  var bytes = [];
  for (var i = 0; i < hex.length; i += 2) {
    bytes.push(parseInt(hex.substr(i, 2), 16));
  }
  function int16le(lo, hi) {
    var val = lo | (hi << 8);
    if (val > 32767) val -= 65536;
    return val;
  }
  var epoch = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
  var battEncoded = bytes.length === 14 ? bytes[13] : null;
  var battMv = (battEncoded === null || battEncoded === 255) ? '' : 3000 + battEncoded * 5;
  return {
    epoch:    epoch >>> 0,
    deviceId: bytes[4],
    ch: [
      int16le(bytes[5], bytes[6]),
      int16le(bytes[7], bytes[8]),
      int16le(bytes[9], bytes[10]),
      int16le(bytes[11], bytes[12]),
    ],
    battEncoded: battEncoded === null ? '' : battEncoded,
    battMv: battMv,
    battV: battMv === '' ? '' : battMv / 1000,
  };
}

// 旧名を残し、既存の手動テストや補助コードとの互換性を維持する。
function parseGatewayV11Record(hex) {
  return parseGatewayRecord(hex);
}


// ═══════════════════════════════════════════════════════════════════════════
// ★【設定D】MONITA Deck 子機（6CH計測ユニット）— 2026-09-12 追加
//
//   **Deck を1台でも置く現場では、下の DECK_CHILD_IDS に DeviceID を書くこと。**
//   ここが空のままだとデータは届いても行き先が無く、databox に落ちる。
//
//   ■ Deck と Flex の違い（ここを取り違えると原因が分からなくなる）
//     | | Flex | Deck |
//     |---|---|---|
//     | CH数        | 4      | 6 |
//     | 送信周期    | 可変   | **60分固定**（Gatewayのflush周期と合わせてある） |
//     | 1回の送信   | 1種類  | **2種類**（静的0x06 ＋ イベント統計0x07） |
//     | バイト順    | リトルエンディアン | **ビッグエンディアン** |
//     | シート      | child_XX | **deck_XX（静的）と deck_XX_event（イベント）** |
//     | 遠隔設定    | 送信間隔・平均・メジアン | **トリガ設定13項目**（deck_trigger シート） |
//
//     ★バイト順が Flex と逆である。Deck 側は ADC も無線の設定値もすべて
//       ビッグエンディアンで揃えてあるため、レコードもそれに合わせた。
//       **Flex のパーサをコピーして流用しないこと。**
//
//   ■ データが来ない / 値がおかしいときの見どころ
//     1. Gateway のシリアルに `&pt=06` が出ているか（出ていなければ Gateway 側の問題）
//     2. DECK_CHILD_IDS に DeviceID があるか
//     3. invalid_payload_log シートに記録が出ていないか
// ═══════════════════════════════════════════════════════════════════════════

// ★Deck 子機の DeviceID（16進2桁・大文字）。現場に合わせてここへ足す。
//   Flex と同じ採番規則（上位3bit=群 / 下位5bit=機器番号1〜31）。
const DECK_CHILD_IDS = [
  // 例: '0A',
];

const DECK_PT_STATIC = '06';   // Gateway が付けてくる &pt= の値（静的レコード）
const DECK_PT_EVENT  = '07';   // 同（イベント統計レコード）

// 1レコードの16進文字数 = (Gateway epoch 4B + ペイロードのpktTypeを除いた分) × 2
const DECK_STATIC_HEX_LEN = 48;   // 4B + 20B
const DECK_EVENT_HEX_LEN  = 50;   // 4B + 21B

const DECK_TRIG_BYTES = 13;

// ★子機の計測定数。case04_Deck/v1.00/lib/DeckMeasure/DeckMeasure.h と必ず一致させること。
//   グローバルチョップ有効・OSR 1024 で fDATA = 1295.34 SPS（SBAS949A 式8）。
//   **fCLKIN/(2×OSR) ではない。**GC 有効時は約1/3になる。
const DECK_SAMPLES_PER_SEC = 1295;   // DeckMeasure::SAMPLES_PER_SEC
const DECK_RING_SAMPLES    = 7770;   // DeckMeasure::RING_SAMPLES（ちょうど6秒ぶん）

// 変位の分解能。子機は int16・0.1 µm/LSB で送る（要件 §7.3.8）
const DECK_DISP_MM_PER_LSB = 0.0001;

function deckHexToBytes_(hex) {
  var b = [];
  for (var i = 0; i < hex.length; i += 2) b.push(parseInt(hex.substr(i, 2), 16));
  return b;
}
function deckU16be_(b, i) { return (b[i] << 8) | b[i + 1]; }
function deckI16be_(b, i) { var v = deckU16be_(b, i); return v > 32767 ? v - 65536 : v; }
function deckI8_(v)       { return v > 127 ? v - 256 : v; }
function deckEpochLe_(b)  { return ((b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)) >>> 0); }

// ── 静的レコード（pt=06 / 48 hex）────────────────────────────────
// Gateway が送る中身: epoch(4B LE) + 子機ペイロードの pktType を除いた 20B
//   [0]     DeviceID
//   [1]     子機FWバージョン
//   [2..13] CH1〜CH6 変位（int16 BE・0.1 µm/LSB）
//   [14]    基板温度（int8・℃）
//   [15]    外気温（int8・℃）
//   [16]    Hour  [17] Min（子機のRTC。Gateway epoch とずれていれば時刻同期を疑う）
//   [18]    CHステータス（bit0〜5 が CH1〜CH6。1=正常）
//   [19]    連番（0〜255で巡回。抜けを見れば取りこぼしが分かる）
function parseDeckStaticRecord(hex) {
  if (hex.length !== DECK_STATIC_HEX_LEN) {
    throw new Error('Deck静的レコード長が不正です: ' + hex.length + ' hex文字');
  }
  var b = deckHexToBytes_(hex);
  var ch = [];
  for (var c = 0; c < 6; c++) ch.push(deckI16be_(b, 4 + 2 + c * 2));
  return {
    epoch:    deckEpochLe_(b),
    deviceId: b[4],
    fwVer:    b[5],
    chRaw:    ch,
    chMm:     ch.map(function (v) { return Number((v * DECK_DISP_MM_PER_LSB).toFixed(4)); }),
    tempBoard: deckI8_(b[18]),
    tempAir:   deckI8_(b[19]),
    childHour: b[20],
    childMin:  b[21],
    chStatus:  b[22],
    seqNo:     b[23],
  };
}

// ── イベント統計レコード（pt=07 / 50 hex）───────────────────────
//   [0]     DeviceID
//   [1]     子機FWバージョン
//   [2]     集約Hour（この統計が何時台のものか）
//   [3..4]  検出件数（uint16 BE。R-3：上限に達しても数え続ける）
//   [5..6]  有効件数（uint16 BE。実際にSDへ収録できた件数）
//   [7..8]  振幅p50  [9..10] p90  [11..12] p99（uint16 BE・0.1 µm/LSB）
//   [13..18] 正規化たわみ形状 6点（uint8。0〜255 を 0〜1 に正規化した値のメジアン）
//   [19]    品質（0〜100）
//   [20]    連番
function parseDeckEventRecord(hex) {
  if (hex.length !== DECK_EVENT_HEX_LEN) {
    throw new Error('Deckイベントレコード長が不正です: ' + hex.length + ' hex文字');
  }
  var b = deckHexToBytes_(hex);
  var shape = [];
  for (var i = 0; i < 6; i++) shape.push(Number((b[17 + i] / 255).toFixed(3)));
  return {
    epoch:     deckEpochLe_(b),
    deviceId:  b[4],
    fwVer:     b[5],
    aggHour:   b[6],
    detected:  deckU16be_(b, 7),
    recorded:  deckU16be_(b, 9),
    p50Mm:     Number((deckU16be_(b, 11) * DECK_DISP_MM_PER_LSB).toFixed(4)),
    p90Mm:     Number((deckU16be_(b, 13) * DECK_DISP_MM_PER_LSB).toFixed(4)),
    p99Mm:     Number((deckU16be_(b, 15) * DECK_DISP_MM_PER_LSB).toFixed(4)),
    shape:     shape,
    quality:   b[23],
    seqNo:     b[24],
  };
}

// ── シート ──────────────────────────────────────────────
const DECK_STATIC_HEADER = [
  '計測日時', 'DeviceID', 'FW',
  'CH1(mm)', 'CH2(mm)', 'CH3(mm)', 'CH4(mm)', 'CH5(mm)', 'CH6(mm)',
  '基板温度(℃)', '外気温(℃)', '子機時刻(hh:mm)', 'CHステータス', '連番', 'LTE-M RSSI(CSQ)',
];
const DECK_EVENT_HEADER = [
  '計測日時', 'DeviceID', 'FW', '集約Hour', '検出件数', '有効件数',
  '振幅p50(mm)', '振幅p90(mm)', '振幅p99(mm)',
  '形状1', '形状2', '形状3', '形状4', '形状5', '形状6',
  '品質', '連番', 'LTE-M RSSI(CSQ)',
];

function getDeckSheet_(ss, name, header) {
  var sheet = ss.getSheetByName(name);
  if (!sheet) {
    sheet = ss.insertSheet(name);
    sheet.appendRow(header);
    sheet.setFrozenRows(1);
    console.log('Deckシートを新規作成しました: ' + name);
  }
  return sheet;
}

// ── 受信処理 ──────────────────────────────────────────
// 形式検査・ロック・書込み失敗時の退避は Flex と共通（receiveTelemetry_ を参照）。
// ここにあるのは「1レコードをシートへ書く」処理だけ。

// ★DECK_CHILD_IDS に無い DeviceID のシートは**自動作成しない**（Codexレビュー指摘）。
//   電波の化けや他現場の子機で deck_XX シートが勝手に増えると、現場の人はどれが本物か
//   判断できない。データは捨てずに invalid_payload_log へ全文を残す（1件は 48/50 hex で収まる）。
//   登録し忘れに気づいたら、そこから復元できる。
function deckRegisteredOrLog_(ss, deviceId, chunk, kind) {
  var hex = deviceIdHex_(deviceId);
  if (DECK_CHILD_IDS.indexOf(hex) >= 0) return hex;
  logInvalidPayload_(ss, 'Deck ' + kind + ': DeviceID ' + hex + ' が DECK_CHILD_IDS に未登録' +
                     '（シートは作らない。登録後にこの行の d から復元できる）', 1, chunk);
  return null;
}

function appendDeckStaticRow_(ss, chunk, csq) {
  var d = parseDeckStaticRecord(chunk);
  var hex = deckRegisteredOrLog_(ss, d.deviceId, chunk, '静的');
  if (!hex) return;
  var sheet = getDeckSheet_(ss, 'deck_' + hex, DECK_STATIC_HEADER);
  var childClock = ('0' + d.childHour).slice(-2) + ':' + ('0' + d.childMin).slice(-2);
  // CHステータスは16進1桁だと読み違えるので、正常CHを列挙した文字列にする
  var okList = [];
  for (var c = 0; c < 6; c++) if (d.chStatus & (1 << c)) okList.push('CH' + (c + 1));
  var statusText = (okList.length === 6) ? '正常' :
                   (okList.length === 0) ? '★全CH異常' : ('正常: ' + okList.join(' '));
  sheet.appendRow([
    new Date(d.epoch * 1000), d.deviceId, d.fwVer,
    d.chMm[0], d.chMm[1], d.chMm[2], d.chMm[3], d.chMm[4], d.chMm[5],
    d.tempBoard, d.tempAir, childClock, statusText, d.seqNo, csq,
  ]);
}

function appendDeckEventRow_(ss, chunk, csq) {
  var d = parseDeckEventRecord(chunk);
  var hex = deckRegisteredOrLog_(ss, d.deviceId, chunk, 'イベント');
  if (!hex) return;
  var sheet = getDeckSheet_(ss, 'deck_' + hex + '_event', DECK_EVENT_HEADER);
  sheet.appendRow([
    new Date(d.epoch * 1000), d.deviceId, d.fwVer, d.aggHour, d.detected, d.recorded,
    d.p50Mm, d.p90Mm, d.p99Mm,
    d.shape[0], d.shape[1], d.shape[2], d.shape[3], d.shape[4], d.shape[5],
    d.quality, d.seqNo, csq,
  ]);
}

// ═══════════════════════════════════════════════════════════════════════════
// Deck のトリガ設定（遠隔変更）— 要件 §7.3.7 / Gateway改修範囲 A-2
//
//   **設定は「deck_trigger」シートに書く。プロンプトで13項目も聞かない。**
//   メニュー［Deck操作］→［トリガ設定シートを準備］でシートを作り、行を埋めてから
//   ［トリガ設定を送信］を押す。値の意味と範囲はシートのヘッダーにコメントで入る。
//
//   ■ 届くまでの流れ
//     deck_trigger シート
//       → queueDeckDownlink_()（Script Properties に予約。Flexと同じ downlink_child_<HEX>）
//       → check_cmd の応答2行目以降（8番目のフィールドに16進26文字で相乗り）
//       → Gateway の applyDownlinkCache()
//       → 子機が起きた瞬間に LoRa 0x82／24B で送信
//       → 子機が 0x83／17B で「実際に適用した13バイト」を返す
//       → downlink_result で戻り、シートのO列に結果が入る
//
//   ★13バイトの並びは子機の TriggerConfig::fromBytes()
//     （case04_Deck/v1.00/lib/DeckMeasure/DeckMeasure.cpp）と**必ず一致させること。**
//     片方だけ直しても誰もエラーを出さない。静かにずれる。
//
//   ★ここでの値域検査は子機側の検査と同じにしてある。**GAS で弾いておかないと、
//     子機に拒否されるまで3回ダウンリンクを撃つことになり、原因も分かりにくい。**
// ═══════════════════════════════════════════════════════════════════════════

const DECK_TRIGGER_SHEET = 'deck_trigger';

// 列の定義。label はヘッダー、note はセルに付ける説明（現場の人が読む）。
const DECK_TRIGGER_COLUMNS = [
  { key: 'deviceId',  label: 'DeviceID',        note: '16進2桁（例 0A）。DECK_CHILD_IDS にも同じ値を登録すること' },
  { key: 'enabled',   label: '収録有効',        note: '1=収録する / 0=一時停止（検出も収録もしない）' },
  { key: 'threshold', label: '閾値',            note: 'ADCコード。0〜65535。「閾値の定義」と組で意味が決まる' },
  { key: 'threshMode',label: '閾値の定義',      note: '0=絶対値 / 1=基線からの変化量（★既定。温度で基線が動いても発火数が変わらない） / 2=変化率' },
  { key: 'chMask',    label: '対象CHマスク',    note: '監視するCHのビット和。CH1=1 CH2=2 CH3=4 CH4=8 CH5=16 CH6=32。全CH=63。0は不可' },
  { key: 'decision',  label: '判定方式',        note: '0=いずれか1CH(OR) / 1=対象CH全部(AND) / 2=N個以上' },
  { key: 'nRequired', label: 'N',               note: '判定方式=2 のときだけ使う。1〜6' },
  { key: 'durationMs',label: '継続時間(ms)',    note: '何ms超え続けたら発火するか。0〜255。スパイク除去用' },
  { key: 'deadTimeMs',label: '不感時間(ms)',    note: '発火後この時間は再発火しない。0〜2550。10ms単位に丸める' },
  { key: 'preSec',    label: 'プリトリガ(秒)',  note: '発火の何秒前から記録するか。0.5〜3.0。0.1秒単位' },
  { key: 'postSec',   label: 'ポストトリガ(秒)',note: '発火の何秒後まで記録するか。1.0以上。0.1秒単位。★プリ+ポストは5.0秒以下（リングバッファ6秒の制約）' },
  { key: 'maxPerHour',label: '最大収録件数(件/時)', note: '1時間に波形を保存する上限。超えたら件数だけ数える。0〜65535' },
  { key: 'staticMin', label: '静的計測の周期(分)', note: '1分値を出す周期。1〜255。LoRa送信は60分周期で固定' },
  { key: 'sentAt',    label: '最終送信',        note: '（自動）予約を入れた日時' },
  { key: 'result',    label: '状態',            note: '（自動）子機からの応答結果' },
];

function getDeckTriggerSheet_(ss) {
  return (ss || getSpreadsheet()).getSheetByName(DECK_TRIGGER_SHEET);
}

// 列番号（1始まり）を key から引く。★列を足したときに番号を直し忘れないようにするため、
//   14・15 のような数字をコードへ直接書かない。
function deckTriggerCol_(key) {
  for (var i = 0; i < DECK_TRIGGER_COLUMNS.length; i++) {
    if (DECK_TRIGGER_COLUMNS[i].key === key) return i + 1;
  }
  throw new Error('deck_trigger に列 ' + key + ' がありません');
}

// メニュー［トリガ設定シートを準備］。無ければ作り、有ればヘッダーと説明を貼り直す。
// ★列を増やしたときも、この関数をもう一度実行すれば既存シートが追随する。
function setupDeckTriggerSheet() {
  var ss = getSpreadsheet();
  var sheet = ss.getSheetByName(DECK_TRIGGER_SHEET) || ss.insertSheet(DECK_TRIGGER_SHEET);
  var labels = DECK_TRIGGER_COLUMNS.map(function (c) { return c.label; });
  sheet.getRange(1, 1, 1, labels.length).setValues([labels]).setFontWeight('bold');
  for (var i = 0; i < DECK_TRIGGER_COLUMNS.length; i++) {
    sheet.getRange(1, i + 1).setNote(DECK_TRIGGER_COLUMNS[i].note);
  }
  sheet.setFrozenRows(1);

  // 既定値の行を1行だけ用意する（要件 §7.3.7 の既定値・子機ファームの初期値と同じ）
  if (sheet.getLastRow() < 2) {
    sheet.appendRow([
      DECK_CHILD_IDS.length ? DECK_CHILD_IDS[0] : '0A',
      1, 200, 1, 63, 0, 2, 20, 500, 1.5, 3.5, 200, 1, '', '',
    ]);
  }
  sheet.autoResizeColumns(1, labels.length);
  SpreadsheetApp.getUi().alert(
    'deck_trigger シートを準備しました。\n\n' +
    '各列のヘッダーにマウスを乗せると、値の意味と範囲が出ます。\n' +
    '行を埋めたら［Deck操作］→［トリガ設定を送信］を押してください。');
}

// シート1行 → 13バイト配列。エラーは文字列の配列で返す（1件ずつ直せるように全部返す）。
function deckRowToTrigBytes_(row) {
  var errs = [];
  // ★空欄を Number('') = 0 として通してはいけない。「閾値を書き忘れた」が
  //   「閾値0＝常に発火」になってしまう。空欄は明確にエラーとして返す。
  function num(key, v) {
    if (v === '' || v === null || v === undefined) { errs.push(key + ' が空欄です'); return 0; }
    var x = Number(v);
    if (isNaN(x)) { errs.push(key + ' が数値ではありません（' + v + '）'); return 0; }
    return x;
  }

  // ★0/1 以外を「真なら1」に丸めない。子機は 0/1 以外を拒否する（fromBytes）
  var enabled    = Math.round(num('収録有効', row.enabled));
  var threshold  = Math.round(num('閾値', row.threshold));
  var threshMode = Math.round(num('閾値の定義', row.threshMode));
  var chMask     = Math.round(num('対象CHマスク', row.chMask));
  var decision   = Math.round(num('判定方式', row.decision));
  var nRequired  = Math.round(num('N', row.nRequired));
  var durationMs = Math.round(num('継続時間(ms)', row.durationMs));
  var deadTen    = Math.round(num('不感時間(ms)', row.deadTimeMs) / 10);   // ×10ms 単位へ
  var preTenth   = Math.round(num('プリトリガ(秒)', row.preSec) * 10);     // ×0.1秒 単位へ
  var postTenth  = Math.round(num('ポストトリガ(秒)', row.postSec) * 10);
  var maxPerHour = Math.round(num('最大収録件数(件/時)', row.maxPerHour));
  var staticMin  = Math.round(num('静的計測の周期(分)', row.staticMin));

  if (enabled !== 0 && enabled !== 1)       errs.push('収録有効は 0 か 1');
  if (threshold < 0 || threshold > 65535)   errs.push('閾値は 0〜65535');
  if (threshMode < 0 || threshMode > 2)     errs.push('閾値の定義は 0〜2');
  if (chMask < 1 || chMask > 63)            errs.push('対象CHマスクは 1〜63（0は不可）');
  if (decision < 0 || decision > 2)         errs.push('判定方式は 0〜2');
  if (decision === 2 && (nRequired < 1 || nRequired > 6)) errs.push('判定方式=2 のとき N は 1〜6');
  // ★N が監視CH数より多いと**一度も発火しない。**「車が来ない」と区別がつかないので弾く
  if (decision === 2 && chMask >= 1 && chMask <= 63 && nRequired > deckPopcount_(chMask)) {
    errs.push('N（' + nRequired + '）が対象CHの数（' + deckPopcount_(chMask) + '）より多い。一度も発火しません');
  }
  if (durationMs < 0 || durationMs > 255)   errs.push('継続時間は 0〜255 ms');
  if (deadTen < 0 || deadTen > 255)         errs.push('不感時間は 0〜2550 ms');
  if (preTenth < 5 || preTenth > 30)        errs.push('プリトリガは 0.5〜3.0 秒');
  if (postTenth < 10)                       errs.push('ポストトリガは 1.0 秒以上');
  if (maxPerHour < 0 || maxPerHour > 65535) errs.push('最大収録件数は 0〜65535');
  if (staticMin < 1 || staticMin > 255)     errs.push('静的計測の周期は 1〜255 分');

  // ★リングバッファに収まるか。子機の TriggerConfig::fitsInRing() と同じ計算にしてある。
  //   RING_SAMPLES − SD書込みの余裕1秒ぶん が使える上限。
  var need = Math.floor((preTenth + postTenth) * DECK_SAMPLES_PER_SEC / 10);
  if (need + DECK_SAMPLES_PER_SEC > DECK_RING_SAMPLES) {
    var maxSec = Math.floor((DECK_RING_SAMPLES - DECK_SAMPLES_PER_SEC) /
                            DECK_SAMPLES_PER_SEC * 10) / 10;
    errs.push('プリ+ポストが長すぎます（' + ((preTenth + postTenth) / 10) +
              '秒）。リングバッファ6秒に収まりません。合計' + maxSec + '秒以下にしてください');
  }

  if (errs.length) return { errors: errs };

  var b = [
    enabled,
    (threshold >> 8) & 0xFF, threshold & 0xFF,
    threshMode,
    chMask & 0x3F,
    ((nRequired & 0x0F) << 4) | (decision & 0x0F),
    durationMs,
    deadTen,
    preTenth,
    postTenth,
    (maxPerHour >> 8) & 0xFF, maxPerHour & 0xFF,
    staticMin,
  ];
  // 最後に子機と同じ規則でバイト列を検査する。上の欄ごとの検査と食い違ったら内部エラー
  var problems = deckTrigBytesProblems_(b);
  if (problems.length) return { errors: ['内部エラー（バイト列の検査で不合格）: ' + problems.join(' / ')] };
  return { bytes: b };
}

function deckPopcount_(v) { var n = 0; for (; v; v &= v - 1) n++; return n; }

function deckNormalizeHex_(v) {
  return ('0' + String(v).trim().replace(/^0x/i, '')).slice(-2).toUpperCase();
}

// 13バイトを**子機の TriggerConfig::fromBytes() と同じ規則で**検査する。問題の一覧を返す（空なら合格）。
// 送る前の最終確認と、子機が「適用した」と返してきた値の確認の両方に使う。
// ★DeckMeasure.cpp の fromBytes() を変えたら、ここも同時に変えること。
function deckTrigBytesProblems_(b) {
  if (!b || b.length !== DECK_TRIG_BYTES) return ['バイト数が ' + (b ? b.length : 0)];
  var errs = [];
  if (b[0] > 1)                errs.push('収録有効が 0/1 以外');
  if (b[3] > 2)                errs.push('閾値の定義が 0〜2 以外');
  if (b[4] & 0xC0)             errs.push('対象CHマスクの上位2bitが立っている');
  var mask = b[4] & 0x3F;
  if (mask === 0)              errs.push('対象CHマスクが 0');
  var dec = b[5] & 0x0F, nReq = b[5] >> 4;
  if (dec > 2)                 errs.push('判定方式が 0〜2 以外');
  if (b[12] === 0)             errs.push('静的計測の周期が 0');
  if (dec === 2 && (nReq === 0 || nReq > deckPopcount_(mask))) errs.push('N が 1〜監視CH数 の範囲外');
  var need = Math.floor((b[8] + b[9]) * DECK_SAMPLES_PER_SEC / 10);
  if (need + DECK_SAMPLES_PER_SEC > DECK_RING_SAMPLES) errs.push('プリ+ポストがリングに収まらない');
  if (b[8] < 5 || b[8] > 30)   errs.push('プリトリガが 0.5〜3.0 秒の範囲外');
  if (b[9] < 10)               errs.push('ポストトリガが 1.0 秒未満');
  return errs;
}

// 子機が「完了」と返してきた適用値が信用できるかを見る。問題が無ければ ''。
// ★Gateway は 13 バイトの中身を検査しない（status の値域だけ見る）。ここが最後の砦。
function deckAppliedCheck_(rec, statusNum, isDeckResult, appliedHex) {
  if (statusNum !== DL_STATUS_OK && statusNum !== DL_STATUS_CLAMPED) return '';
  if (!isDeckResult) return '★完了の応答に適用値が付いていません（Gateway が古い可能性）';
  var problems = deckTrigBytesProblems_(deckHexToBytes_(appliedHex));
  if (problems.length) return '★子機の適用値が規則外: ' + problems.join(' / ');
  if (statusNum === DL_STATUS_OK && appliedHex !== String(rec.trigHex || '').toUpperCase()) {
    return '★「要求どおり適用」と応答したのに要求値と一致しません（子機ファームを確認）';
  }
  return '';
}

function deckBytesToHex_(bytes) {
  return bytes.map(function (v) { return ('0' + (v & 0xFF).toString(16)).slice(-2).toUpperCase(); }).join('');
}

// 16進26文字 → 人が読める1行。子機が返してきた「実際に適用した値」の確認に使う。
function deckTrigHexToText_(hex) {
  if (!/^[0-9A-Fa-f]{26}$/.test(String(hex || ''))) return String(hex || '');
  var b = deckHexToBytes_(hex);
  var modeName = ['絶対値', '基線からの変化量', '変化率'][b[3]] || ('不明(' + b[3] + ')');
  var decName  = ['OR', 'AND', 'N個以上'][b[5] & 0x0F] || ('不明(' + (b[5] & 0x0F) + ')');
  return (b[0] ? '有効' : '停止') +
         ' 閾値=' + ((b[1] << 8) | b[2]) + '(' + modeName + ')' +
         ' CH=' + (b[4] & 0x3F) +
         ' 判定=' + decName + (((b[5] & 0x0F) === 2) ? ('(N=' + (b[5] >> 4) + ')') : '') +
         ' 継続=' + b[6] + 'ms' +
         ' 不感=' + (b[7] * 10) + 'ms' +
         ' プリ=' + (b[8] / 10) + 's ポスト=' + (b[9] / 10) + 's' +
         ' 上限=' + ((b[10] << 8) | b[11]) + '件/h' +
         ' 静的=' + b[12] + '分';
}

// Deck 用の予約を積む。Flex の queueDownlink_() と同じキー・同じ seq 採番規則を使う。
// ★kind:'deck' が付いているかどうかだけで、buildDownlinkLines_ が形式を切り替える。
function queueDeckDownlink_(childHex, trigBytes, sourceNote) {
  var lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);
    var prevRec = dlGet_(childHex);
    var nextSeq = (prevRec && prevRec.seq ? prevRec.seq : 0) + 1;
    var trigHex = deckBytesToHex_(trigBytes);

    dlSet_(childHex, {
      kind: 'deck',
      trigHex: trigHex,
      // Flex 用の欄は Deck では使わないが、共通コードが参照しても落ちないよう 0 を入れる
      sleep: 0, avg: 0, median: 0,
      state: 'queued', attempts: 0, seq: nextSeq, mode: 'set',
    });

    dlLog_(childHex, nextSeq, 'Deck予約', deckTrigHexToText_(trigHex),
           (prevRec && prevRec.state !== 'done' && prevRec.state !== 'failed' && prevRec.state !== 'cancelled')
             ? '★未完了の予約(seq=' + prevRec.seq + ')を上書きしました'
             : sourceNote);
    return nextSeq;
  } catch (err) {
    console.log('Deck downlink reservation lock error: ' + err);
    return null;
  } finally {
    try { lock.releaseLock(); } catch (e2) {}
  }
}

// メニュー［トリガ設定を送信］。deck_trigger シートの行を読んで予約する。
function triggerDeckSendTrigger() {
  var ui = SpreadsheetApp.getUi();
  var ss = getSpreadsheet();
  var sheet = getDeckTriggerSheet_(ss);
  if (!sheet) {
    ui.alert('deck_trigger シートがありません。先に［トリガ設定シートを準備］を実行してください。');
    return;
  }
  var last = sheet.getLastRow();
  if (last < 2) { ui.alert('deck_trigger シートに行がありません。'); return; }

  var values = sheet.getRange(2, 1, last - 1, DECK_TRIGGER_COLUMNS.length).getValues();
  var choices = values.map(function (r, i) {
    return (i + 1) + '. DeviceID=' + r[0] + '（閾値=' + r[2] + ' プリ=' + r[9] + 's ポスト=' + r[10] + 's）';
  });
  var r = ui.prompt('Deck トリガ設定の送信',
                    'どの行を送信しますか。番号を入力してください。\n\n' + choices.join('\n'),
                    ui.ButtonSet.OK_CANCEL);
  if (r.getSelectedButton() !== ui.Button.OK) return;
  var idx = parseInt(r.getResponseText().trim(), 10);
  if (!(idx >= 1 && idx <= values.length)) { ui.alert('番号が不正です。'); return; }

  var v = values[idx - 1];
  var row = {};
  DECK_TRIGGER_COLUMNS.forEach(function (c, i) { row[c.key] = v[i]; });

  var childHex = String(row.deviceId).trim().replace(/^0x/i, '').toUpperCase();
  childHex = ('0' + childHex).slice(-2);
  if (!/^[0-9A-F]{2}$/.test(childHex)) { ui.alert('DeviceIDは16進2桁で入力してください（例 0A）。'); return; }
  if (DECK_CHILD_IDS.indexOf(childHex) < 0) {
    ui.alert('DeviceID ' + childHex + ' が DECK_CHILD_IDS に登録されていません。\n' +
             'Code.gs の DECK_CHILD_IDS に追加してから送信してください。' +
             '（登録しないと、届いたデータの行き先が決まりません）');
    return;
  }

  var enc = deckRowToTrigBytes_(row);
  if (enc.errors) { ui.alert('設定値に誤りがあります:\n\n・' + enc.errors.join('\n・')); return; }

  var trigHex = deckBytesToHex_(enc.bytes);
  var confirm = ui.alert('この内容で送信しますか？',
                         'DeviceID: ' + childHex + '\n\n' + deckTrigHexToText_(trigHex) +
                         '\n\n（16進: ' + trigHex + '）\n\n' +
                         '★子機が次に電波を出したときに届きます。Deck は60分周期なので、' +
                         '最大で1時間ほどかかります。',
                         ui.ButtonSet.OK_CANCEL);
  if (confirm !== ui.Button.OK) return;

  var seq = queueDeckDownlink_(childHex, enc.bytes, 'deck_trigger シート ' + idx + '行目から送信');
  if (seq === null) { ui.alert('予約に失敗しました（ロックのタイムアウト）。もう一度試してください。'); return; }

  sheet.getRange(idx + 1, deckTriggerCol_('sentAt')).setValue(new Date());
  sheet.getRange(idx + 1, deckTriggerCol_('result')).setValue('送信待ち' + deckSeqTag_(seq));
  markSupersededDeckRows_(sheet, childHex, idx + 1, seq);
  ui.alert('予約しました（seq=' + seq + '）。\n子機の応答が返ると、この行の「状態」列が更新されます。');
}

// メニュー［予約を取り消す］。
function triggerCancelDeckReservation() {
  var ui = SpreadsheetApp.getUi();
  var hex = promptChildHex_(ui, 'Deck 予約の取り消し', '取り消す Deck の DeviceID を16進2桁で入力してください（例 0A）。');
  if (!hex) return;
  var rec = dlGet_(hex);
  if (!rec || (rec.state !== 'queued' && rec.state !== 'sent')) {
    ui.alert('DeviceID ' + hex + ' に未完了の予約はありません。');
    return;
  }
  var ok = ui.alert('Deck 予約の取り消し',
                    'DeviceID ' + hex + ' の以下の予約(seq=' + rec.seq + ')を取り消しますか？\n\n' +
                    deckTrigHexToText_(rec.trigHex || ''), ui.ButtonSet.YES_NO);
  if (ok !== ui.Button.YES) return;

  // ★ロックを取り、確認中に状態が変わっていないか確かめてから取り消す（cancelDownlinkLocked_ 参照）
  var res = cancelDownlinkLocked_(hex, rec.seq);
  if (res !== 'ok') { ui.alert(res); return; }

  dlLog_(hex, rec.seq, '取り消し', deckTrigHexToText_(rec.trigHex || ''), '手動で取り消しました');
  updateDeckTriggerResult_(hex, rec.seq, '取り消し済み　' +
                           Utilities.formatDate(new Date(), 'Asia/Tokyo', 'MM/dd HH:mm'));
  refreshCmdStatusSheet();
  ui.alert('DeviceID ' + hex + ' の予約(seq=' + rec.seq + ')を取り消しました。');
}

// 「状態」列に付ける seq の目印。行の特定に使うので書式を変えないこと。
// ★全角括弧で挟むので、seq=5 を探して seq=15 や seq=55 に当たることはない。
function deckSeqTag_(seq) { return '（seq=' + seq + '）'; }

// deck_trigger シートの「状態」列を、子機からの応答結果で更新する。
//
// ★**その予約を送った行**を seq の目印で探す（Codexレビュー指摘）。以前は DeviceID が
//   一致する最初の行に書いていたため、同じ子機の行が複数あると（設定を変えて試す現場では
//   普通にある）、送っていない行に「完了」が付いた。
//   目印が見つからない（行を消した・状態列を手で書き換えた）ときは、**別の行に書かない。**
//   結果は downlink_log に必ず残っている。
function updateDeckTriggerResult_(childHex, seq, text) {
  try {
    var sheet = getDeckTriggerSheet_();
    if (!sheet) return false;
    var last = sheet.getLastRow();
    if (last < 2) return false;
    var idCol = deckTriggerCol_('deviceId'), resCol = deckTriggerCol_('result');
    var ids = sheet.getRange(2, idCol, last - 1, 1).getValues();
    var res = sheet.getRange(2, resCol, last - 1, 1).getValues();
    var tag = deckSeqTag_(seq);
    for (var i = 0; i < ids.length; i++) {
      if (deckNormalizeHex_(ids[i][0]) !== childHex) continue;
      if (String(res[i][0]).indexOf(tag) < 0) continue;
      sheet.getRange(i + 2, resCol).setValue(text + tag);
      return true;
    }
    console.log('deck_trigger に ' + childHex + tag + ' の行が見つからないため状態列は更新しない（downlink_log を参照）');
  } catch (e) {
    console.log('deck_trigger 状態列の更新に失敗: ' + e);
  }
  return false;
}

// 同じ子機の別の行で「送信待ち」のまま残っているものに、置き換わったことを書く。
// GAS の予約は子機ごとに1件なので、新しい行を送った時点で古い行の予約はもう届かない。
// 書かないと、その行は永久に「送信待ち」のままになる。
function markSupersededDeckRows_(sheet, childHex, keepRow, newSeq) {
  var last = sheet.getLastRow();
  if (last < 2) return;
  var idCol = deckTriggerCol_('deviceId'), resCol = deckTriggerCol_('result');
  var ids = sheet.getRange(2, idCol, last - 1, 1).getValues();
  var res = sheet.getRange(2, resCol, last - 1, 1).getValues();
  for (var i = 0; i < ids.length; i++) {
    if (i + 2 === keepRow) continue;
    if (deckNormalizeHex_(ids[i][0]) !== childHex) continue;
    var m = String(res[i][0]).match(/^送信待ち（seq=(\d+)）$/);
    if (!m) continue;
    sheet.getRange(i + 2, resCol).setValue('置き換え済み（' + (keepRow) + '行目の予約 seq=' + newSeq +
                                           ' に置き換え。この行は送られない）' + deckSeqTag_(m[1]));
  }
}




function deviceIdHex_(deviceId) {
  return ('0' + Number(deviceId).toString(16)).slice(-2).toUpperCase();
}

function getProductProfile_(deviceId) {
  var productKey = DEVICE_PRODUCT_REGISTRY[deviceIdHex_(deviceId)] || DEFAULT_PRODUCT_TYPE;
  return PRODUCT_PROFILES[productKey] || PRODUCT_PROFILES[DEFAULT_PRODUCT_TYPE];
}

function channelHeader_(def) {
  return def.label + (def.unit ? '(' + def.unit + ')' : '');
}

function valueIsMissing_(rawValue, def) {
  return (def.missingValues || []).some(function (v) { return rawValue === v; });
}

// 製品プロファイルに従ってCH1〜4を表示値へ変換する。
// rowMissingWhenを持つ製品は、指定センチネル時にblankChannelsだけを空欄化する。
function transformChannels_(rawCh, profile) {
  var rowMissing = profile.rowMissingWhen &&
    rawCh[profile.rowMissingWhen.channel] === profile.rowMissingWhen.value;
  return profile.channelDefs.map(function (def, index) {
    if (rowMissing && profile.rowMissingWhen.blankChannels.indexOf(index) >= 0) return '';
    if (valueIsMissing_(rawCh[index], def)) return '';
    var value = rawCh[index] * (def.scale === undefined ? 1 : def.scale);
    return def.decimals === undefined ? value : Number(value.toFixed(def.decimals));
  });
}


// ================================
// MACアドレス → シート名 取得（「シート名編集」シート参照）
// ================================
// LoRa子機の疑似MAC形式（gateway_v1.1が生成する "00-00-00-00-00-<DeviceID hex2>"）から
// DeviceIDを取り出す。マッチしなければnull。
function extractLoraChildHexFromMac_(mac) {
  var m = /^00-00-00-00-00-([0-9A-Fa-f]{2})$/.exec(String(mac || ''));
  return m ? m[1].toUpperCase() : null;
}

// ★2026-08-16改訂: CMD_STATUS_CHILD_IDSに登録したLoRa子機は
// 「シート名編集」への手動登録が不要になった（群0の許可範囲は0x01〜0x1F）。
// gateway_v1.1が送る疑似MACは "00-00-00-00-00-<DeviceID>" という固定形式なので、
// DeviceIDから "child_<HEX2>" というシート名を直接導出できる。cmd_statusシートで
// 予約状況とデータ振り分け先を1つのタブにまとめて見られるようにするための変更
// （元々は「シート名編集」でMAC↔シート名を手動対応させる必要があった）。
// LoRa子機以外（BLE機器等）は従来通り「シート名編集」を参照する（互換維持）。
function getDeviceSheetNameByMac(ss, mac) {
  var loraHex = extractLoraChildHexFromMac_(mac);
  if (loraHex && CMD_STATUS_CHILD_IDS.indexOf(loraHex) >= 0) {
    return 'child_' + loraHex;
  }

  var mapSheet = ss.getSheetByName("シート名編集");
  if (!mapSheet) return null;
  var last = mapSheet.getLastRow();
  if (last < 3) return null;
  var values = mapSheet.getRange(3, 1, last - 2, 2).getValues();
  for (var i = 0; i < values.length; i++) {
    if (String(values[i][0]).toUpperCase() === String(mac).toUpperCase()) return values[i][1];
  }
  return null;  // 登録されていないMACは無視
}

// 子機データ用シートを取得する。無ければ作成して見出し行を付ける。
// ★2026-08-10追加。従来はシートが存在しないとデータを黙って捨てていた
//   （Gatewayは送信成功、GASも302を返すため、送信側のログからは気づけない）。
//   LoRa子機は getDeviceSheetNameByMac() が 'child_<HEX>' を自動で決めるので、
//   台数を増やすたびに手でシートを作る運用は現実的でない。
function getOrCreateDeviceSheet_(ss, sheetName, profile) {
  var sheet = ss.getSheetByName(sheetName);
  if (sheet) {
    updateDeviceSheetHeaders_(sheet, profile);
    return sheet;
  }

  sheet = ss.insertSheet(sheetName);
  var defs = profile.channelDefs;
  // databox互換の16列に電池電圧(Q列)を追加する。CH列の見出しは製品種別で切り替える。
  sheet.appendRow([
    '計測日時', 'DeviceID', '温度(℃)',
    channelHeader_(defs[0]), channelHeader_(defs[1]), channelHeader_(defs[2]), channelHeader_(defs[3]),
    'CH1 Max(με)', 'CH1 Min(με)', 'CH2 Max(με)', 'CH2 Min(με)',
    'CH3 Max(με)', 'CH3 Min(με)', 'CH4 Max(με)', 'CH4 Min(με)',
    'LTE-M RSSI(CSQ)',
    '電池電圧(V)',
  ]);
  sheet.setFrozenRows(1);
  console.log('子機シートを新規作成しました: ' + sheetName);
  return sheet;
}

// 既存の案件テンプレート（17行目ヘッダー）と自動作成シート（1行目ヘッダー）の
// 両方で、製品プロファイルに合うCH見出しと電池列を設定する。
function updateDeviceSheetHeaders_(sheet, profile) {
  var firstCell = String(sheet.getRange(1, 1).getValue());
  var headerRow = (firstCell === '受信日時' || firstCell === '計測日時') ? 1 : DATA_HEADER_ROW;
  var headers = profile.channelDefs.map(channelHeader_);
  sheet.getRange(headerRow, 1).setValue('計測日時');
  sheet.getRange(headerRow, 4, 1, 4).setValues([headers]);
  sheet.getRange(headerRow, 17).setValue('電池電圧(V)');
}

// d/nの自己記述形式が壊れている場合は、実行ログだけでなく専用シートにも残す。
// 不正データ本体は記録せず、調査に必要な長さと先頭だけを保存する。
function logInvalidPayload_(ss, reason, n, dBlob) {
  console.error('[INVALID-PAYLOAD] ' + reason + ' n=' + n + ' d.length=' + dBlob.length);
  try {
    var sheet = ss.getSheetByName('invalid_payload_log') || ss.insertSheet('invalid_payload_log');
    if (sheet.getLastRow() === 0) {
      sheet.appendRow(['受信日時', '理由', 'n', 'd.length', 'd先頭64文字']);
      sheet.setFrozenRows(1);
    }
    sheet.appendRow([new Date(), reason, n, dBlob.length, dBlob.substr(0, 64)]);
  } catch (err) {
    console.error('[INVALID-PAYLOAD] ログシート記録失敗: ' + err);
  }
}

function profileAlertTriggered_(value, rule) {
  if (value === '' || value === undefined || isNaN(value)) return false;
  if (rule.operator === 'eq') return value === rule.threshold;
  if (rule.operator === 'gt') return value > rule.threshold;
  return value >= rule.threshold;  // 既定はgte
}

// 製品プロファイル固有の閾値アラート。メール設定の有無にかかわらず
// profile_alertsシートへ残すため、故障兆候を黙って捨てない。
function checkProfileAlerts_(ss, profile, values, mac, measuredAt) {
  var props = PropertiesService.getScriptProperties();
  profile.channelDefs.forEach(function (def, index) {
    if (!def.alert || !profileAlertTriggered_(values[index], def.alert)) return;

    var propKey = 'lastProfileAlert_' + mac + '_' + def.key;
    var lastTs = parseInt(props.getProperty(propKey) || '0', 10);
    var now = Date.now();
    if (now - lastTs < ALERT_COOLDOWN_MS) return;

    var message = def.alert.message ||
      (profile.productType + ' ' + def.label + ' が閾値 ' + def.alert.threshold + ' に到達しました');
    var alertSheet = ss.getSheetByName('profile_alerts') || ss.insertSheet('profile_alerts');
    if (alertSheet.getLastRow() === 0) {
      alertSheet.appendRow(['計測日時', '記録日時', 'デバイス', '製品種別', '項目', '値', '内容']);
      alertSheet.setFrozenRows(1);
    }
    alertSheet.appendRow([measuredAt, new Date(), mac, profile.productType, def.label, values[index], message]);
    props.setProperty(propKey, now.toString());
    console.error('[PROFILE-ALERT] ' + mac + ' ' + message + ' value=' + values[index]);

    if (ADMIN_MAILS.length > 0) {
      try {
        MailApp.sendEmail({
          to: ADMIN_MAILS.join(','),
          subject: def.alert.title || '【MONITA警告】' + profile.productType + ' ' + def.label,
          body: message + '\nデバイス: ' + mac + '\n値: ' + values[index] + '\n計測日時: ' + measuredAt,
          name: 'MONITA通知システム',
        });
      } catch (err) {
        console.error('[PROFILE-ALERT] メール送信失敗: ' + err);
      }
    }
  });
}


// ═══════════════════════════════════════════════════════════════════════════
// 受信データの取り込みと、書込み失敗時の退避・再処理（2026-09-13 Codexレビュー対応）
//
//   ★**Gateway は GAS の応答本文を読まない。**postToGAS() は HTTP ステータス 200 だけを見て
//     成功とする（gateway_v1.21 main.cpp の postToGAS 参照）。ContentService は常に 200 を返すので、
//     ここで 'ERROR' を返しても**Gateway は再送しない。**
//     以前はロック待ちのタイムアウトやシート書込みの例外を console.log に出して 'OK' を返しており、
//     そのバッチは誰にも気づかれずに消えていた。
//
//   そこで「書けなかった分は GAS 自身が持っておき、後で書く」ようにした。
//     ① 形式が壊れている      → invalid_payload_log に残して終わり（再試行しても直らない）
//     ② ロックが取れない      → バッチ全体を Script Properties に退避
//     ③ i 件目で書込みに失敗  → **i 件目以降だけ**を退避（書けた分を二重に書かないため）
//     ④ 退避分は、次の受信のついでに2件、および10分ごとの時間主導トリガで再処理する
//     ⑤ 24時間（144回）書けなければ retry_giveup_log に全文を残して諦める
//
//   ★時間主導トリガは**設置時に一度だけ**メニュー［Gateway操作］→
//     ［【初回のみ】受信データ再処理の自動実行を設定］で入れること。
//     入れなくても④の「受信のついで」で再処理はされるが、受信が止まると退避分も止まる。
// ═══════════════════════════════════════════════════════════════════════════

const RETRY_KEY_PREFIX    = 'retry_batch_';
const RETRY_MAX_ATTEMPTS  = 144;   // 10分ごと × 144 = 24時間
const RETRY_PER_REQUEST   = 2;     // 通常の受信1回のついでに再処理する件数（応答を遅らせすぎない）
const RETRY_GIVEUP_SHEET  = 'retry_giveup_log';

// 1レコードをシートへ書く。**例外はそのまま投げる**（呼び出し側が退避に回す）。
function appendTelemetryRecord_(ss, pt, chunk, csq) {
  if (pt === DECK_PT_STATIC) return appendDeckStaticRow_(ss, chunk, csq);
  if (pt === DECK_PT_EVENT)  return appendDeckEventRow_(ss, chunk, csq);
  return appendFlexRecord_(ss, chunk, csq);
}

// Flex / One の1レコード（26 or 28 hex）。旧 doGet のループ本体をそのまま移した。
function appendFlexRecord_(ss, chunk, csq) {
  var d = parseGatewayRecord(chunk);
  var profile = getProductProfile_(d.deviceId);
  var displayCh = transformChannels_(d.ch, profile);
  var measuredAt = new Date(d.epoch * 1000);

  // DeviceID → 内部識別キー（クールダウン用）
  var macHex = deviceIdHex_(d.deviceId);
  var mac = '00-00-00-00-00-' + macHex;

  // DeviceID → シート名（「シート名編集」シートで紐付け。未登録は 'databox' へ）
  var sheetName = getDeviceSheetNameByMac(ss, mac) || 'databox';
  // ★子機シート(child_XX)は無ければ自動作成する。databox等の既存シートを取り違えて
  //   作ってしまわないよう、自動作成の対象は 'child_' で始まる名前だけに限定する。
  var sheet;
  if (sheetName.indexOf('child_') === 0) {
    sheet = getOrCreateDeviceSheet_(ss, sheetName, profile);
  } else {
    sheet = ss.getSheetByName(sheetName);
  }
  if (!sheet) {
    // ★以前は console.log だけで捨てていた。設定の問題なので再試行はしないが、データは残す
    logInvalidPayload_(ss, 'シートが見つかりません: ' + sheetName + '（DeviceID ' + macHex + '）', 1, chunk);
    return;
  }

  // 列構成（17列）。Gatewayは温度・CH Max/Minを送っていないため空欄:
  // A:計測日時(Gateway epoch) B:DeviceID C:温度(℃、空欄)
  // D:CH1 E:CH2 F:CH3 G:CH4
  // H〜O: Max/Min（空欄）
  // P:LTE-M RSSI Q:電池電圧(V。旧13B/255は空欄)
  sheet.appendRow([
    measuredAt,                                            // A: Gateway受信epoch由来の計測日時
    d.deviceId,                                            // B: DeviceID
    '',                                                    // C: 温度(未送信)
    displayCh[0], displayCh[1], displayCh[2], displayCh[3],// D-G: 製品別に変換したCH1-4
    '', '', '', '', '', '', '', '',                        // H-O: Max/Min(未送信)
    csq,                                                   // P: LTE-M RSSI
    d.battV,                                               // Q: 電池電圧(V)
  ]);

  // ★ここから先（アラート）の失敗で、**書けたレコードを退避に回してはいけない。**二重記録になる。
  try {
    // シート設定による従来アラートには製品別の表示値を渡す。
    var dataObj = {
      CH1: displayCh[0], CH2: displayCh[1], CH3: displayCh[2], CH4: displayCh[3],
    };
    checkAlertsForDeviceSheet(sheet, dataObj, mac);
    checkProfileAlerts_(ss, profile, displayCh, mac, measuredAt);
  } catch (err) {
    console.error('[ALERT] アラート判定に失敗（データは記録済み）: ' + err);
  }
}

// ★呼ぶ前に ScriptLock を取っていること。
// 戻り値: { failedAt: 書けなかった最初のレコード番号（全部書けたら -1）, error }
function ingestRecordsLocked_(ss, pt, n, perLen, dBlob, csq) {
  for (var i = 0; i < n; i++) {
    try {
      appendTelemetryRecord_(ss, pt, dBlob.substr(i * perLen, perLen), csq);
    } catch (err) {
      return { failedAt: i, error: String(err) };
    }
  }
  return { failedAt: -1, error: '' };
}

// doGet から呼ぶ。形式検査は済んでいる前提。戻り値は応答文字列（Gateway は読まない。ログ用）。
function receiveTelemetry_(ss, pt, n, perLen, dBlob, csq) {
  var lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);
  } catch (err) {
    stashBatch_(pt, n, dBlob, csq, 'ロック待ちタイムアウト: ' + err);
    return 'RETRY: lock timeout (stashed)';
  }
  var r;
  try {
    if (!ss) throw new Error('スプレッドシートを開けませんでした');
    r = ingestRecordsLocked_(ss, pt, n, perLen, dBlob, csq);
  } catch (err) {
    r = { failedAt: 0, error: String(err) };
  } finally {
    try { lock.releaseLock(); } catch (e2) {}
  }

  if (r.failedAt >= 0) {
    stashBatch_(pt, n - r.failedAt, dBlob.substr(r.failedAt * perLen), csq,
                (r.failedAt + 1) + '/' + n + '件目で書込み失敗: ' + r.error);
    return 'RETRY: write failed at ' + r.failedAt + '/' + n + ' (stashed)';
  }

  // 正常に書けた＝シートが使える状態。ついでに退避分を少しだけ片付ける
  try { retryStashedBatches_(RETRY_PER_REQUEST); } catch (err) { console.error('[RETRY] ' + err); }
  return 'OK';
}

function stashBatch_(pt, n, dBlob, csq, reason) {
  var rec = { pt: pt, n: n, d: dBlob, csq: csq, reason: reason,
              at: new Date().toISOString(), attempts: 0 };
  // キーは時刻順に並ぶようにする（再処理を古い順に行うため）
  var key = RETRY_KEY_PREFIX + Date.now() + '_' + ('00000' + Math.floor(Math.random() * 100000)).slice(-5);
  try {
    PropertiesService.getScriptProperties().setProperty(key, JSON.stringify(rec));
    console.error('[RETRY] 書けなかった受信データを退避: ' + key + ' pt=' + pt + ' n=' + n + ' 理由=' + reason);
  } catch (err) {
    // 退避すらできない。最後の手段として実行ログに全文を残す（Cloud Logging から復元できる）
    console.error('[RETRY] ★退避にも失敗。データ全文: pt=' + pt + ' n=' + n + ' csq=' + csq +
                  ' d=' + dBlob + ' 理由=' + reason + ' 退避エラー=' + err);
  }
}

function perRecordHexLen_(pt, n, dBlob) {
  if (pt === DECK_PT_STATIC) return DECK_STATIC_HEX_LEN;
  if (pt === DECK_PT_EVENT)  return DECK_EVENT_HEX_LEN;
  return dBlob.length / n;
}

function listStashedKeys_() {
  return PropertiesService.getScriptProperties().getKeys()
    .filter(function (k) { return k.indexOf(RETRY_KEY_PREFIX) === 0; })
    .sort();
}

// 退避分を古い順に最大 maxCount 件再処理する。書けた件数を返す。
function retryStashedBatches_(maxCount) {
  var keys = listStashedKeys_();
  if (!keys.length) return 0;
  var props = PropertiesService.getScriptProperties();
  var ss = getSpreadsheet();
  var done = 0;

  for (var k = 0; k < keys.length && k < maxCount; k++) {
    var lock = LockService.getScriptLock();
    if (!lock.tryLock(5000)) break;   // 混んでいるなら次の機会に回す
    try {
      // ★ロックを取ってから読み直す。並行した別の実行が先に処理していれば、もう消えている。
      //   ロックの外で読んだ値で書くと、同じバッチを二重に書く。
      var raw = props.getProperty(keys[k]);
      if (!raw) continue;
      var rec = JSON.parse(raw);
      var perLen = perRecordHexLen_(rec.pt, rec.n, rec.d);
      var r = ingestRecordsLocked_(ss, rec.pt, rec.n, perLen, rec.d, rec.csq);

      if (r.failedAt < 0) {
        props.deleteProperty(keys[k]);
        done++;
        console.log('[RETRY] 再処理で記録できました: ' + keys[k] + '（退避 ' + rec.at + '・' + (rec.attempts + 1) + '回目）');
        continue;
      }
      // 途中まで書けたら、残りだけを持ち直す
      rec.n -= r.failedAt;
      rec.d = rec.d.substr(r.failedAt * perLen);
      rec.attempts++;
      rec.reason = r.error;
      if (rec.attempts >= RETRY_MAX_ATTEMPTS && logRetryGiveUp_(ss, rec)) {
        props.deleteProperty(keys[k]);
        console.error('[RETRY] ★' + RETRY_MAX_ATTEMPTS + '回書けなかったため諦めました（' + RETRY_GIVEUP_SHEET + ' に全文）: ' + keys[k]);
      } else {
        props.setProperty(keys[k], JSON.stringify(rec));
      }
    } catch (err) {
      console.error('[RETRY] 再処理中のエラー（退避分はそのまま残す）: ' + keys[k] + ' ' + err);
    } finally {
      try { lock.releaseLock(); } catch (e2) {}
    }
  }
  return done;
}

function logRetryGiveUp_(ss, rec) {
  try {
    var sheet = ss.getSheetByName(RETRY_GIVEUP_SHEET) || ss.insertSheet(RETRY_GIVEUP_SHEET);
    if (sheet.getLastRow() === 0) {
      sheet.appendRow(['退避日時', '断念日時', 'pt', 'n', 'CSQ', '最後の理由', 'd（全文。復元に使う）']);
      sheet.setFrozenRows(1);
    }
    sheet.appendRow([new Date(rec.at), new Date(), rec.pt || '(Flex)', rec.n, rec.csq, rec.reason, rec.d]);
    return true;
  } catch (err) {
    return false;   // 記録できないなら消さずに持ち続ける
  }
}

// 時間主導トリガから呼ばれる入口（installRetryTrigger で登録）
function retryStashedBatches() {
  var n = retryStashedBatches_(50);
  var left = listStashedKeys_().length;
  if (n || left) console.log('[RETRY] 定期再処理: 記録 ' + n + ' 件 / 残り ' + left + ' 件');
}

// メニュー［【初回のみ】受信データ再処理の自動実行を設定］。何度実行しても1つだけになる。
function installRetryTrigger() {
  ScriptApp.getProjectTriggers().forEach(function (t) {
    if (t.getHandlerFunction() === 'retryStashedBatches') ScriptApp.deleteTrigger(t);
  });
  ScriptApp.newTrigger('retryStashedBatches').timeBased().everyMinutes(10).create();
  SpreadsheetApp.getUi().alert(
    '受信データの再処理を10分ごとに自動実行するよう設定しました。\n\n' +
    '現在退避されている受信データ: ' + listStashedKeys_().length + ' 件');
}

// メニュー［退避中の受信データを今すぐ再処理］
function triggerRetryStashedNow() {
  var before = listStashedKeys_().length;
  var n = retryStashedBatches_(50);
  SpreadsheetApp.getUi().alert(
    '退避中 ' + before + ' 件のうち ' + n + ' 件を記録しました。残り ' + listStashedKeys_().length + ' 件。\n\n' +
    '残りがある場合は、実行ログの [RETRY] に理由が出ています。');
}


// ================================
// Webhook 受信本体（GET）
// ================================
// ★2026-09-22（週次レビュー A14）: Gateway に「もう一度送ってほしい」と伝えるためのロック取得。
//   Gateway(v1.21〜)は応答本文を読めない（chunked 応答をモデムが扱えない）ため、HTTP 200 で
//   'error: lock timeout' と返しても成功と区別できず、ダウンリンクの報告を捨てていた。
//   ここで例外を投げると Google がエラーページ（Content-Length 付き・数KB）を返し、Gateway は
//   それを失敗として検出して報告を再送する（FW 側の GAS_ERROR_PAGE_MIN_LEN 判定）。
//   再送で直る一時的な失敗（ロック待ち）だけに使うこと。値の不正などに使うと、同じ報告を
//   永久に再送し続け、後ろに並んだ報告まで止まる。
function waitLockOrThrowRetryable_(lock) {
  try {
    lock.waitLock(10000);
  } catch (err) {
    var e = new Error('lock timeout: ' + err);
    e.retryable = true;
    throw e;
  }
}

function doGet(e) {
  var p = e.parameter;

  // ★2026-08-04追加: リモートコマンド機能（MQTT代替、HTTPSポーリング方式）
  // ------------------------------------------------------------
  // 背景: MQTT(EMQX)経由のリモートリセットを実装したが、SIM7080Gのこのファームウェアでは
  // 内蔵MQTTクライアント(AT+SM*)・生ソケットへのTLS適用(AT+CASSLCFG)の両方が
  // "operation not allowed"で機能しないことが実機検証で判明した。EMQXはTLS必須(8883番のみ)
  // のため代替不可。既に安定動作しているGASのHTTPS経由でコマンドをポーリングする方式に
  // 変更した（詳細はcase02_Gateway/firmware/gateway_v1.1のcheckRemoteCmd()参照）。
  //
  // action=check_cmd: Gatewayが定期的に呼び、保留中のコマンドがあれば返す（認証不要、
  //   Gateway自身からの定期ポーリングのため）。★2026-08-04修正: 以前はここで即座に
  //   Script Propertiesから削除していたが、GAS Web Appの302リダイレクトをGateway側で
  //   自前で追いかける都合上、1段階目（このcheck_cmd）は成功しても2段階目
  //   （リダイレクト先googleusercontent.comへの再接続）がまれに失敗することがあり、
  //   その場合コマンドが「消費済みなのに実行されない」まま失われていた。
  //   「読む」と「消費する」を分離し、Gatewayが実際に受け取れて実行する直前にだけ
  //   action=ack_cmdで明示的に消費するようにした（失敗時は次サイクルで再送される）。
  //
  // ★2026-08-10拡張: dl=1 が指定されたときだけ、応答を複数行化して2行目以降に
  //   LoRaダウンリンクの予約を相乗りさせる。
  //
  //   【なぜ相乗りさせるか】本番Gateway(v1.20)は既に毎サイクルこのcheck_cmdでGASから本文を
  //   読んでいる。予約取得のためにHTTPリクエストを1本増やすと、通信時間と失敗ポイントが
  //   増えるうえ、本番のGAS通信方式(AT+SH*)をAT+HTTPTOFSへ移行する必要が出てくる
  //   （両者は混在できず、稼働中のテレメトリ送信経路に手を入れることになる）。
  //   既存の応答に相乗りさせれば、実績のある送信経路を一切触らずに済む。
  //
  //   【なぜdl=1で明示的に要求させるか】gateway_v1.1のcheckRemoteCmd()は応答本文全体を
  //   1つの文字列として扱い、cmd == "none" / cmd == "reset" のように完全一致で判定している。
  //   無条件に複数行を返すと "none\n08:4:5:5:0:1:0" となって "none" に一致せず、
  //   旧ファームが未知のコマンドを受け取ったものとして誤動作する。
  //   dl=1 を送らない旧ファームには従来とまったく同じ1行だけを返すことで、
  //   GAS側を先に更新しても稼働中のgateway_v1.1が壊れないようにする。
  //
  //   応答形式:
  //     dl未指定  : "none" または "reset" 等のコマンド1行のみ（★従来と完全に同一）
  //     dl=1      : 1行目 = コマンド（無ければ "none"）
  //                 2行目以降 = 予約 "HEX2:sleepMin:avg:median:attempts:seq:mode"
  //
  //   dl=1 の例（コマンドは無いが予約が2件ある場合）:
  //     none
  //     08:4:5:5:0:1:0
  //     0E:60:10:10:0:1:1
  if (p.action === 'check_cmd') {
    var deviceId = p.device_id || 'default';
    var cmd = PropertiesService.getScriptProperties().getProperty('pending_cmd_' + deviceId) || '';
    if (p.dl !== '1') {
      return ContentService.createTextOutput(cmd || 'none');  // 旧ファーム互換（1行のみ）
    }
    var group = parseInt(p.group || '0', 10);  // group無しの旧ファームは群0
    var out = [cmd || 'none'].concat(buildDownlinkLines_(group));
    var text = out.join('\n');

    // ★2026-09-07: nonce付きで来たら、同じ内容をDriveのファイルにも書く（方式A）。
    //   Gatewayはこの応答本文を読まない（chunkedで読めないため）。Driveから読む。
    //   nonceを送らない従来のファームには何も影響しない。
    if (p.nonce) writeDownlinkFile_(group, String(p.nonce), text);

    return ContentService.createTextOutput(text);
  }

  // action=ack_cmd: Gatewayがコマンドを実際に受け取り、実行する直前に呼ぶ。ここで初めて
  // Script Propertiesから削除する（認証不要、Gateway自身からの呼び出しのため）。
  if (p.action === 'ack_cmd') {
    var deviceId = p.device_id || 'default';
    PropertiesService.getScriptProperties().deleteProperty('pending_cmd_' + deviceId);
    refreshCmdStatusSheet();
    return ContentService.createTextOutput('ok');
  }

  // ══════════════════════════════════════════════
  // LoRaダウンリンク（Class A + 確認応答方式。★2026-08-09追加）
  // ══════════════════════════════════════════════
  // action=check_downlinks: Gatewayが定期的に呼び、未完了の子機宛て予約を一括で取得する。
  //   子機が起きた瞬間に応答しなければならない（受信窓は2秒）ため、Gatewayは事前に
  //   この結果をローカルにキャッシュしておき、アップリンク受信時はネットワークを介さず
  //   即座にLoRa送信する。1行1件・改行区切りで返す: "08:4:5:5:0:1:0\n0E:60:10:10:0:1:1"
  //   ★件数上限はDOWNLINK_MAX_LINES（buildDownlinkLines_参照）を参照。
  if (p.action === 'check_downlinks') {
    var lines = buildDownlinkLines_();
    return ContentService.createTextOutput(lines.length ? lines.join('\n') : 'none');
  }

  // action=downlink_result: Gatewayがダウンリンクの最終結果を報告する。
  //   子機から確認フレーム(PktType 0x05)を受け取った場合はその中身を、規定回数送っても
  //   確認が返らなかった場合は status=99（未達）をGateway自身が報告する。
  //   ★2026-08-10追加: wdt=<分>（子機が計算式で導出したWDTタイムアウト）。
  //   WDT計算式が正しく機能しているかをスプレッドシート上で確認できるようにするため。
  //   例: ...?action=downlink_result&child=08&status=0&sleep=4&avg=5&median=5&attempts=1&wdt=19
  if (p.action === 'downlink_result') {
    var childHex = String(p.child || '').toUpperCase();
    if (!/^[0-9A-F]{2}$/.test(childHex)) {
      return ContentService.createTextOutput('error: bad child id');
    }
    var reportGroup = parseInt(p.group || '0', 10);  // group無しは旧ファーム互換で群0
    var childGroup = parseInt(childHex, 16) >> 5;
    if (reportGroup !== childGroup) {
      var ownershipNote = '報告元の群' + reportGroup + 'と子機IDの群' + childGroup + 'が不一致のため処理をスキップ';
      console.error('Downlink ACK ownership mismatch: child=' + childHex + ', ' + ownershipNote);
      dlLog_(childHex, p.seq, '結果(群不一致)', '', ownershipNote, reportGroup);
      return ContentService.createTextOutput('error: group mismatch');
    }
    var wdtMin = parseInt(p.wdt || '0', 10);
    // ★Deck かどうかは &trig= の有無で判別する。Gateway 側が Deck の報告にだけ付ける。
    //   &sleep= 等は付いてこないので、従来の組み立てをそのまま使うと "?" だらけになる。
    var deckTrigHex = String(p.trig || '').toUpperCase();
    var isDeckResult = /^[0-9A-F]{26}$/.test(deckTrigHex);
    var applied = isDeckResult
      ? deckTrigHexToText_(deckTrigHex)
      : ((p.sleep || '?') + ' / ' + (p.avg || '?') + ' / ' + (p.median || '?') +
         (wdtMin ? ('（WDT=' + wdtMin + '分）') : ''));
    var statusNum = parseInt(p.status || '0', 10);

    // ★予約結果の照合・状態更新が新しい予約と競合しないようにする。
    var lock = LockService.getScriptLock();
    try {
      waitLockOrThrowRetryable_(lock);

      var rec = dlGet_(childHex);

      // ★履歴は「予約が今も生きているか」に関わらず必ず残す。
      //   以前はstale時にここより前でreturnしていたため、予約を入れ直すと
      //   「実際に子機へ配信されて適用された」事実が履歴から消えていた。
      if (!rec || String(p.seq || '') !== String(rec.seq || '')) {
        dlLog_(childHex, p.seq, '結果(期限切れ)', '適用値: ' + applied,
               'この応答が返る前に予約が入れ替わっていたため、現在の予約状態には反映していません' +
               '（子機側では実際に適用されています）');
        return ContentService.createTextOutput('stale: seq mismatch');
      }

      // ★取り消した後に応答が届くことがある（Gateway がすでに送っていた）。子機には
      //   実際に適用されているので事実として記録するが、履歴には分かるように書く。
      var cancelledNote = (rec.state === 'cancelled') ? '　★取り消し後に届いた応答（子機には適用済み）' : '';
      rec.status        = statusNum;
      rec.attempts      = parseInt(p.attempts || '0', 10);
      rec.state = (rec.status === DL_STATUS_OK || rec.status === DL_STATUS_CLAMPED) ? 'done' : 'failed';

      if (rec.kind === 'deck') {
        rec.appliedTrigHex = isDeckResult ? deckTrigHex : '';
        rec.childFw = (p.fw !== undefined && /^\d+$/.test(String(p.fw))) ? parseInt(p.fw, 10) : null;
        rec.appliedCheck = deckAppliedCheck_(rec, statusNum, isDeckResult, deckTrigHex);
        dlSet_(childHex, rec);
        dlLog_(childHex, p.seq, '結果',
               '要求: ' + deckTrigHexToText_(rec.trigHex || '') + '　→　適用: ' + applied +
               (rec.childFw !== null ? ('（子機FW ' + rec.childFw + '）') : ''),
               dlStatusLabel_(rec) + '（' + rec.attempts + '回目で確定）' + cancelledNote, reportGroup);
        // deck_trigger シートの「状態」列にも書き戻す（現場はこのシートしか見ない）
        updateDeckTriggerResult_(childHex, rec.seq, dlStatusLabel_(rec) +
                                 '　' + Utilities.formatDate(new Date(), 'Asia/Tokyo', 'MM/dd HH:mm'));
      } else {
        rec.appliedSleep  = parseInt(p.sleep  || '0', 10);
        rec.appliedAvg    = parseInt(p.avg    || '0', 10);
        rec.appliedMedian = parseInt(p.median || '0', 10);
        rec.appliedWdtMin = wdtMin;
        dlSet_(childHex, rec);
        dlLog_(childHex, p.seq, '結果',
               '要求: ' + rec.sleep + ' / ' + rec.avg + ' / ' + rec.median + '　→　適用: ' + applied,
               dlStatusLabel_(rec) + '（' + rec.attempts + '回目で確定）' + cancelledNote, reportGroup);
      }
    } catch (err) {
      console.log('Downlink result error: ' + err);
      if (err && err.retryable) throw err;  // 週次レビュー A14: waitLockOrThrowRetryable_ のコメント参照
      return ContentService.createTextOutput('error: ' + err);
    } finally {
      try { lock.releaseLock(); } catch (e2) {}
    }

    refreshCmdStatusSheet();
    return ContentService.createTextOutput('ok');
  }

  // action=downlink_sent: Gatewayがダウンリンクを送信した（まだ確認は取れていない）時点の中間報告。
  //   状態を queued → sent に進め、試行回数を記録する。予約自体は消さない（確認が取れるまで再試行）。
  if (p.action === 'downlink_sent') {
    var sentHex = String(p.child || '').toUpperCase();
    if (!/^[0-9A-F]{2}$/.test(sentHex)) {
      return ContentService.createTextOutput('error: bad child id');
    }
    var sentGroup = parseInt(p.group || '0', 10);  // group無しは旧ファーム互換で群0
    var sentChildGroup = parseInt(sentHex, 16) >> 5;
    if (sentGroup !== sentChildGroup) {
      var sentOwnershipNote = '報告元の群' + sentGroup + 'と子機IDの群' + sentChildGroup + 'が不一致のため処理をスキップ';
      console.error('Downlink ACK ownership mismatch: child=' + sentHex + ', ' + sentOwnershipNote);
      dlLog_(sentHex, p.seq, '送信(群不一致)', '', sentOwnershipNote, sentGroup);
      return ContentService.createTextOutput('error: group mismatch');
    }
    // ★送信報告の照合・状態更新が新しい予約と競合しないようにする。
    var lock = LockService.getScriptLock();
    try {
      waitLockOrThrowRetryable_(lock);

      var sentRec = dlGet_(sentHex);

      if (!sentRec || String(p.seq || '') !== String(sentRec.seq || '')) {
        dlLog_(sentHex, p.seq, '送信(期限切れ)', '', '送信後に予約が入れ替わっていました');
        return ContentService.createTextOutput('stale: seq mismatch');
      }

      // ★確定済み（done / failed）・取り消し済みを sent に**戻さない**（Codexレビュー指摘）。
      //   Gateway の報告が順序逆転して「結果 → 送信」の順に届くと、完了した予約が sent に戻り、
      //   buildDownlinkLines_ が再び配信対象にして子機へ撃ち続ける。取り消しも同じく復活する。
      //   Gateway 側（FW99追補）でも順序を守るようにしたが、旧ファームや再送の重複に備えてここでも止める。
      if (sentRec.state !== 'queued' && sentRec.state !== 'sent') {
        dlLog_(sentHex, p.seq, '送信(確定済み)', '',
               '状態が ' + sentRec.state + ' のため sent に戻しません（報告の順序逆転・重複、または取り消し後の送信）',
               sentGroup);
        return ContentService.createTextOutput('stale: already ' + sentRec.state);
      }

      sentRec.state = 'sent';
      sentRec.attempts = parseInt(p.attempts || '1', 10);
      dlSet_(sentHex, sentRec);

      dlLog_(sentHex, p.seq, '送信',
             (sentRec.kind === 'deck')
               ? ('トリガ設定: ' + deckTrigHexToText_(sentRec.trigHex || ''))
               : ('間隔=' + sentRec.sleep + '分 / 平均=' + sentRec.avg + ' / メジアン=' + sentRec.median),
             sentRec.attempts + '回目（子機からの確認応答を待っています）', sentGroup);
    } catch (err) {
      console.log('Downlink sent error: ' + err);
      if (err && err.retryable) throw err;  // 週次レビュー A14: waitLockOrThrowRetryable_ のコメント参照
      return ContentService.createTextOutput('error: ' + err);
    } finally {
      try { lock.releaseLock(); } catch (e2) {}
    }

    refreshCmdStatusSheet();
    return ContentService.createTextOutput('ok');
  }

  // action=set_cmd: 管理者がブラウザ等からURLを叩いてコマンドを予約する（token認証必須）。
  // 例: .../exec?action=set_cmd&device_id=gateway_v11_test&cmd=reset&token=...
  if (p.action === 'set_cmd') {
    if (String(p.token || '') !== CMD_TOKEN) {
      return ContentService.createTextOutput('unauthorized');
    }
    var deviceId = p.device_id || 'default';
    var cmd = p.cmd || '';
    PropertiesService.getScriptProperties().setProperty('pending_cmd_' + deviceId, cmd);
    refreshCmdStatusSheet();
    return ContentService.createTextOutput('ok: queued "' + cmd + '" for device_id=' + deviceId);
  }

  // action=status_report: status_nowコマンドを受けたGatewayが即時報告してくる
  // ステータス（電波強度・稼働時間・空きヒープ）。databoxシートにinfo行と同じ形式で記録する。
  if (p.action === 'status_report') {
    var statusSheet = getSpreadsheet().getSheetByName('databox');
    if (statusSheet) {
      statusSheet.appendRow([
        new Date(),                                              // A: 受信日時
        p.device_id || 'GW',                                     // B: DeviceID欄にGW識別子
        '',                                                      // C: 温度
        '', '', '', '',                                          // D-G: CH med
        '', '', '', '', '', '', '', '',                          // H-O: Max/Min
        p.csq || '',                                             // P: CSQ
        'STATUS uptime=' + (p.uptime_min || '?') + 'min free_heap=' + (p.free_heap || '?') + 'B',
      ]);
    }
    return ContentService.createTextOutput('OK');
  }

  // action=log_dump: log_dumpコマンドを受けたGatewayが送ってくる、gwlog.csv末尾の
  // hexエンコード済みログ。デコードして'gwlog'シート（無ければ自動作成）に記録する。
  // 現場に行かずに直近の起動シーケンス等を確認できる（送れるのは末尾180バイト程度のみ）。
  if (p.action === 'log_dump') {
    var deviceId = p.device_id || 'default';
    var hex = p.log || '';
    var text = '';
    for (var li = 0; li < hex.length; li += 2) {
      text += String.fromCharCode(parseInt(hex.substr(li, 2), 16));
    }
    var ss2 = getSpreadsheet();
    var logSheet = ss2.getSheetByName('gwlog') || ss2.insertSheet('gwlog');
    logSheet.appendRow([new Date(), deviceId, text]);
    return ContentService.createTextOutput('ok');
  }

  // ★2026-07-25: 起動確認のinfo行もdataboxシートに記録するようにした（以前はログのみ）。
  // MACが実機アドレスでなくデバイス紐付けができないため、MAC/DeviceID/CH等は空欄にし、
  // XIAO_ID/SIM_IMEI/SD記録/送信間隔(分)/受信台数と、Gatewayの機種名+ファームバージョンを
  // fw_version列にまとめて記録する。
  if (p.row_type === 'info') {
    console.log('[INFO] ts=' + p.ts + ' sim=' + p.sim + ' csq=' + p.csq +
      ' xiao_id=' + p.xiao_id + ' sim_imei=' + p.sim_imei + ' sim_iccid=' + p.sim_iccid +
      ' sd=' + p.sd + ' interval_min=' + p.interval_min + ' devcount=' + p.devcount +
      ' gw_fw=' + p.gw_fw + ' gw_id=' + p.gw_id + ' group=' + p.group);

    var infoSheet = getSpreadsheet().getSheetByName('databox');
    if (infoSheet) {
      // info行: 受信日時 + Gateway情報のみ。センサ列(D-O)は空欄
      // ★2026-08-30: sim_iccid（SIMカード自体の識別番号）を追加。sim_imeiはモジュール
      //   自体の識別番号でSIMを挿し替えても変わらないため、社内デバイス管理でSIMカード
      //   自体を追跡するには別途ICCIDが必要という要望に対応した。
      infoSheet.appendRow([
        new Date(),                                              // A: 受信日時
        'GW',                                                    // B: DeviceID欄にGW識別子
        '',                                                      // C: 温度
        '', '', '', '',                                          // D-G: CH med
        '', '', '', '', '', '', '', '',                          // H-O: Max/Min
        p.csq || '',                                             // P: CSQ
        'fw' + (p.gw_fw || '?') + ' xiao=' + (p.xiao_id || '') + ' imei=' + (p.sim_imei || '') +
          ' iccid=' + (p.sim_iccid || ''),
        p.gw_id || '',                                           // R: Gateway ID
        p.group || '',                                           // S: Gateway群番号
      ]);
    }
    return ContentService.createTextOutput('OK');
  }

  // ★2026-07-25: 固定オーバーヘッド削減のため送信形式を変更。
  //   ts: DS3231のタイムスタンプ送信自体を廃止（SDカード記録専用にした）。
  //       各レコード先頭のGateway epochを計測日時として使用する。
  //   sim: キャリア名は送信自体を廃止（基本固定運用のため）
  //   csq: "csq=10進値" ではなく "q=1バイトhex" で届く（例: q=17 → 23）。
  //        値の意味は [gateway_csq_signal_strength_notes]（開発メモ）参照
  var sim = '';
  var csq = p.q ? parseInt(p.q, 16) : '';
  var nRaw = String(p.n || '1');
  var n   = parseInt(nRaw, 10);

  var dBlob = String(p.d || '');
  // ★スプレッドシートを開けないのは一時障害でありうる。ここで例外にするとバッチが消えるので、
  //   null のまま進めて receiveTelemetry_ で退避させる（形式エラーのログだけは書けない）。
  var ss = null;
  try { ss = getSpreadsheet(); } catch (err) { console.error('[RECV] スプレッドシートを開けません: ' + err); }

  // nとdから1台分の長さを自己判別する。26=旧13B、28=新14B。
  // 割り切れない/未知長/非hexはパース位置がずれるため、ログを残して全件破棄する。
  if (!/^[1-9][0-9]*$/.test(nRaw)) {
    logInvalidPayload_(ss, 'nが1以上の整数ではありません', n, dBlob);
    return ContentService.createTextOutput('ERROR: invalid n');
  }

  // ★2026-09-12: Deck 子機（&pt=06 / 07）。Gateway FW98 以降が付けてくる。
  //   **&pt= が無ければ従来の Flex 形式。**
  //   Gateway 側は 1バッチに同じ pktType のレコードしか入れないので、
  //   ここで pt を見れば1回のリクエスト全体の形式が決まる。
  var pt = String(p.pt || '');
  var isDeckPt = (pt === DECK_PT_STATIC || pt === DECK_PT_EVENT);
  if (pt !== '' && !isDeckPt) {
    logInvalidPayload_(ss, '未知の pt=' + pt + '（Code.gs 側が Gateway に追随していない可能性）', n, dBlob);
    return ContentService.createTextOutput('ERROR: unknown pt');
  }
  var label = isDeckPt ? ('Deck(pt=' + pt + ')の') : '';

  var perLen;
  if (isDeckPt) {
    perLen = (pt === DECK_PT_STATIC) ? DECK_STATIC_HEX_LEN : DECK_EVENT_HEX_LEN;
    if (dBlob.length !== perLen * n) {
      logInvalidPayload_(ss, label + '1件が' + perLen + ' hexではありません: ' + (dBlob.length / n), n, dBlob);
      return ContentService.createTextOutput('ERROR: unsupported deck record length');
    }
  } else {
    if (dBlob.length % n !== 0) {
      logInvalidPayload_(ss, 'd.lengthがnで割り切れません', n, dBlob);
      return ContentService.createTextOutput('ERROR: invalid d length');
    }
    perLen = dBlob.length / n;
    if (perLen !== 26 && perLen !== 28) {
      logInvalidPayload_(ss, '1台分が26/28 hexではありません: ' + perLen, n, dBlob);
      return ContentService.createTextOutput('ERROR: unsupported record length');
    }
  }
  if (!/^[0-9A-Fa-f]+$/.test(dBlob)) {
    logInvalidPayload_(ss, label + 'dにhex以外の文字が含まれます', n, dBlob);
    return ContentService.createTextOutput('ERROR: non-hex payload');
  }

  // 書込み・失敗時の退避・再処理は receiveTelemetry_ に集約（Flex と Deck で共通）
  return ContentService.createTextOutput(receiveTelemetry_(ss, pt, n, perLen, dBlob, csq));
}


// ================================
// アラート判定ロジック（デバイスシート内蔵の設定行を参照）
// ================================
// Row4〜15 (CH1〜CH12): B=dateKey C=閾値 D=メールタイトル E=メール本文 F〜J=mail-1〜5
function checkAlertsForDeviceSheet(sheet, dataObj, mac) {
  var values = sheet
    .getRange(ALERT_START_ROW, ALERT_START_COL, SETTING_ROWS, ALERT_COL_COUNT)
    .getValues();

  console.log('[ALERT-CHECK] シート=' + sheet.getName() + ' 読み取り範囲=' +
    ALERT_START_ROW + '行目, ' + ALERT_START_COL + '列目から ' + SETTING_ROWS + '行 x ' + ALERT_COL_COUNT + '列');
  console.log('[ALERT-CHECK] 設定内容(先頭3行): ' + JSON.stringify(values.slice(0, 3)));
  console.log('[ALERT-CHECK] dataObj: ' + JSON.stringify(dataObj));

  var props = PropertiesService.getScriptProperties();

  for (var i = 0; i < values.length; i++) {
    var row       = values[i];
    var key       = row[0];             // dateKey（例 "CH1"）
    var threshold = parseFloat(row[1]); // 閾値
    var title     = row[2];             // メールタイトル
    var body      = row[3];             // メール本文
    var addresses = row.slice(4);       // mail-1〜mail-5

    if (!key || isNaN(threshold) || !title || !body) {
      if (key) {
        console.log('[ALERT-CHECK] ' + key + ': 設定不足でスキップ (threshold=' + row[1] + ' title=' + title + ' body=' + body + ')');
      }
      continue;
    }

    var sensorValue = dataObj[key];
    console.log('[ALERT-CHECK] ' + key + ': 実測値=' + sensorValue + ' 閾値=' + threshold +
      ' 超過=' + (sensorValue !== undefined && sensorValue !== '' && !isNaN(sensorValue) && Math.abs(sensorValue) > threshold));
    if (sensorValue === undefined || sensorValue === '' || isNaN(sensorValue)) continue;

    if (Math.abs(sensorValue) > threshold) {

      var toList = addresses.filter(function(m) {
        return m && m.toString().trim() !== '';
      });
      if (toList.length === 0) continue;

      var propKey = 'lastAlert_' + mac + '_' + key;
      var lastTs  = parseInt(props.getProperty(propKey) || '0', 10);
      var now     = Date.now();

      if (now - lastTs < ALERT_COOLDOWN_MS) {
        var remainSec = Math.round((ALERT_COOLDOWN_MS - (now - lastTs)) / 1000);
        console.log('クールダウン中 [' + mac + '/' + key + '] 残り' + remainSec + '秒 → スキップ');
        continue;
      }

      var displayValue = (typeof sensorValue === 'number') ? sensorValue.toFixed(4) : sensorValue;

      try {
        MailApp.sendEmail({
          to:       toList.join(','),
          subject:  title,
          htmlBody: String(body).replace(/{{value}}/g, displayValue),
          name:     'MONITA通知システム'
        });

        props.setProperty(propKey, now.toString());
        console.log('アラート送信 [' + mac + '/' + key + '=' + displayValue + '] → ' + toList.join(','));

      } catch (err) {
        console.log('Mail send error: ' + err);
      }
    }
  }
}


// ================================
// 新規データ未受信チェック（10分）
// ================================
// 「シート名編集」に登録された全デバイスについて、最終データ受信から
// 10分以上経過していれば管理者へ警告メールを送る。
//
// 有効化手順:
//   GAS エディタ → トリガー → この関数を 5〜10分ごとに定期実行する設定を追加
function checkNoDataAlert() {

  var ss    = getSpreadsheet();
  var now   = Date.now();
  var props = PropertiesService.getScriptProperties();

  var mapSheet = ss.getSheetByName("シート名編集");
  if (!mapSheet) return;

  var lastRow = mapSheet.getLastRow();
  if (lastRow < 3) return;

  var devices = mapSheet.getRange(3, 1, lastRow - 2, 2).getValues();

  for (var r = 0; r < devices.length; r++) {
    var mac       = devices[r][0];
    var sheetName = devices[r][1];
    if (!mac || !sheetName) continue;

    var sheet = ss.getSheetByName(sheetName);
    if (!sheet) continue;

    var lastDataRow = sheet.getLastRow();
    if (lastDataRow <= DATA_HEADER_ROW) continue; // ヘッダーのみ = データなし

    // 最終データ受信時刻（A列 = 受信日時）
    var lastTime = sheet.getRange(lastDataRow, 1).getValue();
    if (!(lastTime instanceof Date)) continue;

    var diff = now - lastTime.getTime();

    var propKey   = "lastNoDataAlert_" + mac;
    var lastAlert = parseInt(props.getProperty(propKey) || "0", 10);

    if (diff > NO_DATA_LIMIT_MS && now - lastAlert > NO_DATA_LIMIT_MS) {
      MailApp.sendEmail({
        to:      ADMIN_MAILS.join(","),
        subject: "【未受信警告】" + sheetName,
        body:    "以下のデバイスで10分以上データ受信がありません。\n\n"
               + "MAC: " + mac + "\n"
               + "対象シート: " + sheetName + "\n"
               + "最終受信時刻: " + lastTime + "\n\n"
               + "ご確認をお願いいたします。",
        name:    "MONITA通知システム"
      });
      props.setProperty(propKey, now.toString());
    }
  }
}
