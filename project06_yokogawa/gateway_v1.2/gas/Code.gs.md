# Monita Gateway (LTE-M) → GAS 受信スクリプト（gateway_v1.2用）

`Code.gs` と同内容。ローカルで `.gs` が開けない環境向けの閲覧用Markdown版。
**実際にApps Scriptへ貼り付けるのは [Code.gs](Code.gs) の内容。このファイルは編集用途ではなく閲覧用。**

```javascript
// ================================
// Monita Gateway (LTE-M) → GAS 受信スクリプト
// project06_yokogawa 専用（横河ブリッジHD案件、8CH構成、gateway_v1.2対応）
//
// ================================
// 概要
// ================================
//   対応する子機ファーム:
//     - project06_yokogawa/ver1.1/src/main.cpp（COMM_USE_BLE、BLE_PKT_TYPE=0x11）
//     - project06_yokogawa/ver1.3/src/main.cpp（COMM_USE_LORA、LORA_PKT_TYPE=0x12）
//   対応するGatewayファーム:
//     project06_yokogawa/gateway_v1.2/src/main.cpp
//       （EXPECTED_PKT_TYPE=0x11(BLEビルド)/0x12(LoRaビルド)）
//
//   project06_yokogawa/gateway_v1.1/gas/Code.gs から分岐。ペイロードのワイヤフォーマット
//   （Epoch+DeviceID+CH1-8、42 hex文字/台）はgateway_v1.1と完全に同一のため、
//   doGet()の受信・パース部分はそのまま流用している。
//
//   ★2026-09-03: gateway_v1.2はLoRaダウンリンク基盤（GATEWAY_GROUP_ID・dl=1パラメータ）を
//   持つが、ver1.3フィールドユニット側がダウンリンク受信を未実装のため、本スクリプトでは
//   ダウンリンク応答（2行目以降）は組み立てず、check_cmdは従来どおり1行のみ返す。
//   gateway_v1.2ファーム側もdl=1を送りつつ応答が1行しかない場合に安全側で動作する設計
//   （checkRemoteCmdOnce()参照）なので、この単純な実装のままで問題ない。
//   将来ver1.3にダウンリンク受信を実装したら、汎用テンプレート
//   （case02_Gateway/gas/project07_nexco/Code.gs のbuildDownlinkLines_等）を参考に拡張する。
//
// ================================
// ペイロードフォーマット（Gateway → GAS、&d= パラメータ）
// ================================
//   ★2026-09-10: CH1-5（ひずみ/変位）の精度を上げるため、LoRaビルド（ver1.3）は
//   下記の31バイト/62hex文字フォーマットへ変更。BLEビルド（ver1.1）は従来どおり
//   21バイト/42hex文字のまま。1台あたりのバイト数はビルドによって固定なので、
//   doGet()側では dBlob.length / n で実際のチャンク長を判定して自動で振り分ける
//   （project06_yokogawa/ver1.3/src/main.cppのsendMeasurementToLoRa()、
//   　gateway_v1.2/src/main.cppのbuildBatchQuery()参照）。
//
//   [BLEビルド/ver1.1] 1台あたり 21バイト = 42 hex文字:
//     [0-3]   Epoch(uint32 LE, Gateway RTCのUNIX時刻)
//     [4]     DeviceID
//     [5-6]   CH1 (int16 LE)
//     [7-8]   CH2 (int16 LE)
//     [9-10]  CH3 (int16 LE)
//     [11-12] CH4 (int16 LE)
//     [13-14] CH5 (int16 LE)
//     [15-16] CH6 (int16 LE, 熱電対×10、0.1℃単位)
//     [17-18] CH7 (int16 LE, 電圧×1000、mV単位)
//     [19-20] CH8 (int16 LE, 電圧×1000、mV単位)
//   未計測チャンネルは 0x7FFF（32767）で埋められる。
//
//   [LoRaビルド/ver1.3] 1台あたり 31バイト = 62 hex文字:
//     [0-3]   Epoch(uint32 LE)
//     [4]     DeviceID
//     [5-8]   CH1 (int32 LE, µε×100)
//     [9-12]  CH2 (int32 LE, µε×100)
//     [13-16] CH3 (int32 LE, µε×100)
//     [17-20] CH4 (int32 LE, µε×100)
//     [21-24] CH5 (int32 LE, µε×100)
//     [25-26] CH6 (int16 LE, 熱電対×10、0.1℃単位)
//     [27-28] CH7 (int16 LE, 電圧×1000、mV単位)
//     [29-30] CH8 (int16 LE, 電圧×1000、mV単位)
//   未計測チャンネルはCH1-5が0x7FFFFFFF、CH6-8が0x7FFFで埋められる。
//
// ================================
// スプレッドシート列構成（デバイス別シート・databox シート共通）
// ================================
//   ★2026-09-11: 計測データはDEVICE_SHEET_MAP（下記）に従い、DeviceIDごとに別シートへ
//   書き込まれるようになった（例: DeviceID=30→「コクリエ保管用」、1→「顧客1」、2→「顧客2」）。
//   マップに無いDeviceIDのデータ、およびGWステータス・info行等は従来通りdataboxシートへ入る。
//   列構成はどのシートも共通（★2026-09-12: 列順を計測時刻優先に変更。過去の記録データは
//   フェーズ1の実証用のため列がズレていても問題ない）:
//   A: 受信日時（GASがこのリクエストを処理した時刻＝クラウド受信日時。Gatewayの送信間隔ぶん遅れる）
//   B: 計測日時（GatewayがそのLoRaフレームを受信した瞬間のRTC時刻〈payloadのEpoch、UTC〉。
//      フィールドユニットは計測直後にLoRa送信するため、受信日時よりも実際の計測タイミングに近い）
//   C: DeviceID
//   D: CH1（ひずみ/変位、µε相当。LoRaビルドは小数第2位まで、BLEビルドは整数）
//   E: CH2
//   F: CH3
//   G: CH4
//   H: CH5
//   I: CH6（熱電対、℃。小数第1位）
//   J: CH7（電圧、V。小数第3位）
//   K: CH8（電圧、V。小数第3位）
//   L: LTE-M RSSI(CSQ、GatewayとLTE-Mネットワーク間の電波強度)
//   M: LoRa RSSI(dBm、フィールドユニットとGateway間の電波強度。★2026-09-12追加、LoRaビルドのみ)
// ================================


// ╔════════════════════════════════════════════════════════════════════════╗
// ║  ★★★ 最初にここを書き換える（必須） ★★★                              ║
// ║                                                                        ║
// ║  このファイルを新規のApps Scriptプロジェクトへコピーしたら、           ║
// ║  横河用に新規作成したスプレッドシートのIDへ SPREADSHEET_ID を          ║
// ║  書き換えること（社内テスト用スプレッドシートを流用しないこと）。      ║
// ║                                                                        ║
// ║  IDはスプレッドシートのURLの /d/ と /edit の間:                        ║
// ║    https://docs.google.com/spreadsheets/d/【ここ】/edit                 ║
// ║                                                                        ║
// ║  【書き換えを忘れるとどうなるか】                                      ║
// ║  getSpreadsheet() の SpreadsheetApp.openById() が例外を投げる。GASは    ║
// ║  例外時に HTTP 200 で HTMLエラーページを返すため、Gateway側は          ║
// ║  ステータス200だけを見て「送信成功」と表示してしまう。                 ║
// ║  シリアルログ上は正常に見えるのにシートには1行も入らない、という        ║
// ║  非常に気づきにくい壊れ方をするので要注意。                            ║
// ╚════════════════════════════════════════════════════════════════════════╝
const SPREADSHEET_ID = 'ここに横河用の新規スプレッドシートIDを入れる';

// デプロイ後、Webアプリのdeploy IDを project06_yokogawa/gateway_v1.2/src/main.cpp の
// GAS_SCRIPT_ID 定数へ反映すること（現状は社内テスト用の値のままになっている）。


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
    .addToUi();
}


// ================================
// Gatewayリモートコマンド（スプレッドシートのボタン用）
// ================================
// doGet()のaction=set_cmdと同じ処理を、HTTP経由ではなく直接呼び出す版。
//
// ★注意: gateway_v1.2ファームはGW_DEVICE_IDをXIAOの固有IDから自動生成する
// （"gw_<16桁hex>"、gateway_v1.1の固定文字列"gateway_v11_test"とは方式が異なる）。
// 実機のシリアルモニタ起動ログに出る「GW_DEVICE_ID: gw_...」の値をここへ転記すること
// （転記を忘れるとボタンで予約したコマンドをGatewayが拾えない）。
// 複数Gatewayを扱うようになったら、deviceIdをプロンプトで選ばせる形に拡張すること。
var GW_DEVICE_ID = 'ここに実機シリアルログのGW_DEVICE_IDを転記する';

function triggerGatewayReset() {
  var ui = SpreadsheetApp.getUi();
  var response = ui.alert(
    'Gatewayをリセットしますか？',
    '次の送信サイクル（最大5分後）でGatewayが再起動します。',
    ui.ButtonSet.YES_NO
  );
  if (response !== ui.Button.YES) return;
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + GW_DEVICE_ID, 'reset');
  ui.alert('予約しました。Gatewayが次にオンラインになったタイミング（最大5分後）で再起動します。');
}

function triggerGatewayStop() {
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + GW_DEVICE_ID, 'stop');
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）でデータ送信を停止します。');
}

function triggerGatewayStart() {
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + GW_DEVICE_ID, 'start');
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）でデータ送信を再開します。');
}

function triggerGatewaySendNow() {
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + GW_DEVICE_ID, 'send_now');
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）で今持っているデータを送信します。');
}

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
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + GW_DEVICE_ID, 'interval:' + minutes);
  ui.alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）で送信間隔を' + minutes + '分に変更します。');
}

function triggerGatewayStatusNow() {
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + GW_DEVICE_ID, 'status_now');
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）でステータスをdataboxシートへ報告します。');
}

function triggerGatewayRtcResync() {
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + GW_DEVICE_ID, 'rtc_resync');
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）でRTCを網時刻に再同期します。');
}

function triggerGatewayLogDump() {
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + GW_DEVICE_ID, 'log_dump');
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）で直近のログを"gwlog"シートへ送信します。');
}


// ================================
// 基本設定
// ================================

// データを書き込むシート名（デバイス別振り分け対象外のもの用のフォールバック先）
const DATABOX_SHEET_NAME = 'databox';

// ★2026-09-12: Gateway起動時のinfo行（row_type=info）専用シート。
// 以前はdataboxシートに、子機の計測データ行と同じ列構成を無理やり流用して混在させていた
// （B列:計測日時やD-K列:CH1-8を空欄のまま、L列にcsq/fw/xiao_id/imeiを詰め込んだ文字列で
// 押し込んでいた）。info行はフィールド子機のDeviceIDを持たない「Gateway自身」の状態報告
// なので、DEVICE_SHEET_MAPによる子機ごとの振り分けとは概念が異なり、専用シートに分離する。
const GATEWAY_STATUS_SHEET_NAME = 'Gateway状態';

// リモートコマンド（resetなど）用のトークン。GatewayファームでHTTP経由のset_cmdを
// 使う場合のみ必要（現状スプレッドシートのボタンはPropertiesServiceを直接操作するため
// 未使用。将来ブラウザから直接set_cmdを叩く運用に備えて残す）。
const CMD_TOKEN = 'monita-yokogawa-gw-v1.2-cmd-2026';

// ================================
// ★2026-09-11追加: 計測データのDeviceID別シート振り分け
// ================================
// キー: フィールドユニットのDeviceID（10進数）、値: 書き込み先シート名。
// ここに登録の無いDeviceIDのデータは、従来通り DATABOX_SHEET_NAME（databox）へ書き込む
// （未知のデバイスを見落とさないためのフォールバック）。
// 新しいデバイスを追加するときは、この対応表に1行追記するだけでよい。
const DEVICE_SHEET_MAP = {
  30: 'コクリエテスト用',   // DeviceID=30 (0x1E)
  1:  '0001',            // DeviceID=1
  2:  '0002',            // DeviceID=2
};

// 新規シート作成時に入れる見出し行（databox・振り分け先シート共通のレイアウト）
const DATABOX_HEADER = [
  '受信日時', '計測日時', 'DeviceID', 'CH1', 'CH2', 'CH3', 'CH4', 'CH5', 'CH6', 'CH7', 'CH8', 'CSQ', 'LoRa RSSI',
];

// Gateway状態シート（row_type=info専用）の見出し行
const GATEWAY_STATUS_HEADER = [
  '受信日時', 'Gateway ID', '群番号', 'FWバージョン', 'XIAO ID', 'SIM IMEI', 'SIM ICCID',
  'SIM名', 'CSQ', 'SDカード', '送信間隔(分)', '受信済み台数',
];

function getSpreadsheet() {
  return SpreadsheetApp.openById(SPREADSHEET_ID);
}

function getDataboxSheet() {
  var ss = getSpreadsheet();
  var sheet = ss.getSheetByName(DATABOX_SHEET_NAME);
  if (!sheet) {
    sheet = ss.insertSheet(DATABOX_SHEET_NAME);
    sheet.appendRow(DATABOX_HEADER);
  }
  return sheet;
}

// Gateway起動時のinfo行専用。無ければ新規作成し見出し行を入れる。
function getGatewayStatusSheet() {
  var ss = getSpreadsheet();
  var sheet = ss.getSheetByName(GATEWAY_STATUS_SHEET_NAME);
  if (!sheet) {
    sheet = ss.insertSheet(GATEWAY_STATUS_SHEET_NAME);
    sheet.appendRow(GATEWAY_STATUS_HEADER);
  }
  return sheet;
}

// 計測データ用。DeviceIDに対応するシートを返す（無ければ新規作成し見出し行を入れる）。
// DEVICE_SHEET_MAPに無いDeviceIDは、既定のdataboxシートへフォールバックする。
function getSheetForDevice(deviceId) {
  var name = DEVICE_SHEET_MAP[deviceId] || DATABOX_SHEET_NAME;
  var ss = getSpreadsheet();
  var sheet = ss.getSheetByName(name);
  if (!sheet) {
    sheet = ss.insertSheet(name);
    sheet.appendRow(DATABOX_HEADER);
  }
  return sheet;
}


// ================================
// ペイロード（HEX文字列）デコード
// ================================
// レイアウト（42 hex文字 = 21バイト/台）:
//   [0-3]  Epoch (uint32 LE)
//   [4]    DeviceID
//   [5-6]  CH1 (int16 LE)
//   [7-8]  CH2
//   [9-10] CH3
//   [11-12] CH4
//   [13-14] CH5
//   [15-16] CH6
//   [17-18] CH7
//   [19-20] CH8
function int16le_(lo, hi) {
  var val = lo | (hi << 8);
  if (val > 32767) val -= 65536;
  return val;
}

function int32le_(b0, b1, b2, b3) {
  var val = (b0 | (b1 << 8) | (b2 << 16) | (b3 << 24));
  return val | 0; // 32bit符号付きへ正規化
}

// [BLEビルド/ver1.1] 42 hex文字 = 21バイト、CH1-8すべてint16
function parseYokogawaRecord(hex42) {
  var bytes = [];
  for (var i = 0; i < hex42.length; i += 2) {
    bytes.push(parseInt(hex42.substr(i, 2), 16));
  }
  var epoch = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
  var ch = [];
  for (var c = 0; c < 8; c++) {
    ch.push(int16le_(bytes[5 + c * 2], bytes[6 + c * 2]));
  }
  return {
    epoch:    epoch >>> 0,
    deviceId: bytes[4],
    ch:       ch,
  };
}

// [LoRaビルド/ver1.3] 64 hex文字 = 32バイト、CH1-5はint32・CH6-8はint16、末尾1バイトはRSSI(int8)
// ★2026-09-12: 横河ブリッジHD側にLoRaの電波強度も見せるため、Gateway受信時のRSSI(dBm)を
// 末尾に追加（project06_yokogawa/gateway_v1.2/src/main.cppのbuildBatchQuery()参照）。
function parseYokogawaRecordLora(hex64) {
  var bytes = [];
  for (var i = 0; i < hex64.length; i += 2) {
    bytes.push(parseInt(hex64.substr(i, 2), 16));
  }
  var epoch = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
  var ch = [];
  for (var c = 0; c < 5; c++) {
    var o = 5 + c * 4;
    ch.push(int32le_(bytes[o], bytes[o + 1], bytes[o + 2], bytes[o + 3]));
  }
  for (var c2 = 0; c2 < 3; c2++) {
    var o2 = 25 + c2 * 2;
    ch.push(int16le_(bytes[o2], bytes[o2 + 1]));
  }
  var rssiRaw = bytes[31];
  var rssi = (rssiRaw > 127) ? (rssiRaw - 256) : rssiRaw;  // int8へ変換(dBm)
  return {
    epoch:    epoch >>> 0,
    deviceId: bytes[4],
    ch:       ch,
    rssi:     rssi,
  };
}

// 未計測センチネル値（int16=0x7FFF、int32=0x7FFFFFFF）を空欄に変換する
function sentinelToBlank(v) {
  return (v === 32767 || v === 2147483647) ? '' : v;
}

// ★2026-09-10追加: CH1-8は無線上では整数だが、実際はスケーリングされた値
// （CH1-5=µε×100、CH6=0.1℃単位×10、CH7/8=mV単位×1000）で送られてきている
// （project06_yokogawa/ver1.3/src/main.cppのsendMeasurementToLoRa()参照）。
// 未計測センチネルの空欄化と、実単位への換算（割り算・丸め）をまとめて行う。
function sentinelToScaled(v, divisor, decimals) {
  if (v === 32767 || v === 2147483647) return '';
  return Number((v / divisor).toFixed(decimals));
}


// ================================
// Webhook 受信本体（GET）
// ================================
function doGet(e) {
  var p = e.parameter;

  // ------------------------------------------------------------
  // リモートコマンド機能（Gatewayファームのcheck_cmd定期ポーリングに対応）
  // ------------------------------------------------------------
  // gateway_v1.2は常に "&dl=1" を付けて問い合わせてくるが、本スクリプトは
  // ダウンリンク予約を組み立てないため1行のみ返す。ファーム側は応答が1行しか
  // 無い場合も安全に動作する（冒頭コメント参照）。
  if (p.action === 'check_cmd') {
    var deviceId = p.device_id || 'default';
    var cmd = PropertiesService.getScriptProperties().getProperty('pending_cmd_' + deviceId) || '';
    return ContentService.createTextOutput(cmd || 'none');
  }

  if (p.action === 'ack_cmd') {
    var deviceId2 = p.device_id || 'default';
    PropertiesService.getScriptProperties().deleteProperty('pending_cmd_' + deviceId2);
    return ContentService.createTextOutput('ok');
  }

  if (p.action === 'set_cmd') {
    if (String(p.token || '') !== CMD_TOKEN) {
      return ContentService.createTextOutput('unauthorized');
    }
    var deviceId3 = p.device_id || 'default';
    var cmd3 = p.cmd || '';
    PropertiesService.getScriptProperties().setProperty('pending_cmd_' + deviceId3, cmd3);
    return ContentService.createTextOutput('ok: queued "' + cmd3 + '" for device_id=' + deviceId3);
  }

  if (p.action === 'status_report') {
    var statusSheet = getDataboxSheet();
    statusSheet.appendRow([
      new Date(),                                              // A: 受信日時
      '',                                                      // B: 計測日時（GW自身の行なので無し）
      'GW',                                                    // C: DeviceID欄にGW識別子
      '', '', '', '', '', '', '', '',                          // D-K: CH1-8
      (p.csq || '') + '  STATUS uptime=' + (p.uptime_min || '?') +
        'min free_heap=' + (p.free_heap || '?') + 'B',         // L: CSQ + ステータス文言
    ]);
    return ContentService.createTextOutput('OK');
  }

  if (p.action === 'log_dump') {
    var deviceId4 = p.device_id || 'default';
    var hex = p.log || '';
    var text = '';
    for (var li = 0; li < hex.length; li += 2) {
      text += String.fromCharCode(parseInt(hex.substr(li, 2), 16));
    }
    var ss2 = getSpreadsheet();
    var logSheet = ss2.getSheetByName('gwlog') || ss2.insertSheet('gwlog');
    logSheet.appendRow([new Date(), deviceId4, text]);
    return ContentService.createTextOutput('ok');
  }

  // 起動確認のinfo行（子機DeviceIDを持たないGateway自身の状態報告のため、
  // DEVICE_SHEET_MAPとは別の専用シートへ書く。詳細はGATEWAY_STATUS_SHEET_NAME参照）
  if (p.row_type === 'info') {
    var infoSheet = getGatewayStatusSheet();
    infoSheet.appendRow([
      new Date(),        // 受信日時
      p.gw_id || '',     // Gateway ID
      p.group || '',     // 群番号
      p.gw_fw || '',     // FWバージョン
      p.xiao_id || '',   // XIAO ID
      p.sim_imei || '',  // SIM IMEI
      p.sim_iccid || '', // SIM ICCID
      p.sim || '',       // SIM名
      p.csq || '',       // CSQ
      p.sd || '',        // SDカード
      p.interval_min || '', // 送信間隔(分)
      p.devcount || '',  // 受信済み台数
    ]);
    return ContentService.createTextOutput('OK');
  }

  // ------------------------------------------------------------
  // 通常の計測データ受信
  // ------------------------------------------------------------
  var csq = p.q ? parseInt(p.q, 16) : '';
  var n   = parseInt(p.n || '1', 10);

  var lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);

    var dBlob = p.d || '';

    // ★2026-09-10: 1台あたりのhex文字数はビルドによって固定（BLE=42/LoRa=62）。
    // n件分をdBlob全体から均等割りして実際のチャンク長を判定する（Gatewayが
    // どちらのビルドでも同じ&d=/&n=形式で送ってくるため、GAS側で自動判別する）。
    var recLen = (n > 0) ? Math.floor(dBlob.length / n) : 0;
    var isLora = (recLen === 64);

    for (var i = 0; i < n; i++) {
      var chunk = dBlob.substr(i * recLen, recLen);
      if (chunk.length < recLen || recLen === 0) continue;
      var d = isLora ? parseYokogawaRecordLora(chunk) : parseYokogawaRecord(chunk);

      // ★2026-09-11: 1回のリクエストに複数デバイス分がまとまることがあるため、
      // レコードごとにDeviceIDを見てシートを決める（ループの外で1回だけ取得しない）。
      var sheet = getSheetForDevice(d.deviceId);

      sheet.appendRow([
        new Date(),                                            // A: 受信日時（クラウド受信日時）
        new Date(d.epoch * 1000),                               // B: 計測日時（Gateway受信時のRTC時刻、UTC epoch→Date変換）
        d.deviceId,                                            // C: DeviceID
        isLora ? sentinelToScaled(d.ch[0], 100, 2) : sentinelToBlank(d.ch[0]),  // D: CH1
        isLora ? sentinelToScaled(d.ch[1], 100, 2) : sentinelToBlank(d.ch[1]),  // E: CH2
        isLora ? sentinelToScaled(d.ch[2], 100, 2) : sentinelToBlank(d.ch[2]),  // F: CH3
        isLora ? sentinelToScaled(d.ch[3], 100, 2) : sentinelToBlank(d.ch[3]),  // G: CH4
        isLora ? sentinelToScaled(d.ch[4], 100, 2) : sentinelToBlank(d.ch[4]),  // H: CH5
        sentinelToScaled(d.ch[5], 10, 1),                       // I: CH6（熱電対、℃。小数第1位まで）
        sentinelToScaled(d.ch[6], 1000, 3),                     // J: CH7（電圧、V。小数第3位まで）
        sentinelToScaled(d.ch[7], 1000, 3),                     // K: CH8（電圧、V。小数第3位まで）
        csq,                                                    // L: LTE-M RSSI(CSQ)
        isLora ? d.rssi : '',                                   // M: LoRa RSSI(dBm、LoRaビルドのみ)
      ]);
    }
  } catch (err) {
    console.log('Lock or append error: ' + err);
  } finally {
    try { lock.releaseLock(); } catch (e2) {}
  }

  return ContentService.createTextOutput('OK');
}

```
