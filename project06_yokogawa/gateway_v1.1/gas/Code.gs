// ================================
// Monita Gateway (LTE-M) → GAS 受信スクリプト
// project06_yokogawa 専用（横河ブリッジHD案件、8CH構成）
//
// ================================
// 概要
// ================================
//   対応する子機ファーム: project06_yokogawa/ver1.1/src/main.cpp（COMM_USE_BLE、BLE_PKT_TYPE=0x11）
//   対応するGatewayファーム: project06_yokogawa/gateway_v1.1/src/main.cpp（EXPECTED_PKT_TYPE=0x11）
//
//   case02_Gateway/gas/project07_nexco/Code.gs（NEXCO案件専用）から派生。
//   NEXCO側はv3.03/project07_NEXCO専用フォーマットの複数バージョン対応で複雑化しているため、
//   横河は8CH固定の単一フォーマットのみを扱う専用スクリプトとして新規に分離した
//   （2026-08-05）。NEXCO側スプレッドシート・GASデプロイとは完全に独立しており、
//   本スクリプトの変更がNEXCO側に影響することはない。
//
// ================================
// ペイロードフォーマット（Gateway → GAS、&d= パラメータ）
// ================================
//   1台あたり 21バイト = 42 hex文字:
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
//   未計測チャンネルは 0x7FFF（32767）で埋められる（project06_yokogawa/ver1.1/src/main.cpp参照）。
//
// ================================
// スプレッドシート列構成（databox シート）
// ================================
//   A: 受信日時
//   B: DeviceID
//   C: CH1（ひずみ/変位、µε相当）
//   D: CH2
//   E: CH3
//   F: CH4
//   G: CH5
//   H: CH6（熱電対、℃）
//   I: CH7（電圧、mV）
//   J: CH8（電圧、mV）
//   K: LTE-M RSSI(CSQ)
// ================================


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
// Gatewayファーム（project06_yokogawa/gateway_v1.1/src/main.cpp）のGW_DEVICE_ID定数と
// 必ず一致させること（不一致だとボタンで予約したコマンドをGatewayが拾えない）。
// 複数Gatewayを扱うようになったら、deviceIdをプロンプトで選ばせる形に拡張すること。
var GW_DEVICE_ID = 'gateway_v11_test';

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

// 横河ブリッジHD案件専用スプレッドシート（2026-08-05、NEXCO案件のシートとは別物）
const SPREADSHEET_ID = '14g7rMQIT0tHwrYcripV84in_ME7B9yodOC7SDqFz17w';

// データを書き込むシート名
const DATABOX_SHEET_NAME = 'databox';

// リモートコマンド（resetなど）用のトークン。GatewayファームでHTTP経由のset_cmdを
// 使う場合のみ必要（現状スプレッドシートのボタンはPropertiesServiceを直接操作するため
// 未使用。将来ブラウザから直接set_cmdを叩く運用に備えて残す）。
const CMD_TOKEN = 'monita-yokogawa-gw-cmd-2026';

function getSpreadsheet() {
  return SpreadsheetApp.openById(SPREADSHEET_ID);
}

function getDataboxSheet() {
  var ss = getSpreadsheet();
  return ss.getSheetByName(DATABOX_SHEET_NAME) || ss.insertSheet(DATABOX_SHEET_NAME);
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
function parseYokogawaRecord(hex42) {
  var bytes = [];
  for (var i = 0; i < hex42.length; i += 2) {
    bytes.push(parseInt(hex42.substr(i, 2), 16));
  }
  function int16le(lo, hi) {
    var val = lo | (hi << 8);
    if (val > 32767) val -= 65536;
    return val;
  }
  var epoch = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
  var ch = [];
  for (var c = 0; c < 8; c++) {
    ch.push(int16le(bytes[5 + c * 2], bytes[6 + c * 2]));
  }
  return {
    epoch:    epoch >>> 0,
    deviceId: bytes[4],
    ch:       ch,
  };
}

// 未計測センチネル値（0x7FFF = 32767）を空欄に変換する
function sentinelToBlank(v) {
  return (v === 32767) ? '' : v;
}


// ================================
// Webhook 受信本体（GET）
// ================================
function doGet(e) {
  var p = e.parameter;

  // ------------------------------------------------------------
  // リモートコマンド機能（Gatewayファームのcheck_cmd定期ポーリングに対応）
  // ------------------------------------------------------------
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
      'GW',                                                    // B: DeviceID欄にGW識別子
      '', '', '', '', '', '', '', '',                          // C-J: CH1-8
      (p.csq || '') + '  STATUS uptime=' + (p.uptime_min || '?') +
        'min free_heap=' + (p.free_heap || '?') + 'B',         // K: CSQ + ステータス文言
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

  // 起動確認のinfo行
  if (p.row_type === 'info') {
    var infoSheet = getDataboxSheet();
    infoSheet.appendRow([
      new Date(),                                              // A: 受信日時
      'GW',                                                    // B: DeviceID欄にGW識別子
      '', '', '', '', '', '', '', '',                          // C-J: CH1-8
      (p.csq || '') + '  fw' + (p.gw_fw || '?') +
        ' xiao=' + (p.xiao_id || '') + ' imei=' + (p.sim_imei || ''),  // K
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
    var sheet = getDataboxSheet();

    for (var i = 0; i < n; i++) {
      var chunk = dBlob.substr(i * 42, 42);
      if (chunk.length < 42) continue;
      var d = parseYokogawaRecord(chunk);

      sheet.appendRow([
        new Date(),                                            // A: 受信日時
        d.deviceId,                                            // B: DeviceID
        sentinelToBlank(d.ch[0]),                               // C: CH1
        sentinelToBlank(d.ch[1]),                               // D: CH2
        sentinelToBlank(d.ch[2]),                               // E: CH3
        sentinelToBlank(d.ch[3]),                               // F: CH4
        sentinelToBlank(d.ch[4]),                               // G: CH5
        sentinelToBlank(d.ch[5]),                               // H: CH6（熱電対、0.1℃単位×10値。project06_yokogawa/ver1.1/src/main.cppのbleAdvertiseMeasurement()参照。実温度で見たい場合は列に/10の数式を追加すること）
        sentinelToBlank(d.ch[6]),                               // I: CH7（電圧、mV）
        sentinelToBlank(d.ch[7]),                               // J: CH8（電圧、mV）
        csq,                                                    // K: LTE-M RSSI
      ]);
    }
  } catch (err) {
    console.log('Lock or append error: ' + err);
  } finally {
    try { lock.releaseLock(); } catch (e2) {}
  }

  return ContentService.createTextOutput('OK');
}
