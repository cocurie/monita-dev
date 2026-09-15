// ================================
// Monita Flex (Sigfox) → Sigfox Backend Callback → GAS 受信スクリプト
// project03_ishikawa 専用（関西大学 石川先生案件）
//
// ================================
// バージョン対応表
// ================================
//   GASスクリプトバージョン: 2
//     - v1: 受領版（YCE版 CH1/CH3レンジ用のコピー。d6 のコメントが「CH3レンジ」のまま）
//     - v2: project03_ishikawa ファームに合わせて修正（2026-09-15）
//       * d1〜d6 のコメントを実ペイロード（µε / CH1レンジ / 気温℃×10）に修正
//       * 値を符号付き16bitとして解釈（コールバックが uint 設定でも負値を正しく戻す）
//       * 値が欠けている場合に 0 で埋めない（0µε と区別がつかなくなるため空欄にする）
//       * デバイスIDの照合で数値/文字列・大小文字の違いを吸収（数字だけのIDが無言で捨てられていた）
//       * 書き込み失敗を Logger だけで黙殺せず、管理者へ受信内容ごとメールする
//       * 未受信判定を 24h → 36h に変更（12時間間隔では1回の取りこぼしで警告が出てしまうため）
//
//   対応する子機ファーム:
//     - project03_ishikawa/ishikawa_sigfox_range/src/main.cpp（COMM_MODE_SIGFOX）
//       SLEEP_MINUTES=720（12時間に1回送信）, SAMPLES_PER_AVG=5, MEASURE_COUNT=5
//
// ================================
// Sigfox ペイロード（12バイト、int16 little-endian × 6）
// ================================
//   data1: CH1 ひずみ（µε）
//   data2: CH2 ひずみ（µε）
//   data3: CH3 ひずみ（µε）
//   data4: CH4 ひずみ（µε）
//   data5: CH1 レンジ（計測1回＝約2.5秒間の5平均値の max-min、µε）※12時間の変動幅ではない
//   data6: 基板温度（DS3231、℃×10）※外気温ではない。シート側で /10 すること
//
//   Sigfox Backend の Custom payload config:
//     data1::int:16:little-endian data2::int:16:little-endian data3::int:16:little-endian data4::int:16:little-endian data5::int:16:little-endian data6::int:16:little-endian
//
// ================================
// デバイスシート列構成
// ================================
//   A: デバイスID / B: 受信時刻 / C〜H: d1〜d6 / I: lqi / J〜AM: 数式（d7〜d12 は J〜O）
//
// ★デプロイID（Webhook URL）はこのファイルに書かない。
//   このリポジトリは公開されており、URLが分かれば誰でも偽データを書き込み・アラートメールを送らせられるため。
// ================================


// ================================
// 基本設定
// ================================
const SPREADSHEET_ID = '1oDW2gQEYT1UWmT2zH7mRP8UuSdIHeM6hL3M1zPC2EZ8';   //★スプレッドシートのリンクから貼り付け★


// アラート設定が始まる行番号（各デバイスシートの何行目から読むか）
const ALERT_START_ROW = 3;

// アラート設定の最大行数（3行目〜14行目の12行分）
const SETTING_ROWS = 12;

// 数式セルが始まる列番号（d1〜d6 は生データ=3〜8列目、lqi=9列目、d7〜d12 は数式=10列目〜）
const START_COL = 10;

// 数式コピー対象の列数（39列目まで数式があるため START_COL から 39 列目までの幅）
const FORMULA_COL_COUNT = 39 - START_COL + 1;

// アラートのクールダウン時間
// 送信間隔が12時間のため、0（クールダウンなし）でも同じアラートは1日最大2通。
// 送信間隔を短くして試験する場合は 10 * 60 * 1000（10分）等に変更する。
const ALERT_COOLDOWN_MS = 0;
// const ALERT_COOLDOWN_MS = 10 * 60 * 1000;

// データ未受信アラートの判定時間（36時間）
// ★子機の送信間隔（12時間）より必ず長くすること。24時間ちょうどだと、
//   Sigfox の取りこぼしが1回あっただけで（間隔が24時間＋数十秒になり）警告が出る。
//   36時間 = 1回の取りこぼしは許容し、2回連続で届かなければ警告。
const NO_DATA_LIMIT_MS = 36 * 60 * 60 * 1000;

// 管理者メールアドレス（未受信警告・書き込み失敗などシステム通知の送信先）
const ADMIN_MAILS = [
  "cocurie.kanri@gmail.com"
];


// ================================
// スプレッドシート取得
// ================================
// SPREADSHEET_ID で指定したスプレッドシートを開いて返す共通関数
function getSpreadsheet() {
  return SpreadsheetApp.openById(SPREADSHEET_ID);
}


// ================================
// 数値変換
// ================================
// ペイロードの int16 値を数値にする。
// - 値が無い・数値でない場合は ""（空欄）を返す。0 で埋めると実測の 0µε と区別できない。
// - 32767 を超える値は符号なしで解釈されたものとみなし、負値へ戻す
//   （コールバック設定が uint:16 になっていても -1µε が 65535 にならないように）。
function toInt16(value) {
  if (value === undefined || value === null || value === "") return "";
  var n = parseInt(value, 10);
  if (isNaN(n)) return "";
  if (n > 32767 && n <= 65535) n -= 65536;
  return n;
}


// ================================
// Webhook 受信本体
// ================================
// Sigfox Backend の Callback（HTTP POST, application/json）で自動的に呼ばれる。
//
// 受信 JSON の例（Callback の body 設定）:
//   { "device": "{device}", "time": {time},
//     "data1": {customData#data1}, "data2": {customData#data2}, "data3": {customData#data3},
//     "data4": {customData#data4}, "data5": {customData#data5}, "data6": {customData#data6},
//     "lqi": "{lqi}" }
function doPost(e) {

  // JSON パース
  var json   = JSON.parse(e.postData.contents);
  var device = json.device;
  var time   = new Date(json.time * 1000);  // Unix 秒 → JavaScript Date

  // センサ値取得
  var d1  = toInt16(json.data1);            // CH1 ひずみ（µε）
  var d2  = toInt16(json.data2);            // CH2 ひずみ（µε）
  var d3  = toInt16(json.data3);            // CH3 ひずみ（µε）
  var d4  = toInt16(json.data4);            // CH4 ひずみ（µε）
  var d5  = toInt16(json.data5);            // CH1 レンジ（µε）
  var d6  = toInt16(json.data6);            // 基板温度（℃×10）
  var lqi = json.lqi || "";                 // 電波品質（Sigfoxネットワーク付与: "Good"/"Average"/"Limit" 等）

  var ss = getSpreadsheet();

  // デバイスID からシート名を取得する
  // 「シート名編集」シートの A列=デバイスID、B列=シート名 を参照する
  // ★Sigfox のデバイスIDは16進文字列。数字だけのIDをセルに入力すると
  //   スプレッドシートが数値に変換するため、文字列化・大文字化してから比較する。
  function normalizeId(id) {
    return String(id).trim().toUpperCase();
  }

  function getDeviceSheetName(device) {
    const mapSheet = ss.getSheetByName("シート名編集");
    if (!mapSheet) return null;
    const last = mapSheet.getLastRow();
    if (last < 3) return null;
    const values = mapSheet.getRange(3, 1, last - 2, 2).getValues();
    const target = normalizeId(device);
    for (let i = 0; i < values.length; i++) {
      if (values[i][0] !== "" && normalizeId(values[i][0]) === target) return values[i][1];
    }
    return null;  // 登録されていないデバイスは無視
  }

  var sheetName = getDeviceSheetName(device);
  if (!sheetName) {
    Logger.log('未登録デバイスのため破棄: device=' + device);
    return;
  }

  var sheet = ss.getSheetByName(sheetName);
  if (!sheet) {
    Logger.log('シートが存在しないため破棄: device=' + device + ' sheet=' + sheetName);
    return;
  }

  // 排他ロック取得（複数デバイスの同時 POST による競合を防ぐ）
  var lock = LockService.getScriptLock();

  try {
    lock.waitLock(10000);  // 最大 10 秒待機

    // スプレッドシートへの行追加
    // 列構成: [デバイスID, 時刻, d1, d2, d3, d4, d5, d6, lqi]
    sheet.appendRow([device, time, d1, d2, d3, d4, d5, d6, lqi]);

    const lastRow = sheet.getLastRow();
    const prevRow = lastRow - 1;

    // 数式コピー: 前の行の数式（d7〜d12）を新しい行に引き継ぐ
    // contentsOnly: false = 値ではなく数式をコピー
    if (prevRow > 1) {
      sheet
        .getRange(prevRow, START_COL, 1, FORMULA_COL_COUNT)
        .copyTo(sheet.getRange(lastRow, START_COL, 1, FORMULA_COL_COUNT), {
          contentsOnly: false
        });
    }

    // 数式の再計算待ち（Google Sheets の数式計算は非同期のため待機が必要）
    SpreadsheetApp.flush();
    Utilities.sleep(2000);

    // 数式セルを一括取得（d7〜d12 の 6 セル分）
    var formulaValues = sheet.getRange(lastRow, START_COL, 1, 6).getValues()[0];

    // 2 秒待っても空の場合はさらに 2 秒待ってリトライ（1 回のみ）
    if (formulaValues[0] === "" || formulaValues[0] === null) {
      Logger.log("数式未計算 → 2秒後に再取得");
      Utilities.sleep(2000);
      formulaValues = sheet.getRange(lastRow, START_COL, 1, 6).getValues()[0];
    }

    // d7〜d12: スプレッドシートの数式で計算された加工後データ
    var d7  = formulaValues[0];
    var d8  = formulaValues[1];
    var d9  = formulaValues[2];
    var d10 = formulaValues[3];
    var d11 = formulaValues[4];
    var d12 = formulaValues[5];

    // デバッグ用ログ（GAS エディタの「実行ログ」で確認可能。不要なら削除可）
    Logger.log('受信: device=' + device +
      ' d1=' + d1 + ' d2=' + d2 + ' d3=' + d3 + ' d4=' + d4 +
      ' d5=' + d5 + ' d6=' + d6 + ' lqi=' + lqi +
      ' d7=' + d7 + ' d8=' + d8 + ' d9=' + d9 +
      ' d10=' + d10 + ' d11=' + d11 + ' d12=' + d12);

    // d1〜d12 をオブジェクトにまとめてアラート判定へ渡す
    const dataObj = { d1, d2, d3, d4, d5, d6, d7, d8, d9, d10, d11, d12 };
    checkAlertsForDeviceSheet(sheet, dataObj, device, time);

  } catch (err) {
    // ★12時間に1回しか届かないため、1件の取りこぼしが半日分の欠測になる。
    //   ログだけで黙殺せず、受信内容を添えて管理者へ通知する（手で追記できるように）。
    Logger.log('Lock or append error: ' + err);
    notifyAdminWriteError(err, device, sheetName, e.postData.contents);
  } finally {
    // 成功・失敗問わず必ずロックを解放する
    try { lock.releaseLock(); } catch (e) {}
  }
}


// 書き込み失敗を管理者へ通知する。通知自体の失敗で doPost を落とさない。
function notifyAdminWriteError(err, device, sheetName, rawBody) {
  try {
    MailApp.sendEmail({
      to:      ADMIN_MAILS.join(","),
      subject: "【書き込み失敗】デバイス " + device,
      body:    "受信データをスプレッドシートへ書き込めませんでした。\n"
             + "下記の受信内容をシートへ手で追記してください。\n\n"
             + "エラー: " + err + "\n"
             + "デバイスID: " + device + "\n"
             + "対象シート: " + sheetName + "\n"
             + "受信内容: " + rawBody + "\n",
      name:    "モニタリングシステム"
    });
  } catch (mailErr) {
    Logger.log('Admin notify error: ' + mailErr);
  }
}


// ================================
// アラート判定ロジック
// ================================
// デバイスシートのアラート設定（3行目〜14行目）を読み取り、
// 受信データが閾値を超えていればメールを送信する。
// ★d6 は ℃×10 のため、d6 に閾値を設定する場合は10倍の値で書く（例: 40℃ → 400）。
//   ℃で判定したい場合はシートの数式列（d7〜d12）で /10 した列をキーにする。
function checkAlertsForDeviceSheet(deviceSheet, dataObj, device, time) {

  if (!deviceSheet) return;

  // アラート設定行を全列分読み取る
  var lastCol = deviceSheet.getLastColumn();
  var values = deviceSheet
    .getRange(ALERT_START_ROW, 1, SETTING_ROWS, lastCol)
    .getValues();

  // ScriptProperties: 「最終アラート送信時刻」を保存するキーバリューストア
  // キー例: 'lastAlert_MON001_d1'
  var props = PropertiesService.getScriptProperties();

  for (var i = 0; i < values.length; i++) {

    var row       = values[i];
    var key       = row[0];             // A列: センサキー（例: "d1", "d7"）
    var threshold = parseFloat(row[1]); // B列: 閾値（絶対値で比較）
    var title     = row[2];             // C列: メール件名
    var body      = row[3];             // D列: メール本文
    var addresses = row.slice(4);       // E列以降: 送信先メールアドレス

    // 必須項目が空の行はスキップ
    if (!key || isNaN(threshold) || !title || !body) continue;

    // dataObj からセンサキーに対応する値を取得（例: key="d1" → dataObj.d1）
    // 欠測（空欄）は判定しない
    var sensorValue = dataObj[key];
    if (sensorValue === undefined || sensorValue === null || sensorValue === "") continue;

    // 閾値判定（正負どちらの超過でも検知するため絶対値で比較）
    if (Math.abs(sensorValue) > threshold) {

      // 有効なメールアドレスのみ抽出
      var toList = addresses.filter(function(m) {
        return m && m.toString().trim() !== "";
      });
      if (toList.length === 0) continue;

      // クールダウン判定
      // 同一デバイス・同一センサキーで ALERT_COOLDOWN_MS 以内の再送を防ぐ。
      var propKey = 'lastAlert_' + device + '_' + key;
      var lastTs  = parseInt(props.getProperty(propKey) || '0', 10);
      var now     = Date.now();

      if (now - lastTs < ALERT_COOLDOWN_MS) {
        var remainSec = Math.round((ALERT_COOLDOWN_MS - (now - lastTs)) / 1000);
        Logger.log('クールダウン中 [' + device + '/' + key + '] 残り' + remainSec + '秒 → スキップ');
        continue;
      }

      // 表示用の値を整形（数値なら小数4桁）
      var displayValue = (typeof sensorValue === "number")
        ? sensorValue.toFixed(4)
        : sensorValue;

      // メール送信
      try {
        MailApp.sendEmail({
          to:       toList.join(","),
          subject:  title,
          htmlBody: String(body).replace(/{{value}}/g, displayValue),  // {{value}} を実測値に置換
          name:     "ひずみセンサ通知システム"
        });

        // 送信成功 → 最終送信時刻を保存（クールダウン用）
        props.setProperty(propKey, now.toString());
        Logger.log('アラート送信 [' + device + '/' + key + '=' + displayValue + '] → ' + toList.join(','));

      } catch (err) {
        Logger.log('Mail send error: ' + err);
      }
    }
  }
}


// ================================
// 新規データ未受信チェック
// ================================
// 特定デバイスから NO_DATA_LIMIT_MS（36時間）以上データが届かない場合に管理者へ警告メールを送る。
// 電源断・通信障害の検知に使用する。
//
// ★この関数は時間主導トリガーを設定したときだけ動く（関数内に無効化スイッチは無い）。
// 有効化手順:
//   GAS エディタ → トリガー → この関数を 1 時間ごとに定期実行する設定を追加
function checkNoDataAlert() {

  var ss    = getSpreadsheet();
  var now   = Date.now();
  var props = PropertiesService.getScriptProperties();

  // 「シート名編集」シートから全デバイスのリストを取得
  var mapSheet = ss.getSheetByName("シート名編集");
  if (!mapSheet) return;

  var lastRow = mapSheet.getLastRow();
  if (lastRow < 3) return;

  var devices = mapSheet.getRange(3, 1, lastRow - 2, 2).getValues();

  for (var r = 0; r < devices.length; r++) {
    var deviceId  = devices[r][0];
    var sheetName = devices[r][1];
    if (!deviceId || !sheetName) continue;

    var sheet = ss.getSheetByName(sheetName);
    if (!sheet) continue;

    var lastDataRow = sheet.getLastRow();
    if (lastDataRow < 2) continue;

    // 最終データ受信時刻を取得（B列 = 時刻列）
    var lastTime = sheet.getRange(lastDataRow, 2).getValue();
    if (!(lastTime instanceof Date)) continue;

    var diff = now - lastTime.getTime();

    // 前回の未受信アラート送信時刻（二重送信防止）
    var propKey   = "lastNoDataAlert_" + deviceId;
    var lastAlert = parseInt(props.getProperty(propKey) || "0", 10);

    // 最終受信から NO_DATA_LIMIT_MS 以上 かつ 前回警告から NO_DATA_LIMIT_MS 以上 経過した場合に送信
    if (diff > NO_DATA_LIMIT_MS && now - lastAlert > NO_DATA_LIMIT_MS) {
      MailApp.sendEmail({
        to:      ADMIN_MAILS.join(","),
        subject: "【未受信警告】デバイス " + deviceId,
        body:    "以下のデバイスで" + (NO_DATA_LIMIT_MS / 3600000) + "時間以上データ受信がありません。\n\n"
               + "デバイスID: " + deviceId + "\n"
               + "対象シート: " + sheetName + "\n"
               + "最終受信時刻: " + lastTime + "\n\n"
               + "ご確認をお願いいたします。",
        name:    "モニタリングシステム"
      });
      props.setProperty(propKey, now.toString());
    }
  }
}
