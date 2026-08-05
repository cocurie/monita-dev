// ================================
// Monita Gateway (LTE-M) → GAS 受信スクリプト
// project07_NEXCO 専用（中国自動車道有野川付近RC床版 剥落モニタリング）
//
// ================================
// バージョン対応表
// ================================
//   GASスクリプトバージョン: 4
//     - v1〜3: 旧フォーマット（case02_Gateway/firmware/gateway_v1.1 対応）
//     - v4: project07_NEXCO 専用フォーマットに対応（2026-07-25）
//       ペイロード: 26B/台 = 52 hex文字
//       DeviceID(1B) + Temp(1B) + CH1-4メジアン(8B) + CH1-4 Max/Min(16B, チャンネルごとに隣接)
//
//   対応する子機ファーム:
//     - project07_NEXCO/firmware/src/main.cpp（COMM_MODE_BLE）
//       SAMPLES_PER_AVG=5, MEASURE_COUNT=10
//
//   対応するGatewayファーム:
//     - project07_NEXCO/firmware_gateway/src/main.cpp
//
// ================================
// スプレッドシート列構成（databox シート）
// ================================
//   A: 受信日時
//   B: DeviceID
//   C: 温度(℃)
//   D: CH1 メジアン(με)
//   E: CH2 メジアン(με)
//   F: CH3 メジアン(με)
//   G: CH4 メジアン(με)
//   H: CH1 Max(με)
//   I: CH1 Min(με)
//   J: CH2 Max(με)
//   K: CH2 Min(με)
//   L: CH3 Max(με)
//   M: CH3 Min(με)
//   N: CH4 Max(με)
//   O: CH4 Min(με)
//   P: LTE-M RSSI(CSQ)
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
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）でデータ送信を停止します。');
}

function triggerGatewayStart() {
  var deviceId = 'gateway_v11_test';
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + deviceId, 'start');
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）でデータ送信を再開します。');
}

function triggerGatewaySendNow() {
  var deviceId = 'gateway_v11_test';
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + deviceId, 'send_now');
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
  ui.alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）で送信間隔を' + minutes + '分に変更します。');
}

// ステータス確認（電波強度・稼働時間・空きヒープをdataboxシートへ次サイクルで報告させる）
function triggerGatewayStatusNow() {
  var deviceId = 'gateway_v11_test';
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + deviceId, 'status_now');
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）でステータスをdataboxシートへ報告します。');
}

// RTC再同期（網時刻(AT+CCLK)での強制補正を今すぐ実行させる）
function triggerGatewayRtcResync() {
  var deviceId = 'gateway_v11_test';
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + deviceId, 'rtc_resync');
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）でRTCを網時刻に再同期します。');
}

// 診断ログ吸い上げ（gwlog.csv末尾を次サイクルでgwlogシートへ送らせる）
function triggerGatewayLogDump() {
  var deviceId = 'gateway_v11_test';
  PropertiesService.getScriptProperties().setProperty('pending_cmd_' + deviceId, 'log_dump');
  SpreadsheetApp.getUi().alert('予約しました。次にGatewayがオンラインになったタイミング（最大5分後）で直近のログを"gwlog"シートへ送信します。');
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
    .addToUi();
}


// ================================
// 【動作確認用・一時】メール送信権限テスト
// ================================
// エディタの関数選択プルダウンでこれを選んで「実行」を押す。
// 初回は認証ダイアログが出るはずなので許可する。
// 確認後はこの関数を削除してよい。
function testMailPermission() {
  MailApp.sendEmail(ADMIN_MAILS[0], "【テスト】GAS権限確認", "このメールが届けば、メール送信の認証は問題ありません。");
  console.log("テストメール送信を試みました → " + ADMIN_MAILS[0]);
}


// ================================
// 基本設定
// ================================

const SPREADSHEET_ID = '12VfgxPoRmpr9tkI1myzvIgvjqcDxERkVcQxXWzvYr0I';

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

// 管理者メールアドレス（未受信警告の送信先）
const ADMIN_MAILS = [
  "cocurie.kanri@gmail.com"
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
// gateway_v1.1 現行フォーマットデコード（13バイト/台 = 26 hex文字）
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
// 温度・CH Max/Minはgateway_v1.1側で送っていないため空欄になる。
function parseGatewayV11Record(hex26) {
  var bytes = [];
  for (var i = 0; i < hex26.length; i += 2) {
    bytes.push(parseInt(hex26.substr(i, 2), 16));
  }
  function int16le(lo, hi) {
    var val = lo | (hi << 8);
    if (val > 32767) val -= 65536;
    return val;
  }
  var epoch = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
  return {
    epoch:    epoch >>> 0,
    deviceId: bytes[4],
    ch: [
      int16le(bytes[5], bytes[6]),
      int16le(bytes[7], bytes[8]),
      int16le(bytes[9], bytes[10]),
      int16le(bytes[11], bytes[12]),
    ],
  };
}


// ================================
// MACアドレス → シート名 取得（「シート名編集」シート参照）
// ================================
function getDeviceSheetNameByMac(ss, mac) {
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


// ================================
// Webhook 受信本体（GET）
// ================================
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
  if (p.action === 'check_cmd') {
    var deviceId = p.device_id || 'default';
    var cmd = PropertiesService.getScriptProperties().getProperty('pending_cmd_' + deviceId) || '';
    return ContentService.createTextOutput(cmd || 'none');
  }

  // action=ack_cmd: Gatewayがコマンドを実際に受け取り、実行する直前に呼ぶ。ここで初めて
  // Script Propertiesから削除する（認証不要、Gateway自身からの呼び出しのため）。
  if (p.action === 'ack_cmd') {
    var deviceId = p.device_id || 'default';
    PropertiesService.getScriptProperties().deleteProperty('pending_cmd_' + deviceId);
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
    return ContentService.createTextOutput('ok: queued "' + cmd + '" for device_id=' + deviceId);
  }

  // action=status_report: status_nowコマンドを受けたGatewayが即時報告してくる
  // ステータス（電波強度・稼働時間・空きヒープ）。databoxシートにinfo行と同じ形式で記録する。
  if (p.action === 'status_report') {
    var statusSheet = getSpreadsheet().getSheetByName('databox');
    if (statusSheet) {
      statusSheet.appendRow([
        new Date(),                                              // A: 受信日時
        'GW',                                                    // B: DeviceID欄にGW識別子
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
      ' xiao_id=' + p.xiao_id + ' sim_imei=' + p.sim_imei +
      ' sd=' + p.sd + ' interval_min=' + p.interval_min + ' devcount=' + p.devcount +
      ' gw_fw=' + p.gw_fw);

    var infoSheet = getSpreadsheet().getSheetByName('databox');
    if (infoSheet) {
      // info行: 受信日時 + Gateway情報のみ。センサ列(D-O)は空欄
      infoSheet.appendRow([
        new Date(),                                              // A: 受信日時
        'GW',                                                    // B: DeviceID欄にGW識別子
        '',                                                      // C: 温度
        '', '', '', '',                                          // D-G: CH med
        '', '', '', '', '', '', '', '',                          // H-O: Max/Min
        p.csq || '',                                             // P: CSQ
        'fw' + (p.gw_fw || '?') + ' xiao=' + (p.xiao_id || '') + ' imei=' + (p.sim_imei || ''),
      ]);
    }
    return ContentService.createTextOutput('OK');
  }

  // ★2026-07-25: 固定オーバーヘッド削減のため送信形式を変更。
  //   ts: DS3231のタイムスタンプ送信自体を廃止（SDカード記録専用にした）。
  //       計測日時は受信日時（サーバ側のnew Date()）で代用する。
  //   sim: キャリア名は送信自体を廃止（基本固定運用のため）
  //   csq: "csq=10進値" ではなく "q=1バイトhex" で届く（例: q=17 → 23）。
  //        値の意味は [gateway_csq_signal_strength_notes]（開発メモ）参照
  var sim = '';
  var csq = p.q ? parseInt(p.q, 16) : '';
  var n   = parseInt(p.n || '1', 10);

  var ss = getSpreadsheet();

  var lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);

    var dBlob = p.d || '';

    for (var i = 0; i < n; i++) {
      var chunk = dBlob.substr(i * 26, 26);
      if (chunk.length < 26) continue;
      var d = parseGatewayV11Record(chunk);

      // DeviceID → 内部識別キー（クールダウン用）
      var macHex = ('0' + d.deviceId.toString(16)).slice(-2).toUpperCase();
      var mac = '00-00-00-00-00-' + macHex;

      // DeviceID → シート名（「シート名編集」シートで紐付け。未登録は 'databox' へ）
      var sheetName = getDeviceSheetNameByMac(ss, mac) || 'databox';
      var sheet = ss.getSheetByName(sheetName);
      if (!sheet) {
        console.log('シートが見つかりません: ' + sheetName);
        continue;
      }

      // 列構成（16列）。gateway_v1.1は温度・CH Max/Minを送っていないため空欄:
      // A:受信日時 B:DeviceID C:温度(℃、空欄)
      // D:CH1 E:CH2 F:CH3 G:CH4
      // H〜O: Max/Min（空欄）
      // P:LTE-M RSSI
      sheet.appendRow([
        new Date(),                                            // A: 受信日時
        d.deviceId,                                            // B: DeviceID
        '',                                                    // C: 温度(未送信)
        d.ch[0], d.ch[1], d.ch[2], d.ch[3],                    // D-G: CH1-4
        '', '', '', '', '', '', '', '',                        // H-O: Max/Min(未送信)
        csq,                                                   // P: LTE-M RSSI
      ]);

      // アラート判定（gateway_v1.1はCH1-4の実測値のみ。Max/Min/温度は未送信）
      var dataObj = {
        CH1: d.ch[0], CH2: d.ch[1], CH3: d.ch[2], CH4: d.ch[3],
      };

      checkAlertsForDeviceSheet(sheet, dataObj, mac);
    }

  } catch (err) {
    console.log('Lock or append error: ' + err);
  } finally {
    try { lock.releaseLock(); } catch (e2) {}
  }

  return ContentService.createTextOutput('OK');
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
        name:    "有野川モニタリングシステム"
      });
      props.setProperty(propKey, now.toString());
    }
  }
}
