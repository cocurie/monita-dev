// ================================
// Monita LoRa複数台テスト → GAS 受信スクリプト
//   対象: case01_Flex/test_sketches/22_lora_multi_child（子機・ダミーデータ）
//         ＋ case02_Gateway/firmware/gateway_v1.1（COMM_MODE_LORAビルド・受信/送信）
//
//   ★これは13台LoRa疎通テスト専用の最小GASです。
//     本番のproject07_NEXCO用GAS（case02_Gateway/gas/project07_nexco/Code.gs、
//     BLE版26B/台フォーマット）とは別物なので混同しないこと。
//     アラート判定・デバイス別シート振り分けは行わず、全台を1シートに素直に記録する。
// ================================
// Gateway(gateway_v1.1)が送るクエリ形式（buildBatchQuery/postBootInfoRow由来）
// ================================
//   データ行（row_type無し）:
//     q  = CSQ（1バイトを16進2文字。10進へは parseInt(q,16)。意味は開発メモ
//          [gateway_csq_signal_strength_notes] 参照）
//     n  = 台数
//     d  = 各台13バイト=26hex文字の連結。1台分の内訳:
//            [0-3]  Gateway RTC(DS3231)のUNIX時刻 epoch (uint32 LE)。
//                   RTC無し起動時は0（★2026-07-25追加。main.cpp:1300-1317参照）
//            [4]    DeviceID (1B)
//            [5-6]  CH1 (int16 LE)
//            [7-8]  CH2 (int16 LE)
//            [9-10] CH3 (int16 LE)
//            [11-12] CH4 (int16 LE)
//          ※子機ファーム(22)の元ペイロードは19B(PktType/FW/BATT/Hour/Min/Range含む)だが、
//            Gatewayが送信時にEpoch+DeviceID+CH1-4の13Bだけ抜き出して&d=に圧縮している。
//            BATT/FW/Range/RSSIはこのテストでは送られてこない。
//          ★このフォーマットはfirmware(gateway_v1.1)の実装に追従が必要。ズレると
//            DeviceIDが範囲外の値(0x00, 0xD4等)になる形で壊れた値が記録される
//            （2026-07-25、Epoch追加をGAS側が未追従で発生した実例あり）。
//            main.cppのbuildBatchQuery()のsnprintf/chunk長と必ず突き合わせること。
//
//   info行（起動確認、row_type=info）:
//     ts, sim, csq, xiao_id, sim_imei, sd, interval_min, devcount, gw_fw
// ================================
// スプレッドシート列構成（lora_test シート）
// ================================
//   A: 受信日時(サーバ)   B: 計測日時(Gateway RTC。RTC無しは空欄)   C: DeviceID(16進)
//   D: CH1   E: CH2   F: CH3   G: CH4   H: CSQ(10進)   I: 備考
// ================================

// ★スプレッドシートID。このスクリプトをスプレッドシートに紐付け（コンテナバインド）
//   している場合は空文字のままでよい（getActiveSpreadsheet()を使う）。
//   スタンドアロンスクリプトの場合は、記録先スプレッドシートのIDをここに設定する。
var SPREADSHEET_ID = '12VfgxPoRmpr9tkI1myzvIgvjqcDxERkVcQxXWzvYr0I';

// データ・info行の記録先シート名（無ければ自動生成する）
var SHEET_NAME = 'lora_test';

function getSpreadsheet() {
  if (SPREADSHEET_ID) return SpreadsheetApp.openById(SPREADSHEET_ID);
  return SpreadsheetApp.getActiveSpreadsheet();
}

// 記録先シートを取得（無ければヘッダ付きで作成）
function getOrCreateSheet() {
  var ss = getSpreadsheet();
  var sheet = ss.getSheetByName(SHEET_NAME);
  if (!sheet) {
    sheet = ss.insertSheet(SHEET_NAME);
    sheet.appendRow(['受信日時', '計測日時(RTC)', 'DeviceID', 'CH1', 'CH2', 'CH3', 'CH4', 'CSQ', '備考']);
  }
  return sheet;
}

// 符号付き16ビット（リトルエンディアン）を復号する
function int16le(lo, hi) {
  var v = (hi << 8) | lo;
  if (v >= 0x8000) v -= 0x10000;
  return v;
}

// 符号無し32ビット（リトルエンディアン）を復号する（Gateway RTCのUNIX epoch用）
function uint32le(b0, b1, b2, b3) {
  return (b0 + b1 * 0x100 + b2 * 0x10000) + b3 * 0x1000000;  // *演算は32bit符号あり左シフトの罠を避けるため乗算で合成
}

// &d= の1台分（26hex文字=13バイト）を {epoch, deviceId, ch:[CH1..4]} に復号する
// バイト順は firmware(gateway_v1.1) buildBatchQuery() の snprintf 引数順と必ず一致させること:
//   Epoch(4B LE) + DeviceID(1B) + CH1-4(各2B LE)
function parseLoraChunk(hex26) {
  var b = [];
  for (var i = 0; i < 13; i++) b.push(parseInt(hex26.substr(i * 2, 2), 16));
  return {
    epoch: uint32le(b[0], b[1], b[2], b[3]),
    deviceId: b[4],
    ch: [
      int16le(b[5], b[6]),
      int16le(b[7], b[8]),
      int16le(b[9], b[10]),
      int16le(b[11], b[12]),
    ],
  };
}

function doGet(e) {
  var p = (e && e.parameter) ? e.parameter : {};

  // ── info行（起動確認）──────────────────────────
  if (p.row_type === 'info') {
    console.log('[INFO] ts=' + p.ts + ' sim=' + p.sim + ' csq=' + p.csq +
      ' xiao_id=' + p.xiao_id + ' sim_imei=' + p.sim_imei +
      ' sd=' + p.sd + ' interval_min=' + p.interval_min +
      ' devcount=' + p.devcount + ' gw_fw=' + p.gw_fw);

    var infoSheet = getOrCreateSheet();
    infoSheet.appendRow([
      new Date(),   // A: 受信日時
      '',           // B: 計測日時(RTC)（info行は空欄）
      'GW',         // C: DeviceID欄にGateway識別子
      '', '', '', '',  // D-G: CH（info行は空欄）
      p.csq || '',  // H: CSQ
      'fw' + (p.gw_fw || '?') + ' xiao=' + (p.xiao_id || '') +
        ' imei=' + (p.sim_imei || '') + ' sd=' + (p.sd || '') +
        ' interval_min=' + (p.interval_min || '') + ' devcount=' + (p.devcount || ''),
    ]);
    return ContentService.createTextOutput('OK');
  }

  // ── データ行（子機の受信データ）────────────────
  var csq = p.q ? parseInt(p.q, 16) : '';
  var n   = parseInt(p.n || '1', 10);
  var dBlob = p.d || '';

  var sheet = getOrCreateSheet();

  var lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);

    for (var i = 0; i < n; i++) {
      var chunk = dBlob.substr(i * 26, 26);
      if (chunk.length < 26) continue;
      var d = parseLoraChunk(chunk);
      var idHex = ('0' + d.deviceId.toString(16)).slice(-2).toUpperCase();
      // epoch=0はRTC未初期化（main.cppのrtcAvailable=false時）を意味するため空欄にする
      var measuredAt = d.epoch > 0 ? new Date(d.epoch * 1000) : '';

      sheet.appendRow([
        new Date(),                                   // A: 受信日時(サーバ)
        measuredAt,                                   // B: 計測日時(Gateway RTC)
        '0x' + idHex,                                 // C: DeviceID
        d.ch[0], d.ch[1], d.ch[2], d.ch[3],           // D-G: CH1-4
        csq,                                          // H: CSQ(10進)
        '',                                           // I: 備考
      ]);
    }
  } catch (err) {
    console.log('Lock or append error: ' + err);
  } finally {
    try { lock.releaseLock(); } catch (e2) {}
  }

  return ContentService.createTextOutput('OK');
}
