// ================================
// Monita Gateway (LTE-M) → GAS 受信スクリプト
// project07_NEXCO 用
//
// 実データスプレッドシート構成に合わせた版:
//   https://docs.google.com/spreadsheets/d/1gWDPFg2qxtb61-lSDZO8KEF1y4V74aoDylNjspc0S78/
//
// 設計方針:
//   ・ファーム（case02_Gateway）・WebアプリURLは変更しない（GASは非スタンドアロンのまま）
//   ・ペイロードのCHデコードは可変長（バイト数からCH数を自動算出。最大12CH分の枠を用意）
//     ※ ただし PktType=0x03（Monita Flex v3.03）は固定レイアウトの専用パーサーで解析する
//       （FW_VERSION・CH1-4レンジを含むため、汎用の可変長デコードとは前提が異なる）
//   ・info行（row_type=info）はログのみ。どのデバイスシートにも書き込まない
//     （MACが "Monita Gateway" 固定文字列でシート名編集に登録しようがないため、そもそも紐付け不可）
//   ・デバイスの識別は MACアドレス。「シート名編集」シートで MAC → シート名 を紐付ける
//   ・アラート設定は各デバイスシート内に埋め込み（datebox1と同じ構成をテンプレートとして踏襲）
//     Row4〜15: CH1〜CH12 の dateKey / 閾値 / メールタイトル / メール本文 / mail-1〜5
//   ・未受信アラート判定は 10分（送信間隔 約5分の2周期抜けを想定）
//
// ================================
// バージョン対応表（ファームとGASの紐付け）
// ================================
//   GASスクリプトバージョン: 3
//     - v1: 汎用可変長デコードのみ（CH数はバイト数から自動算出、FW_VERSION/レンジ非対応）
//     - v2: PktType=0x03（Monita Flex v3.03, BLE）専用パーサーを追加。FW_VERSION・CH1-4レンジに対応
//     - v3: PktType=0x04（Monita Flex v3.10, LoRa）も同じ専用パーサーへ振り分け（2026-07-18）。
//       LoRaのMSDペイロードはBLEのv3.03と同一19バイトレイアウトのため、parsePayloadV303()を
//       そのまま流用できる。★LoRaはBLEのMACアドレスに相当するものが無く、Gatewayファームが
//       {0,0,0,0,0,DeviceID}の疑似MAC（例: DeviceID=0x01 → "01-00-00-00-00-00"）を使うため、
//       「シート名編集」シートにこの疑似MAC形式で事前登録しておく必要がある（登録が無いと
//       "未登録デバイス" としてスキップされる）。
//
//   対応する子機ファーム:
//     - Monita Flex v3.03（BLE）: case01_Flex/v3.03_sigfox/src/main.cpp の FW_VERSION >= 1
//       （FW_VERSION未満のバージョンは旧16バイト形式のため本パーサーでは正しく解析できない）
//     - Monita Flex v3.10（LoRa）: case01_Flex/v3.10_lora/src/main.cpp（COMM_MODE_LORA）
//
//   対応するGatewayファーム:
//     - case02_Gateway/firmware/gateway_v1.1 の GATEWAY_FW_VERSION >= 1
//       （MAX_PAYLOAD=24に拡張済み。MAX_PAYLOAD=16の旧バージョンではv3.03の19バイトMSDが
//         末尾で切り詰められ、CH1-4レンジが欠落するため要アップデート）
//     - Gatewayは受信バイト列をそのまま転送するだけなので、GAS側のパース処理さえ
//       対応していれば旧Gatewayファームでも「MAX_PAYLOAD拡張前」の範囲内では動作する
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

const SPREADSHEET_ID = '1gWDPFg2qxtb61-lSDZO8KEF1y4V74aoDylNjspc0S78';

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

// Monita Flex v3.03（BLEモード）の Pkt type
const PKT_TYPE_V303 = 0x03;
// Monita Flex v3.10（LoRaモード）の Pkt type。MSDレイアウトはv3.03と同一のため同じパーサーを使う
const PKT_TYPE_V310_LORA = 0x04;

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
// 圧縮エンコード（DeviceID + CH1〜4のみ、9バイト/台）デコード
// ================================
// ★2026-07-25追加: 512バイト送信ポリシーのもと、MAC全体・RSSI・FWVersion・
// BATT・Hour-Min・Rangeを送るのをやめ、1台9バイト（18 hex文字）に圧縮した。
// 対応するGateway側: case02_Gateway/firmware/gateway_v1.1/src/main.cpp buildBatchQuery()
// レイアウト: [0]DeviceID [1-2]CH1 [3-4]CH2 [5-6]CH3 [7-8]CH4（すべてint16 LE）
function parseCompactRecord(hex9) {
  var bytes = [];
  for (var i = 0; i < hex9.length; i += 2) {
    bytes.push(parseInt(hex9.substr(i, 2), 16));
  }
  function int16le(lo, hi) {
    var val = lo | (hi << 8);
    if (val > 32767) val -= 65536;
    return val;
  }
  return {
    deviceId: bytes[0],
    ch: [
      int16le(bytes[1], bytes[2]),
      int16le(bytes[3], bytes[4]),
      int16le(bytes[5], bytes[6]),
      int16le(bytes[7], bytes[8]),
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

  // info行（Gateway起動時の機器情報）はログのみ。MACが実機アドレスでなくシート紐付け不可のため書き込まない
  if (p.row_type === 'info') {
    console.log('[INFO] ts=' + p.ts + ' sim=' + p.sim + ' csq=' + p.csq +
      ' xiao_id=' + p.xiao_id + ' sim_imei=' + p.sim_imei +
      ' sd=' + p.sd + ' interval_min=' + p.interval_min + ' devcount=' + p.devcount +
      ' gw_fw=' + p.gw_fw);
    return ContentService.createTextOutput('OK');
  }

  var ts  = p.ts  || '';
  var sim = p.sim || '';
  var csq = p.csq || '';
  var n   = parseInt(p.n || '1', 10);

  var ss = getSpreadsheet();

  var lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);

    var dBlob = p.d || '';

    for (var i = 0; i < n; i++) {
      var chunk = dBlob.substr(i * 18, 18);
      if (chunk.length < 18) continue;
      var d = parseCompactRecord(chunk);

      // ★2026-07-25: 圧縮エンコードではMACを送らなくなったため、DeviceIDから
      // 疑似MAC（00-00-00-00-00-XX）を組み立てて従来通り「シート名編集」で
      // ルーティングする（LoRaの疑似MAC方式と統一。BLE運用時は「シート名編集」を
      // 実MACではなくこの疑似MAC形式で登録し直す必要がある）
      var macHex = ('0' + d.deviceId.toString(16)).slice(-2).toUpperCase();
      var mac = '00-00-00-00-00-' + macHex;

      var sheetName = getDeviceSheetNameByMac(ss, mac);
      if (!sheetName) {
        console.log('未登録デバイス MAC=' + mac + ' → スキップ（シート名編集に未登録）');
        continue;
      }
      var sheet = ss.getSheetByName(sheetName);
      if (!sheet) {
        console.log('シートが見つかりません: ' + sheetName);
        continue;
      }

      // 列構成（datebox1 と同一。圧縮エンコードでは送っていない項目
      // （FlexHour/FlexMin/RSSI/fw_version/CH5-12/レンジ）は空欄で書き込む）:
      // 受信日時, 計測日時, MAC, PktType, DeviceID, FlexHour, FlexMin,
      // BLE_RSSI, SIM, LTE-M_RSSI, XIAO_ID, SIM_IMEI, SD記録, 送信間隔(分), 受信台数, CH1〜CH12,
      // fw_version, ch1_range, ch2_range, ch3_range, ch4_range
      sheet.appendRow([
        new Date(), ts ? new Date(ts) : '', mac,
        PKT_TYPE_V303, d.deviceId,
        '', '',
        '', sim, csq,
        '', '', '', '', '',              // XIAO_ID,SIM_IMEI,SD記録,送信間隔(分),受信台数 → データ行では未使用
        d.ch[0], d.ch[1], d.ch[2], d.ch[3], '', '',
        '', '', '', '', '', '',
        '',
        '', '', '', '',
      ]);

      // 加工用データ（CH7〜CH12）の数式を1つ上の行からコピー
      // （相対参照は自動でその行にずれる。copyToはvalues指定していないので上書きされる）
      var lastRow = sheet.getLastRow();
      var prevRow = lastRow - 1;
      var ch7to12 = ['', '', '', '', '', '']; // フォールバック（数式が無い場合。圧縮エンコードにはCH7-12の生値がないため空欄）

      if (prevRow > DATA_HEADER_ROW) {
        sheet
          .getRange(prevRow, FORMULA_START_COL, 1, FORMULA_COL_COUNT)
          .copyTo(sheet.getRange(lastRow, FORMULA_START_COL, 1, FORMULA_COL_COUNT), {
            contentsOnly: false
          });

        // 数式の計算結果を読み戻す（アラート判定にはペイロードの生値ではなく計算後の値を使う）
        SpreadsheetApp.flush();
        Utilities.sleep(1500);
        ch7to12 = sheet.getRange(lastRow, FORMULA_START_COL, 1, FORMULA_COL_COUNT).getValues()[0];
      }

      var dataObj = {
        CH1: d.ch[0], CH2: d.ch[1], CH3: d.ch[2], CH4: d.ch[3], CH5: '', CH6: '',
        CH7: ch7to12[0], CH8: ch7to12[1], CH9: ch7to12[2], CH10: ch7to12[3], CH11: ch7to12[4], CH12: ch7to12[5],
        FlexHour: '', FlexMin: '',
        BLE_RSSI: '', 'LTE-M_RSSI': Number(csq),
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
