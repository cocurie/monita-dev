// ================================
// Monita — doPost(JSON) 受信 ＋ LoRaダウンリンク 統合版
// ================================
// ★このファイルの位置づけ
//   アイペック案件（project13_ipec）用に、以下の2つを1本にまとめたもの。
//     ① 「シート名編集」方式の DeviceID 別シート振り分け（新テンプレート）
//     ② LoRaダウンリンク一式（動作実績のある GAS から移植）
//   さらに ③ として **doPost(JSON) によるデータ受信**を新規に追加している。
//
// ★スタンドアロン前提
//   script.google.com で新規作成し、スプレッドシートには紐付けない。
//   SpreadsheetApp.getActiveSpreadsheet() は使えないので必ず ID で開く。
//
// ================================
// GET(URLクエリ) から POST(JSON) へ移行した理由
// ================================
//   従来の Gateway は AT+HTTPTOFS の GET でデータを送っており、
//   URL 長 512 バイトの制約に収めるため情報を捨てていた
//   （計測日時・BATT・FWVersion・Range・RSSI は送っていなかった）。
//
//   Gateway を AT+CAOPEN（生TCP+SSL）へ統一したことでボディを送れるようになり、
//   **512 バイト制限が消え、捨てていた項目を全部載せられる**ようになった。
//   （2026-09-22 実機確認: CAOPEN で Drive GET / InfluxDB POST とも成功）
//
//   これに伴い、HEX ペイロードを手で切り出す各種パーサー
//   （parsePayload / parsePayloadV303 / parseNEXCORecord / parseGatewayRecord）は
//   **すべて不要になったため本ファイルには含めていない**。JSON.parse() で足りる。
//
//   doGet は残してあるが、**制御系アクション専用**（ダウンリンク・リモートコマンド）。
//   テレメトリの受信口は doPost だけ。

// ================================
// ★ 案件ごとに書き換える箇所（03_template 雛形 2026-09-23）
// ================================
//   1. SPREADSHEET_ID          … 案件のスプレッドシート（このすぐ下）
//   2. DOWNLINK_DRIVE_FILE_IDS … 群ごとの Drive テキストファイル（このすぐ下）。Gateway の
//                                project_config.h の DOWNLINK_DRIVE_FILE_ID と同じ値にする
//   3. CMD_TOKEN               … 案件ごとに別の値にする
//   4. TEMPLATE_SHEET_NAME     … 新しい子機シートを作るときの複製元シート名（既定: 雛形）
//   5. ADMIN_MAILS             … データ未受信アラートの宛先（空だと送られない）
//   6. NO_DATA_LIMIT_MS        … 何時間データが来なければ未受信とみなすか
//   7. GATEWAY_DEVICE_ID / GATEWAY_GROUP … Gateway の project_config.h の GATEWAY_NAME / GATEWAY_GROUP_ID と同じ値
//   閾値アラートの条件・閾値・宛先はコードではなく、各子機シートの dateKey 欄で設定する。
//
//   コード以外の設定手順（デプロイ・トリガー・Drive ファイルの共有など）は
//   03_template/README.md のチェックリストを参照。
//
// ================================
// 受信する JSON（Gateway → doPost）
// ================================
//   {"gw":"<GATEWAY_NAME>","csq":17,"d":[{"id":14,"t":"2026-09-23 12:05:00",
//     "ch":[-2346,null,-7206],"batt":381,"rssi":-41,"fw":11}, ...]}
//   ・ch の要素数は子機ごとに違ってよい（子機が送ってきたチャネル数そのまま）。CH1, CH2, … 列に入る
//   ・null は欠測（センサー読み取り失敗・未使用）。シートでは空欄になる
//   ・t は子機の時刻が無効なときは省略される（計測日時が空欄になる）
//   ・rx は Gateway が LoRa で受信した時刻（Gateway FW18〜）。Gateway は子機ごとに数件ためて
//     まとめて送るので、同じ子機が複数行になり「受信日時」（GAS の時刻）は全行同じになる
//
// ================================
// バージョン対応表（ペイロード形式）  ★CLAUDE.md §6: ペイロード形式を変えたらここを更新する
// ================================
//   子機 → Gateway（LoRa）
//     0x04 … 19バイト固定・4ch（v3.10_lora FW10 / v3.20 / firmware_child）。欠測・時刻無効の表現なし
//     0x06 … 可変長 8+2n バイト・n ch（03_template/child FW11〜）。欠測=0x8000、時刻無効=0xFF
//   Gateway → GAS（JSON）
//     03_template/gateway FW15〜: ch 配列は子機のチャネル数どおり。0x06 の欠測は null
//     firmware_gateway FW14 以前: ch は常に4要素（欠測の区別なし）


// ╔════════════════════════════════════════════════════════════════════════╗
// ║  ★★★ 最初にここを書き換える（必須） ★★★                              ║
// ║                                                                        ║
// ║  別のスプレッドシート／案件へコピーしたら、必ず SPREADSHEET_ID を       ║
// ║  そのスプレッドシートのIDへ書き換えること。                            ║
// ║                                                                        ║
// ║  【書き換えを忘れるとどうなるか】                                      ║
// ║  openById() が例外を投げる。GASは例外時に HTTP 200 で HTMLエラーページ  ║
// ║  を返すため、Gateway側は「送信成功」と表示してしまう。                  ║
// ║  シートには1行も入らないのにログは正常に見える、という非常に気づき      ║
// ║  にくい壊れ方をする（2026-08-30に実際に発生し、丸一日気づけなかった）。 ║
// ╚════════════════════════════════════════════════════════════════════════╝
const SPREADSHEET_ID = '1FX80YVT7hZvnSxnlzJp7Pfez4plUKVgHUqujiSxy-rM';

// ╔════════════════════════════════════════════════════════════════════════╗
// ║  ★★★ ダウンリンクを使う群ごとに、Driveファイルを用意して登録する ★★★  ║
// ║                                                                        ║
// ║  【なぜDrive経由なのか】GAS Web App の応答は Transfer-Encoding:chunked  ║
// ║  で Content-Length が付かない。Gateway 側はこれを読めないため、         ║
// ║  Gateway が読む本文だけを Drive のファイルへ逃がす。Drive は            ║
// ║  Content-Length 付きで配信されるため確実に取得できる                    ║
// ║  （2026-09-22 実機確認済み: CAOPEN で 200/13バイト取得成功）。          ║
// ║                                                                        ║
// ║  【用意のしかた】群ごとにテキストファイルを1つ Drive に作り、共有を     ║
// ║  「リンクを知る全員／閲覧者」にして、URLの /d/ と /view の間のIDを      ║
// ║  下に登録する。中身は空でよい（GASが上書きする）。                      ║
// ║                                                                        ║
// ║  ★★ Googleドキュメント／スプレッドシートは使えない ★★                  ║
// ║  Gateway が読む drive.usercontent.google.com/download は、              ║
// ║  アップロードされた実ファイル(blob)しか配信できない。ネイティブの        ║
// ║  Googleドキュメントを指定すると HTTP 500・本文0バイトが返る              ║
// ║  （2026-09-22に実際に踏んだ）。必ず .txt をアップロードすること。        ║
// ║                                                                        ║
// ║  ★★ 検証用には本番とは別のファイルを作ること ★★                       ║
// ║  本番の群0ファイル(1bulwGZ...)をそのまま使うと、稼働中の Gateway が     ║
// ║  読む内容を書き換えてしまい、nonce不一致で予約が破棄される。            ║
// ╚════════════════════════════════════════════════════════════════════════╝
const DOWNLINK_DRIVE_FILE_IDS = {
  // ★2026-09-24 同じ場所で別構成（群0）の検証が動いているため、こちらを群1へ移した。ファイルは検証用のものをそのまま使う
  1: '16cKUzyfeTFDsa6i-Xc9i7GQSQpXir1Sy',  // 群1（検証用 monita_downlink_group0_ipec_test.txt）
  // 1: 'ここに群1用のファイルID',
};

// リモートコマンド（set_cmd）の認証トークン。Gateway 側の GW_CMD_TOKEN と揃える。
const CMD_TOKEN = 'monita-gw-cmd-2026';

// ★2026-09-23 Gateway の送信間隔をシートから変えるための識別子（Gateway FW17〜）。
//   Gateway の project_config.h の GATEWAY_NAME / GATEWAY_GROUP_ID と同じ値にする。
const GATEWAY_DEVICE_ID = 'ipec_gw';
const GATEWAY_GROUP = 1;   // ★2026-09-24 0→1（同じ場所の別構成の検証が群0を使っているため）


// ================================
// シート定義
// ================================
// 「シート名編集」に登録の無い DeviceID のデータは、このシートへフォールバックする
const DATABOX_SHEET_NAME = 'databox';

// databox・振り分け先シート共通の見出し行（新規シート作成時にこれを入れる）
//
// ★doPost は「ヘッダー名で列を探して入れる」方式なので、
//   列を足しても GAS を直す必要がない。順番を入れ替えても追従する。
//   逆に、見出し文字列を変えると入らなくなるので注意。
//
// ★BATT(mV) と FW は、GET時代には送れていなかった項目。
//   不要なら列ごと消してよい（doPost 側は黙って無視する）。
const DATABOX_HEADER = [
  '受信日時', 'Gateway受信日時', '計測日時', 'DeviceID', 'CH1', 'CH2', 'CH3', 'CH4', 'CH5', 'CH6', 'CH7', 'CH8',
  'CH9', 'CH10', 'CH11', 'CH12', 'CH13', 'CH14', 'CH15', 'CSQ', 'LoRa RSSI',
  'BATT(mV)', 'FW',
];

// ================================
// アラート設定（デバイスシート上段の「dateKey方式」）
// ================================
// ★実際のテンプレート（シート 0002）の構造に合わせている。
//
//   1行目   アラートメール通知設定
//   2行目   dateKey | 条件 | 閾値 | メールタイトル | メール本文 | mail-1〜mail-20
//   3行目〜 d1, d2, … d25 の設定行
//   （空行）
//   N-1行目 閾値datekey | d1 | d2 | … ← **どのdateKeyがどの列に対応するか**の対応行
//   N行目   クラウド受信時刻 | … | CH1 | CH2 | …          ← データ見出し
//   N+1行目〜 データ
//
// ★この「対応行」があるのがこの設計の要点。
//   設定本体は縦に並べたまま、dateKey→列 の対応を1行で表現できるので、
//   ・閾値・タイトル・本文・宛先20件が横に広がらない
//   ・生データ列（CH1〜15）と補正用列（d17〜d25）を同じ仕組みで扱える
//   という2つを同時に満たせる。
//
//   d16 は BATT(mV) に割り当てられている（2026-09-22、シート0002で確定）。

// 設定ブロック内の列位置（dateKey列からの相対）
const ALERT_COL_DATEKEY   = 0;  // dateKey
const ALERT_COL_CONDITION = 1;  // 条件（絶対値/上回る/下回る。空欄は絶対値）
const ALERT_COL_THRESHOLD = 2;  // 閾値
const ALERT_COL_TITLE     = 3;  // メールタイトル
const ALERT_COL_BODY      = 4;  // メール本文（{{value}} が実測値に置換される）
const ALERT_COL_MAIL_FROM = 5;  // mail-1 以降
const ALERT_MAIL_COUNT    = 10; // mail-1 〜 mail-10（2026-09-23 に20→10。Q列からダウンリンク設定欄を置くため）

// レイアウト検出に使う目印
const ALERT_HEADER_KEYWORD = 'dateKey';       // 設定ブロックの見出し行
const ALERT_MAPROW_KEYWORD = '閾値datekey';   // dateKey→列 の対応行
const ALERT_SETTING_MAX_ROWS = 60;            // 設定行の探索上限
// ★2026-09-24 設定欄は10行に集約し、監視対象はプルダウンで選ぶ。
const ALERT_SETTING_ROWS = 10;
const ALERT_DEFAULT_BODY = '現在の値：{{value}}';
// ★2026-09-24 設定10行の直後に置く案内の見出し（再実行時の目印）。
const ALERT_GUIDE_KEYWORD = 'アラートメールの設定（使い方）';

// レイアウト検出の探索範囲（見出し行を探す最大行数）
const LAYOUT_PROBE_ROWS = 60;

// 新規デバイスシートは、このシートを複製して作る。
// ★構造（設定ブロック・対応行・書式）を手で組み立てるより、実物を複製するほうが確実。
//   複製後はデータ行だけを消す（設定と見出しは残す）。
const TEMPLATE_SHEET_NAME = '雛形';   // 2026-09-23 に 0002 を「雛形」へ改名

// ★見出し名のゆらぎ吸収。
//   テンプレート(0002)と databox で見出し文字列が違うため、別名で引けるようにする。
//   例: 'クラウド受信時刻' と '受信日時'、'LTE-M RSSI CSQ …' と 'CSQ'。
//   完全一致 → 前方一致 の順で探す。
const FIELD_ALIASES = {
  '受信日時':   ['受信日時', 'クラウド受信時刻'],
  'Gateway受信日時': ['Gateway受信日時'],   // ★2026-09-23 Gateway FW18〜。列が無いシートでは黙って捨てる
  '計測日時':   ['計測日時', '計測時刻'],
  'DeviceID':   ['DeviceID'],
  'CSQ':        ['CSQ', 'LTE-M RSSI'],
  'LoRa RSSI':  ['LoRa RSSI'],
  'BATT(mV)':   ['BATT(mV)', 'BATT'],
  'FW':         ['FW'],
};

// 見出し行から、論理名に対応する列インデックス（0起点）を返す。無ければ -1。
// ★CH1〜CH15 は前方一致にすると CH1 が CH10〜CH15 にも当たるため、完全一致だけで引く。
function findColumnIndex_(header, logicalName) {
  const candidates = FIELD_ALIASES[logicalName] || [logicalName];
  for (let ci = 0; ci < candidates.length; ci++) {
    const exact = header.indexOf(candidates[ci]);
    if (exact >= 0) return exact;
  }
  if (/^CH\d+$/.test(logicalName)) return -1;   // CH系は完全一致のみ
  for (let ci = 0; ci < candidates.length; ci++) {
    for (let h = 0; h < header.length; h++) {
      if (String(header[h]).indexOf(candidates[ci]) === 0) return h;
    }
  }
  return -1;
}

// 同じ項目で連続発報しないための待ち時間。
// 【検証時】0 にすると毎回送信される
const ALERT_COOLDOWN_MS = 60 * 60 * 1000;  // 60分

// データ未受信アラートの判定時間。
// ★1時間間隔で運用する場合、1時間にすると正常でも誤検知する。
//   1サイクル分＋余裕をみて3時間を既定とする。
const NO_DATA_LIMIT_MS = 3 * 60 * 60 * 1000;  // 3時間

// 未受信アラートの通知先（閾値アラートは各シートの mail-1〜10 を使う）
const ADMIN_MAILS = [
  // 'monitor@example.com',
];

function getSpreadsheet() {
  return SpreadsheetApp.openById(SPREADSHEET_ID);
}


// ================================
// 「シート名編集」シート方式（DeviceID別シート振り分け）
// ================================
// 列構成:
//   A: DeviceID(10進)   … 数値。子機のDeviceIDそのもの
//   B: DeviceID(16進)   … 表示用の参考列。読み込み処理では見ない
//   C: シート名         … 書き込み先のシート名
//   D: 有効無効         … "有効" の行だけをマッピングに採用する
//   E: 備考             … 読み込み処理では見ない
//
// ★キャッシュする理由: doPost() は受信のたびに呼ばれる高頻度処理。毎回スプレッドシートを
//   読みに行くとレイテンシが増え、Gateway 側のタイムアウトにも影響し得る。
//   5分だけキャッシュし、「直したのに反映されない」が起きても最大5分で収まるようにする。
const SHEET_NAME_MAP_SHEET_NAME = 'シート名編集';
const SHEET_NAME_MAP_CACHE_KEY = 'device_sheet_map_v1';
const SHEET_NAME_MAP_CACHE_TTL_SEC = 300; // 5分

function loadDeviceSheetMapFromSheet() {
  const ss = getSpreadsheet();
  const sheet = ss.getSheetByName(SHEET_NAME_MAP_SHEET_NAME);
  if (!sheet) {
    Logger.log('「' + SHEET_NAME_MAP_SHEET_NAME + '」シートが見つかりません。databoxへフォールバックします');
    return {};
  }
  const lastRow = sheet.getLastRow();
  if (lastRow < 2) return {};

  const rows = sheet.getRange(2, 1, lastRow - 1, 4).getValues();
  const map = {};
  rows.forEach(function (row) {
    const deviceId = row[0];
    const sheetName = row[2];
    const enabled = row[3];
    if (deviceId === '' || sheetName === '') return;
    if (enabled !== '有効') return;
    map[Number(deviceId)] = String(sheetName);
  });
  return map;
}

function getDeviceSheetMap() {
  const cache = CacheService.getScriptCache();
  const cached = cache.get(SHEET_NAME_MAP_CACHE_KEY);
  if (cached !== null) return JSON.parse(cached);
  const map = loadDeviceSheetMapFromSheet();
  cache.put(SHEET_NAME_MAP_CACHE_KEY, JSON.stringify(map), SHEET_NAME_MAP_CACHE_TTL_SEC);
  return map;
}

// ★「シート名編集」を直したのに5分待てない、というときだけ手動実行する。
function clearDeviceSheetMapCacheOnce() {
  CacheService.getScriptCache().remove(SHEET_NAME_MAP_CACHE_KEY);
  Logger.log('キャッシュを削除しました。次回アクセス時に再読み込みされます');
}

// ================================
// シートのレイアウト検出
// ================================
// ★行位置を固定値で持たない。テンプレート(0002)・databox・今後の派生シートで
//   見出し行の位置が違うため、目印の文字列から毎回探す。
//   固定値にすると、シートを1行足しただけで静かに壊れる。

// データ見出し行と、時刻見出しがある列を探す。
// 戻り値: { row, col }（col は1起点）。見つからなければ { row: 1, col: 1 }。
//
// ★A列だけを見てはいけない。テンプレート(0002)は A列を空けて B列から始める形に
//   変更されており（2026-09-22 確認）、A列固定だと見出しを見つけられずに
//   「データは入るがアラートが丸ごと効かない」という気づきにくい壊れ方をする。
//   先頭数列を横に探すことで、列を1つずらしても追従できるようにする。
const HEADER_PROBE_COLS = 5;

function findHeaderPos_(sheet) {
  const nRow = Math.min(LAYOUT_PROBE_ROWS, sheet.getMaxRows());
  const nCol = Math.min(HEADER_PROBE_COLS, sheet.getMaxColumns());
  const probe = sheet.getRange(1, 1, nRow, nCol).getValues();
  const names = FIELD_ALIASES['受信日時'];
  for (let r = 0; r < probe.length; r++) {
    for (let c = 0; c < probe[r].length; c++) {
      if (names.indexOf(String(probe[r][c]).trim()) >= 0) {
        return { row: r + 1, col: c + 1 };
      }
    }
  }
  return { row: 1, col: 1 };
}

function findHeaderRow_(sheet) {
  return findHeaderPos_(sheet).row;
}

// アラート設定のレイアウトを検出する。
// 戻り値: { settingHeaderRow, keyCol, mapRow, dataHeaderRow } / 設定が無ければ null
function findAlertLayout_(sheet) {
  const dataHeaderRow = findHeaderRow_(sheet);
  if (dataHeaderRow < 3) return null;   // 設定ブロックが入る余地が無い＝設定なしシート

  const nRow = Math.min(LAYOUT_PROBE_ROWS, dataHeaderRow);
  const nCol = Math.min(10, sheet.getMaxColumns());
  const probe = sheet.getRange(1, 1, nRow, nCol).getValues();

  let settingHeaderRow = -1, keyCol = -1, mapRow = -1;
  for (let r = 0; r < probe.length; r++) {
    for (let c = 0; c < probe[r].length; c++) {
      const v = String(probe[r][c]).trim();
      if (v === ALERT_HEADER_KEYWORD && settingHeaderRow < 0) {
        settingHeaderRow = r + 1; keyCol = c + 1;
      }
      if (v === ALERT_MAPROW_KEYWORD) mapRow = r + 1;
    }
  }
  if (settingHeaderRow < 0 || mapRow < 0) return null;
  return { settingHeaderRow: settingHeaderRow, keyCol: keyCol,
           mapRow: mapRow, dataHeaderRow: dataHeaderRow };
}

// DeviceIDに対応するシートを返す（無ければテンプレートを複製して作る）。
function getSheetForDevice(deviceId) {
  const map = getDeviceSheetMap();
  const name = map[Number(deviceId)] || DATABOX_SHEET_NAME;
  const ss = getSpreadsheet();
  let sheet = ss.getSheetByName(name);
  if (sheet) return sheet;

  // ★テンプレートを複製する。設定ブロック・対応行・書式をそのまま引き継げる。
  //   手で組み立てると、テンプレートを直したときに追従できない。
  const tpl = ss.getSheetByName(TEMPLATE_SHEET_NAME);
  if (tpl) {
    sheet = tpl.copyTo(ss).setName(name);
    // ★2026-09-24 コピーでは保護が引き継がれないため、自動欄の警告を付け直す。
    UI_protectDownlink_(sheet);
    UI_protectAlertGuide_(sheet);
    const hr = findHeaderRow_(sheet);
    const last = sheet.getLastRow();
    if (last > hr) {
      sheet.getRange(hr + 1, 1, last - hr, sheet.getLastColumn()).clearContent();
    }
    Logger.log('テンプレート「' + TEMPLATE_SHEET_NAME + '」を複製して作成: ' + name +
               '（★閾値はテンプレートの値のままなので見直すこと）');
  } else {
    // テンプレートが無い場合の保険。見出しだけ作る（アラートは効かない）
    sheet = ss.insertSheet(name);
    sheet.appendRow(DATABOX_HEADER);
    sheet.setFrozenRows(1);
    Logger.log('テンプレートが見つからないため見出しのみで作成: ' + name);
  }
  return sheet;
}

// データ行を追記する。
// ★設定ブロックは見出しより上にあるので appendRow がそのまま使える。
function writeDataRow_(sheet, row) {
  sheet.appendRow(row);
}

// ================================
// ★ doPost — Gateway から JSON を受け取ってシートへ書き込む（新規）
// ================================
// 期待する JSON:
//   {
//     "gw":  "gw_xxxxxxxx",          // Gateway個体ID（任意）
//     "csq": 21,                     // LTE-M 電波強度
//     "d": [                         // 子機ごとの配列。台数制限なし
//       { "id":   14,                        // DeviceID(10進)
//         "t":    "2026-09-22 12:02:00",     // 計測日時（子機のRTC由来）
//         "ch":   [-2345, -1665, -7206, 0],  // CH1..CHn（任意の長さ）
//         "batt": 3300,                      // 電池電圧(mV)
//         "rssi": -25,                       // LoRa RSSI(dBm)
//         "fw":   10 }                       // 子機ファームバージョン
//     ]
//   }
//
// ★列はヘッダー名で探して入れる。ヘッダーに無い項目は黙って捨てる。
//   後から列を足すだけで自動的に入るようになる（GASの修正不要）。
function doPost(e) {
  if (!e || !e.postData || !e.postData.contents) {
    return ContentService.createTextOutput('error: no body');
  }

  let payload;
  try {
    payload = JSON.parse(e.postData.contents);
  } catch (err) {
    console.error('[doPost] JSON parse 失敗: ' + err);
    logInvalidPayload_('JSONとして解釈できません', String(e.postData.contents));
    return ContentService.createTextOutput('error: bad json');
  }

  // ★2026-09-24 FW24: 診断ログには d が無いので、テレメトリの検証より先に受け付ける。
  if (payload && payload.type === 'gw_log') {
    if (!Array.isArray(payload.lines)) {
      logInvalidPayload_('gw_log の lines が配列ではありません', String(e.postData.contents));
      return ContentService.createTextOutput('error: bad log');
    }
    const lock = LockService.getScriptLock();
    try {
      lock.waitLock(10000);
      const sheet = getGatewayEventLogSheet_();
      const now = new Date();
      payload.lines.forEach(function (line) {
        const r = sheet.getLastRow() + 1;
        sheet.getRange(r, 3, 1, 2).setNumberFormat('@');
        sheet.getRange(r, 1, 1, 4).setValues([[now, payload.gw || '', String(payload.run || ''), String(line)]]);
      });
      return ContentService.createTextOutput('ok:' + payload.lines.length);
    } catch (err) {
      console.error('[GW-LOG] 記録失敗: ' + err);
      return ContentService.createTextOutput('error: log write');
    } finally {
      try { lock.releaseLock(); } catch (e2) {}
    }
  }

  const devices = payload.d || [];
  if (!Array.isArray(devices) || devices.length === 0) {
    logInvalidPayload_('d が配列でない、または空です', String(e.postData.contents));
    return ContentService.createTextOutput('error: no devices');
  }

  const csq = (payload.csq === undefined) ? '' : payload.csq;
  const now = new Date();
  let written = 0, dup = 0;
  // ★2026-09-24 FW21: キャッシュは消えることがあるため、同じPOST内とシートの最後の300行でも確認する。
  //   rx が無い（Gateway の時計が無効）ものは判定できないので書く。
  const dedupCache = CacheService.getScriptCache();
  const DEDUP_TTL_SEC = 6 * 60 * 60;
  function recordKey(dev) { return (dev && dev.rx) ? (dev.id + '|' + String(dev.rx).replace(/\D/g, '')) : null; }
  function cacheKey(key) { return 'rx_' + key.replace('|', '_'); }

  // ★複数の Gateway が同時に投げてきたときの追記競合を防ぐ。
  const lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);

    const ss = getSpreadsheet();
    const keys = devices.map(recordKey).filter(function (k) { return k !== null; }).map(cacheKey);
    const cached = keys.length ? dedupCache.getAll(keys) : {};
    const seen = new Set(), sheetData = {}, writtenKeys = {};
    devices.forEach(function (dev) {
      if (dev === null || dev.id === undefined) return;
      const key = recordKey(dev);
      if (key && (seen.has(key) || cached[cacheKey(key)])) { dup++; return; }
      if (key) seen.add(key);
      const sheet = getSheetForDevice(dev.id);
      const sheetId = sheet.getSheetId();
      if (!sheetData[sheetId]) {
        const headerRow = findHeaderRow_(sheet);
        const header = sheet.getRange(headerRow, 1, 1, sheet.getLastColumn()).getValues()[0];
        const existing = new Set();
        const idCol = findColumnIndex_(header, 'DeviceID'), rxCol = findColumnIndex_(header, 'Gateway受信日時');
        const first = Math.max(headerRow + 1, sheet.getLastRow() - 299);
        const count = sheet.getLastRow() - first + 1;
        if (idCol >= 0 && rxCol >= 0 && count > 0) {
          const ids = sheet.getRange(first, idCol + 1, count, 1).getValues();
          const times = sheet.getRange(first, rxCol + 1, count, 1).getValues();
          times.forEach(function (row, i) {
            const v = row[0];
            const stamp = v instanceof Date ? Utilities.formatDate(v, ss.getSpreadsheetTimeZone(), 'yyyyMMddHHmmss')
                                           : String(v).replace(/\D/g, '');
            if (stamp) existing.add(ids[i][0] + '|' + stamp);
          });
        }
        sheetData[sheetId] = { header: header, existing: existing };
      }
      const data = sheetData[sheetId];
      if (key && data.existing.has(key)) { dup++; return; }
      const header = data.header;
      const row = new Array(header.length).fill('');

      // ★見出し名のゆらぎを吸収して列を探す（'受信日時' と 'クラウド受信時刻' 等）。
      //   完全一致だけにすると、テンプレート(0002)側の長い見出しに入らない。
      function put(name, value) {
        const i = findColumnIndex_(header, name);
        if (i >= 0 && value !== undefined && value !== null) row[i] = value;
      }

      put('受信日時', now);
      put('Gateway受信日時', dev.rx || '');   // Gateway FW18〜: まとめて送るので1件ごとの受信時刻
      put('計測日時', dev.t || '');
      put('DeviceID', dev.id);
      (dev.ch || []).forEach(function (v, i) { put('CH' + (i + 1), v); });
      put('CSQ', csq);
      put('LoRa RSSI', dev.rssi);
      put('BATT(mV)', dev.batt);
      put('FW', dev.fw);

      writeDataRow_(sheet, row);
      written++;
      if (key) writtenKeys[cacheKey(key)] = '1';

      // ★閾値アラートは書き込みの「後」に評価する。
      //   先に評価してメール送信で例外が出ると、データそのものが残らないため。
      try {
        checkAlertsForDeviceSheet_(sheet, dev.id, header, row);
      } catch (alertErr) {
        console.error('[ALERT] 判定中にエラー（データ書き込みは完了済み）: ' + alertErr);
      }
    });
    // ★FW21: シートへの書き込みを確定してからキャッシュに載せる。途中失敗時は既存行で拾う。
    SpreadsheetApp.flush();
    if (Object.keys(writtenKeys).length) dedupCache.putAll(writtenKeys, DEDUP_TTL_SEC);
  } catch (err) {
    console.error('[doPost] 書き込み失敗: ' + err);
    return ContentService.createTextOutput('error: ' + err);
  } finally {
    try { lock.releaseLock(); } catch (e2) {}
  }

  // ★Gateway FW20〜はこの本文を読んで成否を判定する（ok:<書いた件数> dup:<重複で捨てた件数>）。形式を変えないこと。
  return ContentService.createTextOutput('ok:' + written + (dup ? ' dup:' + dup : ''));
}

// 受信内容が壊れている場合の記録。本体は残さず、調査に必要な長さと先頭だけ保存する。
// ★2026-09-24 長さと末尾200文字も残す。2026-09-23 22:26 の破損は先頭200文字が正常で、
//   「後ろが途中で切れた」のか「後ろに別の文字（LTE モジュールへ送った AT コマンド等）が混ざった」のか
//   区別できなかったため。先頭に = や + があると数式とみなされるので、' を付けて文字列として書く。
const INVALID_PAYLOAD_HEADER = ['受信日時', '理由', '先頭200文字', '本文の長さ', '末尾200文字'];
function logInvalidPayload_(reason, body) {
  console.error('[INVALID-PAYLOAD] ' + reason);
  try {
    const ss = getSpreadsheet();
    const sheet = ss.getSheetByName('invalid_payload_log') || ss.insertSheet('invalid_payload_log');
    if (sheet.getLastRow() === 0) {
      sheet.appendRow(INVALID_PAYLOAD_HEADER);
      sheet.setFrozenRows(1);
    } else if (sheet.getLastColumn() < INVALID_PAYLOAD_HEADER.length) {
      sheet.getRange(1, 1, 1, INVALID_PAYLOAD_HEADER.length).setValues([INVALID_PAYLOAD_HEADER]);   // 旧形式のシートに列を足す
    }
    const text = String(body || '');
    const tail = text.length > 200 ? text.substr(text.length - 200) : text;
    sheet.appendRow([new Date(), reason, "'" + text.substr(0, 200), text.length, "'" + tail]);
  } catch (err) {
    console.error('[INVALID-PAYLOAD] ログ記録失敗: ' + err);
  }
}


// ================================
// 閾値アラート（dateKey 方式）
// ================================
// 設定ブロック（縦）と、dateKey→列 の対応行を突き合わせて判定する。
//
//   設定:  dateKey | 条件 | 閾値 | メールタイトル | メール本文 | mail-1〜20
//   対応行: 閾値datekey | d1 | d2 | … ← d1 が CH1 の列の上に来る
//
// ★条件（2026-09-22 追加）
//   絶対値（空欄も同じ） … |値| > 閾値。ひずみのように正負どちらにも振れる量
//   上回る               … 値 > 閾値
//   下回る               … 値 < 閾値。**電池電圧の低下（d16 = BATT(mV)）**など
//
//   旧実装は絶対値固定だったため、電池切れの予兆を捕まえられなかった。
//   1年間の無人運用では「下回る」がいちばん実用的になる。
function checkAlertsForDeviceSheet_(sheet, deviceId, dataHeader, dataRow) {
  const layout = findAlertLayout_(sheet);
  if (!layout) return;   // 設定ブロックが無いシート（databox 等）は何もしない

  const nCol = sheet.getLastColumn();

  // dateKey → 列インデックス（0起点）の対応を作る
  const mapVals = sheet.getRange(layout.mapRow, 1, 1, nCol).getValues()[0];
  const keyToCol = {};
  for (let c = 0; c < mapVals.length; c++) {
    const k = String(mapVals[c]).trim();
    if (/^d\d+$/.test(k)) keyToCol[k] = c;
  }

  // 設定ブロックを読む（dateKey 列から mail-20 まで）
  const nSettingRows = Math.min(ALERT_SETTING_MAX_ROWS,
                                layout.mapRow - layout.settingHeaderRow - 1);
  if (nSettingRows <= 0) return;
  const nSettingCols = ALERT_COL_MAIL_FROM + ALERT_MAIL_COUNT;
  const settings = sheet.getRange(layout.settingHeaderRow + 1, layout.keyCol,
                                  nSettingRows, nSettingCols).getValues();

  const props = PropertiesService.getScriptProperties();
  const now = Date.now();

  settings.forEach(function (row) {
    // ★2026-09-24 列名付きの選択肢と旧形式の d番号をどちらも受け付ける。
    const keyMatch = String(row[ALERT_COL_DATEKEY]).trim().match(/^d\d+/);
    if (!keyMatch) return;                      // 空行・区切り行は飛ばす
    const key = keyMatch[0];

    const condition = String(row[ALERT_COL_CONDITION] || '').trim();
    const threshold = parseFloat(row[ALERT_COL_THRESHOLD]);
    const title     = row[ALERT_COL_TITLE];
    const body      = row[ALERT_COL_BODY];
    const addresses = row.slice(ALERT_COL_MAIL_FROM)
                         .filter(function (m) { return m && String(m).trim() !== ''; })
                         .map(function (m) { return String(m).trim(); });

    // 設定が揃っていない行は静かに飛ばす（閾値だけ入っている状態など）
    if (isNaN(threshold) || !title || !body || addresses.length === 0) return;

    const col = keyToCol[key];
    if (col === undefined) {
      console.log('[ALERT] ' + key + ' は対応行に見当たりません（列未割当）');
      return;
    }
    const value = dataRow[col];
    if (value === '' || value === null || value === undefined || isNaN(value)) return;

    let hit;
    if (condition === '上回る')      hit = (value > threshold);
    else if (condition === '下回る') hit = (value < threshold);
    else                            hit = (Math.abs(value) > threshold);  // 既定＝絶対値
    if (!hit) return;

    // ★2026-09-24 同じ列の上限・下限が互いの通知を抑止しないよう、条件ごとに待つ。
    const propKey = 'lastAlert_' + deviceId + '_' + key + '_' + (condition || '絶対値');
    const lastTs = parseInt(props.getProperty(propKey) || '0', 10);
    if (now - lastTs < ALERT_COOLDOWN_MS) {
      console.log('[ALERT] クールダウン中 ' + deviceId + '/' + key +
                  ' 残り' + Math.round((ALERT_COOLDOWN_MS - (now - lastTs)) / 1000) + '秒');
      return;
    }

    try {
      MailApp.sendEmail({
        to: addresses.join(','),
        subject: String(title),
        htmlBody: String(body).replace(/{{value}}/g, value),
        name: 'MONITA通知システム',
      });
      props.setProperty(propKey, String(now));
      console.log('[ALERT] 送信 ' + deviceId + '/' + key +
                  '(' + String(dataHeader[col]).substr(0, 12) + ')=' + value +
                  ' ' + (condition || '絶対値') + ' ' + threshold + ' → ' + addresses.join(','));
    } catch (err) {
      console.error('[ALERT] メール送信失敗: ' + err);
    }
  });
}

// クールダウンを手動でリセットする（検証中に連続で試したいとき）。
function resetAlertCooldown() {
  const props = PropertiesService.getScriptProperties();
  const all = props.getProperties();
  let n = 0;
  Object.keys(all).forEach(function (k) {
    if (k.indexOf('lastAlert_') === 0 || k.indexOf('lastNoDataAlert_') === 0) {
      props.deleteProperty(k); n++;
    }
  });
  console.log('クールダウン情報を ' + n + ' 件削除しました');
}

// メール送信の権限確認。初回は承認ダイアログが出るので許可すること。
function testMailPermission() {
  if (ADMIN_MAILS.length === 0) {
    console.log('ADMIN_MAILS が未設定です。ファイル冒頭に宛先を追加してください');
    return;
  }
  MailApp.sendEmail(ADMIN_MAILS[0], '【テスト】GAS権限確認',
                    'このメールが届けば、メール送信の認証は問題ありません。');
  console.log('テストメールを送信しました → ' + ADMIN_MAILS[0]);
}


// ================================
// データ未受信アラート
// ================================
// ★GASのトリガーで定期実行する（15〜30分ごとを推奨）。
//   エディタ左の「トリガー」→ 関数 checkNoDataAlert、イベントソース「時間主導型」。
//
// ★1年間の無人運用では、閾値アラートよりこちらのほうが重要になりやすい。
//   Gateway が現場で止まっても、データが来ないこと自体には気づけないため。
//
// 判定対象は「シート名編集」で有効になっている全デバイス。
// 各シートの A列（受信日時）の最終値を見る。
function checkNoDataAlert() {
  if (ADMIN_MAILS.length === 0) {
    console.log('ADMIN_MAILS が未設定のため未受信アラートはスキップします');
    return;
  }
  const ss = getSpreadsheet();
  const map = loadDeviceSheetMapFromSheet();   // ここはキャッシュを使わない（定期実行なので）
  const props = PropertiesService.getScriptProperties();
  const now = Date.now();

  Object.keys(map).forEach(function (deviceId) {
    const sheetName = map[deviceId];
    const sheet = ss.getSheetByName(sheetName);
    if (!sheet) return;

    const last = sheet.getLastRow();
    const headerRow = findHeaderRow_(sheet);
    if (last <= headerRow) return;   // 見出しまで＝まだデータなし

    // ★A列を遡って最後の日時を探す。設定ブロックのラベルは文字列なので
    //   Date かどうかで判定すれば取り違えない。
    // ★時刻列は A列とは限らない（0002 は B列）。検出した列を読む。
    const pos = findHeaderPos_(sheet);
    const col = sheet.getRange(1, pos.col, last, 1).getValues();
    let lastTime = null;
    for (let i = col.length - 1; i >= headerRow; i--) {
      if (col[i][0] instanceof Date) { lastTime = col[i][0]; break; }
    }
    if (!lastTime) return;

    const diff = now - lastTime.getTime();
    if (diff <= NO_DATA_LIMIT_MS) return;

    const propKey = 'lastNoDataAlert_' + deviceId;
    const lastAlert = parseInt(props.getProperty(propKey) || '0', 10);
    if (now - lastAlert < NO_DATA_LIMIT_MS) return;   // 同じ通知を繰り返さない

    try {
      MailApp.sendEmail({
        to: ADMIN_MAILS.join(','),
        subject: '【未受信警告】' + sheetName + '（DeviceID ' + deviceId + '）',
        body: '以下のデバイスで ' + Math.round(NO_DATA_LIMIT_MS / 60000) +
              '分以上データ受信がありません。\n\n' +
              'DeviceID: ' + deviceId + '\n' +
              '対象シート: ' + sheetName + '\n' +
              '最終受信時刻: ' + lastTime + '\n\n' +
              'ご確認をお願いいたします。',
        name: 'MONITA通知システム',
      });
      props.setProperty(propKey, String(now));
      console.log('[NO-DATA] 通知 ' + sheetName + ' 最終受信=' + lastTime);
    } catch (err) {
      console.error('[NO-DATA] メール送信失敗: ' + err);
    }
  });
}


// ================================
// ダウンリンク — Drive への書き出し
// ================================
// Gateway が読む本文を Drive のファイルへ書く（方式A・2段目の取得元）。
// 先頭行は Gateway が送ってきたワンタイム値(nonce)。
//
// ★nonce が必要な理由: Gateway は 1段目(GAS)の応答本文を読まない。
//   つまり GAS が失敗していても Gateway には成功に見える。そのまま 2段目を読むと
//   「古いファイルの内容を新鮮だと思い込む」事故が起きる（2026-08-30 の
//   SPREADSHEET_ID 未設定と同じ形の落とし穴）。毎回違う nonce を書かせることで、
//   Gateway 側が「今回のリクエストで書かれたファイルか」を判定できる。
function writeDownlinkFile_(group, nonce, body) {
  const fileId = DOWNLINK_DRIVE_FILE_IDS[group];
  if (!fileId || fileId.indexOf('ここに') === 0) {
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

// ★エディタから手動実行して、Drive書き込みだけを単独で確認するための関数。
//   Gateway を動かさずに「GASがDriveへ書けるか」だけを切り分けられる。
//   初回実行時は Drive へのアクセス承認ダイアログが出るので許可すること。
function testWriteDownlinkFile() {
  const group = 0;
  const nonce = 'TEST' + Math.floor(Math.random() * 10000);
  if (!writeDownlinkFile_(group, nonce, 'none')) {
    console.log('✗ 書き込み失敗。上に出ている [DOWNLINK] のログを見ること');
    return;
  }
  const content = DriveApp.getFileById(DOWNLINK_DRIVE_FILE_IDS[group]).getBlob().getDataAsString();
  console.log('✓ 書き込み成功。ファイルの現在の内容:');
  console.log(content);
  console.log('先頭行が ' + nonce + ' になっていればGateway側のnonce検証を通る');
}


// ================================
// ダウンリンク予約（Class A + 確認応答方式）
// ================================
// 【方式】子機は省電力のため常時受信できない。そこで「子機が自分のアップリンクを
//   送った直後に2秒だけ受信窓を開け、Gatewayがその場で即座に応答する」Class A方式。
//   Gateway は送信しただけでは予約を消さず、子機からの確認フレームを受け取って
//   初めて完了扱いにする。取りこぼしても子機の次サイクルで自動的に再試行される。
//
// 【保持形式】Script Properties のキー `downlink_child_<HEX2>` に JSON で保持。
//   state: queued（予約直後）→ sent（送信済み・確認待ち）→ done / failed

const DL_STATUS_OK          = 0;   // 要求通り適用
const DL_STATUS_RANGE_ERROR = 1;   // 値域エラー（子機が拒否）
const DL_STATUS_CLAMPED     = 2;   // クランプ適用（WDT制約等で要求と異なる値を適用）
const DL_STATUS_SAVE_ERROR  = 3;   // 子機の flash 保存に失敗（設定は変更されていない）。子機 FW12〜
const DL_STATUS_NO_ACK      = 99;  // 未達（Gatewayが規定回数送っても確認が返らず）

const DOWNLINK_MAX_LINES = 20;

// ダウンリンクの宛先になりうる子機DeviceID（16進2桁）。この Gateway の群の 1〜31 番（群0 = 0x01〜0x1F、群1 = 0x21〜0x3F）
// ★2026-09-24 群0固定の一覧だったため、GATEWAY_GROUP から作るように変えた（群を変えるとダウンリンクが一切届かなくなっていた）
const CMD_STATUS_CHILD_IDS = (function () {
  const ids = [];
  for (let n = 1; n <= 31; n++) ids.push(('0' + (GATEWAY_GROUP * 32 + n).toString(16).toUpperCase()).slice(-2));
  return ids;
})();

// 一覧表示する Gateway 側デバイスID
// ★2026-09-23 本番用の ID（gateway_v11_test 等）から、この案件の Gateway に差し替え。
//   メニュー・設定欄から入れる Gateway コマンドは、先頭の ID 宛てになる。
const CMD_STATUS_GATEWAY_IDS = [
  { id: GATEWAY_DEVICE_ID, label: 'Gateway' },
];

function dlKey_(childHex) { return 'downlink_child_' + childHex; }

function dlGet_(childHex) {
  const raw = PropertiesService.getScriptProperties().getProperty(dlKey_(childHex));
  if (!raw) return null;
  try { return JSON.parse(raw); } catch (e) { return null; }
}

function dlSet_(childHex, obj) {
  obj.updated = new Date().toISOString();
  PropertiesService.getScriptProperties().setProperty(dlKey_(childHex), JSON.stringify(obj));
}

// 未完了（queued / sent）の予約を、Gatewayが解釈する1行1件の形式で組み立てる。
// 形式: HEX2:sleepMin:avg:median:attempts:seq:mode
//   ★attempts も seq も GAS 側を正とする。Gateway 側で数えると、再取得のたびに
//     リセットされたり、予約を入れ直しても古い試行回数を引き継いだりするため。
//   ★mode: 0=通常の設定変更 / 1=ステータス確認のみ
function buildDownlinkLines_(group) {
  const lines = [];
  for (let ci = 0; ci < CMD_STATUS_CHILD_IDS.length && lines.length < DOWNLINK_MAX_LINES; ci++) {
    const hex = CMD_STATUS_CHILD_IDS[ci];
    if (group !== undefined && (parseInt(hex, 16) >> 5) !== group) continue;
    const d = dlGet_(hex);
    if (d && (d.state === 'queued' || d.state === 'sent')) {
      lines.push(hex + ':' + d.sleep + ':' + d.avg + ':' + d.median +
                 ':' + (d.attempts || 0) + ':' + (d.seq || 0) +
                 ':' + (d.mode === 'status' ? 1 : 0));
    }
  }
  return lines;
}

function dlWdtSuffix_(d) {
  // ★2026-09-24 自動再起動までの時間は現在値欄に表示し、状態には付記しない。
  return '';
}

function dlStatusLabel_(d) {
  if (!d) return '';
  if (d.state === 'queued') return '';
  if (d.state === 'sent')   return '送信済み（確認待ち）';
  if (d.status === DL_STATUS_OK)          return '完了' + dlWdtSuffix_(d);
  if (d.status === DL_STATUS_CLAMPED)     return '完了（値を丸めた: 間隔=' + d.appliedSleep + '分, 平均=' + d.appliedAvg + ', メジアン=' + d.appliedMedian + '）' + dlWdtSuffix_(d);
  if (d.status === DL_STATUS_RANGE_ERROR) return '失敗（子機が値域エラーで拒否。設定は変更されていない）';
  if (d.status === DL_STATUS_SAVE_ERROR)  return '失敗（子機の flash 保存に失敗。設定は変更されていない）';
  if (d.status === DL_STATUS_NO_ACK)      return '失敗（未達。' + (d.attempts || 0) + '回試行しても確認が返らず）';
  return '失敗（不明なステータス: ' + d.status + '）';
}

function promptChildHex_(ui, title, message) {
  const r = ui.prompt(title, message, ui.ButtonSet.OK_CANCEL);
  if (r.getSelectedButton() !== ui.Button.OK) return null;
  const hex = r.getResponseText().trim().replace(/^0x/i, '');
  if (!/^[0-9a-fA-F]{1,2}$/.test(hex)) {
    ui.alert('DeviceIDは16進2桁で入力してください（例: 0E）。');
    return null;
  }
  return ('0' + hex).slice(-2).toUpperCase();
}

// 子機が確認応答で返した「実際に適用されている値」を返す。不明なら null。
// ★2026-09-23 Codexレビュー反映: 旧実装は、確認済みの値が無いと「直近の予約値」や固定の既定値（送信間隔60分・平均5・メジアン5）で
//   埋めていた（Codexレビュー指摘#8）。案件ごとに子機の初期値は違うので、平均だけ変えたつもりが
//   送信間隔まで60分に変わることがあった。ステータス確認の予約（値がすべて0）も「直近の予約値」として
//   拾っていた。→ 確認済みの値だけを使い、無ければ null を返して呼び出し側で止める。
//   確認済みの値は予約とは別のキーに保存する。予約を入れ直すと予約レコードは作り直されるため。
function dlAppliedKey_(childHex) { return 'downlink_applied_' + childHex; }

function getKnownChildSettings_(childHex) {
  const raw = PropertiesService.getScriptProperties().getProperty(dlAppliedKey_(childHex));
  if (raw) {
    try { return JSON.parse(raw); } catch (e) {}
  }
  const d = dlGet_(childHex);   // 別キーに保存する前の完了記録（移行用）
  if (d && d.state === 'done' && d.appliedSleep) {
    return { sleep: d.appliedSleep, avg: d.appliedAvg, median: d.appliedMedian, wdt: d.appliedWdtMin || '' };
  }
  return null;
}

// ★予約ごとに通し番号(seq)を振る。Gatewayの報告にこのseqを含めてもらい、
//   一致しない報告は無視する。これが無いと、古い予約の「未達」報告が、
//   その後に入れ直した新しい予約を失敗扱いで上書きして消す（実機で発生）。
function queueDownlink_(childHex, sleepMin, avg, median, sourceNote, mode) {
  const lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);
    const prevRec = dlGet_(childHex);
    // ★2026-09-23 Codexレビュー反映: seq は予約レコードとは別のカウンタで単調増加させる（Codexレビュー指摘#3）。
    //   旧実装は予約レコードの seq +1 だったため、取消（レコード削除）の後は seq=1 に戻り、
    //   Gateway が保持している完了済みの seq=1 と同じ番号になって、新しい予約が送られなかった。
    const props = PropertiesService.getScriptProperties();
    const seqKey = 'downlink_seq_' + childHex;
    const lastSeq = Math.max(prevRec && prevRec.seq ? prevRec.seq : 0, parseInt(props.getProperty(seqKey) || '0', 10));
    const nextSeq = lastSeq + 1;
    props.setProperty(seqKey, String(nextSeq));

    dlSet_(childHex, {
      sleep: sleepMin, avg: avg, median: median,
      state: 'queued', attempts: 0, seq: nextSeq,
      mode: mode || 'set',
    });

    dlLog_(childHex, nextSeq, (mode === 'status') ? 'ステータス確認要求' : '予約',
           (mode === 'status') ? '（設定変更なし）' : ('間隔=' + sleepMin + '分 / 平均=' + avg + ' / メジアン=' + median),
           (prevRec && prevRec.state !== 'done' && prevRec.state !== 'failed')
             ? '★未完了の予約(seq=' + prevRec.seq + ')を上書きしました'
             : sourceNote);
  } catch (err) {
    console.log('Downlink reservation lock error: ' + err);
    return false;
  } finally {
    try { lock.releaseLock(); } catch (e2) {}
  }
  // ★2026-09-24 FW21: 予約失敗を呼び出し元へ返す。表示更新だけの失敗で予約済みを見失わない。
  try { syncDownlinkBlock_(childHex); refreshCmdStatusSheet(); }
  catch (err) { console.error('Downlink display error: ' + err); }
  return true;
}


// 予約を取り消す（★2026-09-23 Codexレビュー反映: 報告処理と同じロックで守る。Codexレビュー指摘#3）。
// 取り消しても seq のカウンタは残すので、次の予約は必ず新しい番号になる。
function cancelDownlink_(childHex, note) {
  const lock = LockService.getScriptLock();
  let d = null;
  try {
    lock.waitLock(10000);
    d = dlGet_(childHex);
    if (!d) return false;
    dlLog_(childHex, d.seq, '取消',
           '間隔=' + d.sleep + '分, 平均=' + d.avg + ', メジアン=' + d.median + '（状態: ' + (d.state || '?') + '）', note);
    PropertiesService.getScriptProperties().deleteProperty(dlKey_(childHex));
  } catch (err) {
    console.log('Downlink cancel lock error: ' + err);
    return false;
  } finally {
    try { lock.releaseLock(); } catch (e2) {}
  }
  syncDownlinkBlock_(childHex);
  refreshCmdStatusSheet();
  return true;
}


// ================================
// doGet — 制御系アクション専用
// ================================
// ★テレメトリ（計測データ）は doPost で受ける。ここには来ない。
//   doGet に残しているのは、Gateway からのポーリング・報告と、管理者操作だけ。
function doGet(e) {
  const p = (e && e.parameter) ? e.parameter : {};

  // ── ダウンリンク予約＋リモートコマンドの取得 ──────────────
  // Gateway が定期的に呼ぶ。dl=1 のとき2行目以降に予約を相乗りさせる。
  //   1行目     : コマンド（無ければ "none"）
  //   2行目以降 : "HEX2:sleepMin:avg:median:attempts:seq:mode"
  //
  // ★nonce 付きで来たら、同じ内容を Drive のファイルにも書く（方式A）。
  //   Gateway はこの応答本文を読まない（chunked で読めないため）。Drive から読む。
  if (p.action === 'check_cmd') {
    const deviceId = p.device_id || 'default';
    // ★2026-09-23 Gateway FW17〜 は現在の送信間隔（gw_interval）を毎回載せてくる。
    //   予約した間隔と一致したら、ここで予約を消して完了にする（コマンドを返す前に行う）。
    if (p.gw_interval !== undefined) recordGatewayInterval_(deviceId, parseInt(p.gw_interval, 10));
    const cmd = PropertiesService.getScriptProperties().getProperty('pending_cmd_' + deviceId) || '';
    if (p.dl !== '1') {
      return ContentService.createTextOutput(cmd || 'none');  // 旧ファーム互換（1行のみ）
    }
    const group = parseInt(p.group || '0', 10);
    const text = [cmd || 'none'].concat(buildDownlinkLines_(group)).join('\n');
    if (p.nonce) writeDownlinkFile_(group, String(p.nonce), text);
    return ContentService.createTextOutput(text);
  }

  // Gateway がコマンドを実際に受け取り、実行する直前に呼ぶ。ここで初めて消費する。
  // ★「読む」と「消費する」を分離している理由: 応答の取得に失敗することがあり、
  //   読んだ時点で消すと「消費済みなのに実行されない」まま失われるため。
  if (p.action === 'ack_cmd') {
    const ackKey = 'pending_cmd_' + (p.device_id || 'default');
    const props = PropertiesService.getScriptProperties();
    // ★cmd が付いていれば、予約中のコマンドと同じときだけ消す（後から入れ直した予約を消さないため）
    const lock = LockService.getScriptLock();
    try {
      lock.waitLock(10000);
      const pending = props.getProperty(ackKey);
      if (p.cmd === undefined || pending === String(p.cmd)) {
        props.deleteProperty(ackKey);
        if (pending) gwLog_('Gatewayコマンド 完了', p.cmd || pending, 'Gateway が受け取りました');
      } else {
        gwLog_('Gatewayコマンド 完了(不一致)', p.cmd, '現在の予約: ' + (pending || 'なし'));
      }
    } catch (err) {
      return ContentService.createTextOutput('error: ack lock');
    } finally {
      try { lock.releaseLock(); } catch (e2) {}
    }
    syncGatewayBlock_();
    refreshCmdStatusSheet();
    return ContentService.createTextOutput('ok');
  }

  // Gateway の起動ログ（Gateway FW23〜）。起動するたびに機器情報を1行残す。
  if (p.action === 'gw_boot') {
    try {
      appendGatewayBootLog_(p);
      return ContentService.createTextOutput('ok');
    } catch (err) {
      console.error('[GW-BOOT] 記録失敗: ' + err);
      return ContentService.createTextOutput('error: ' + err);   // Gateway は ok 以外なら次サイクルで送り直す
    }
  }

  // Gateway が送信間隔を変更した直後に現在値を知らせてくる（Gateway FW17〜）。
  if (p.action === 'gw_state') {
    if (p.gw_interval !== undefined) recordGatewayInterval_(p.device_id || 'default', parseInt(p.gw_interval, 10));
    return ContentService.createTextOutput('ok');
  }

  // Gateway がダウンリンクを送信した時点の中間報告（まだ確認は取れていない）。
  if (p.action === 'downlink_sent') {
    const sentHex = String(p.child || '').toUpperCase();
    if (!/^[0-9A-F]{2}$/.test(sentHex)) return ContentService.createTextOutput('error: bad child id');

    const sentGroup = parseInt(p.group || '0', 10);
    const sentChildGroup = parseInt(sentHex, 16) >> 5;
    if (sentGroup !== sentChildGroup) {
      const note = '報告元の群' + sentGroup + 'と子機IDの群' + sentChildGroup + 'が不一致のため処理をスキップ';
      console.error('Downlink ACK ownership mismatch: child=' + sentHex + ', ' + note);
      dlLog_(sentHex, p.seq, '送信(群不一致)', '', note, sentGroup);
      return ContentService.createTextOutput('error: group mismatch');
    }

    const lock = LockService.getScriptLock();
    try {
      lock.waitLock(10000);
      const rec = dlGet_(sentHex);
      if (!rec || String(p.seq || '') !== String(rec.seq || '')) {
        dlLog_(sentHex, p.seq, '送信(期限切れ)', '', '送信後に予約が入れ替わっていました');
        return ContentService.createTextOutput('stale: seq mismatch');
      }
      rec.state = 'sent';
      rec.attempts = parseInt(p.attempts || '1', 10);
      dlSet_(sentHex, rec);
      dlLog_(sentHex, p.seq, '送信',
             '間隔=' + rec.sleep + '分 / 平均=' + rec.avg + ' / メジアン=' + rec.median,
             rec.attempts + '回目（子機からの確認応答を待っています）', sentGroup);
    } catch (err) {
      console.log('Downlink sent lock error: ' + err);
      return ContentService.createTextOutput('error: lock timeout');
    } finally {
      try { lock.releaseLock(); } catch (e2) {}
    }
    syncDownlinkBlock_(sentHex);
    refreshCmdStatusSheet();
    return ContentService.createTextOutput('ok');
  }

  // ダウンリンクの最終結果報告。
  //   子機から確認フレームを受け取った場合はその中身、規定回数送っても確認が
  //   返らなかった場合は status=99（未達）を Gateway 自身が報告する。
  if (p.action === 'downlink_result') {
    const childHex = String(p.child || '').toUpperCase();
    if (!/^[0-9A-F]{2}$/.test(childHex)) return ContentService.createTextOutput('error: bad child id');

    const reportGroup = parseInt(p.group || '0', 10);
    const childGroup = parseInt(childHex, 16) >> 5;
    if (reportGroup !== childGroup) {
      const note = '報告元の群' + reportGroup + 'と子機IDの群' + childGroup + 'が不一致のため処理をスキップ';
      console.error('Downlink ACK ownership mismatch: child=' + childHex + ', ' + note);
      dlLog_(childHex, p.seq, '結果(群不一致)', '', note, reportGroup);
      return ContentService.createTextOutput('error: group mismatch');
    }

    const wdtMin = parseInt(p.wdt || '0', 10);
    const applied = (p.sleep || '?') + ' / ' + (p.avg || '?') + ' / ' + (p.median || '?') +
                    (wdtMin ? ('（自動再起動 ' + wdtMin + '分）') : '');

    const lock = LockService.getScriptLock();
    try {
      lock.waitLock(10000);
      const rec = dlGet_(childHex);

      // ★履歴は「予約が今も生きているか」に関わらず必ず残す。
      //   以前は seq 不一致のときここより前で return しており、予約を入れ直すと
      //   「実際に子機へ配信されて適用された」事実が履歴から消えていた。
      if (!rec || String(p.seq || '') !== String(rec.seq || '')) {
        dlLog_(childHex, p.seq, '結果(期限切れ)', '適用値: ' + applied,
               'この応答が返る前に予約が入れ替わっていたため、現在の予約状態には反映していません' +
               '（子機側では実際に適用されています）');
        return ContentService.createTextOutput('stale: seq mismatch');
      }

      rec.status        = parseInt(p.status || '0', 10);
      rec.attempts      = parseInt(p.attempts || '0', 10);
      rec.appliedSleep  = parseInt(p.sleep  || '0', 10);
      rec.appliedAvg    = parseInt(p.avg    || '0', 10);
      rec.appliedMedian = parseInt(p.median || '0', 10);
      rec.appliedWdtMin = wdtMin;
      rec.state = (rec.status === DL_STATUS_OK || rec.status === DL_STATUS_CLAMPED) ? 'done' : 'failed';
      dlSet_(childHex, rec);
      // ★2026-09-23 Codexレビュー反映: 子機が返した現在値は、成功・失敗にかかわらず「子機の実際の状態」なので保存する
      //   （値域エラーや保存失敗のときも、子機は変更前の値を返してくる）。
      if (rec.appliedSleep) {
        PropertiesService.getScriptProperties().setProperty(dlAppliedKey_(childHex), JSON.stringify({
          sleep: rec.appliedSleep, avg: rec.appliedAvg, median: rec.appliedMedian, wdt: wdtMin || '',
          at: new Date().toISOString(),
        }));
      }

      dlLog_(childHex, p.seq, '結果',
             '要求: ' + rec.sleep + ' / ' + rec.avg + ' / ' + rec.median + '　→　適用: ' + applied,
             dlStatusLabel_(rec) + '（' + rec.attempts + '回目で確定）', reportGroup);
    } catch (err) {
      console.log('Downlink result lock error: ' + err);
      return ContentService.createTextOutput('error: lock timeout');
    } finally {
      try { lock.releaseLock(); } catch (e2) {}
      processDeferredBatch_();
    }
    syncDownlinkBlock_(childHex);
    refreshCmdStatusSheet();
    return ContentService.createTextOutput('ok');
  }

  // 管理者がブラウザ等からURLを叩いてコマンドを予約する（token認証必須）。
  if (p.action === 'set_cmd') {
    if (String(p.token || '') !== CMD_TOKEN) {
      return ContentService.createTextOutput('unauthorized');
    }
    // ★2026-09-24 FW24: URL からの操作も同じ予約枠を使い、別コマンドを上書きしない。
    if ((p.device_id || 'default') === GATEWAY_DEVICE_ID) {
      const cmd = String(p.cmd || '');
      const interval = /^interval:(\d+)$/.exec(cmd);
      const err = interval ? queueGatewayInterval_(Number(interval[1]), 'URLから予約')
                           : queueGatewayCmd_(cmd, cmd, 'URLから予約');
      return ContentService.createTextOutput(err || ('ok: queued "' + cmd + '"'));
    }
    const conflict = gatewayCmdConflict_(String(p.cmd || ''), p.device_id || 'default');
    if (conflict) return ContentService.createTextOutput(conflict);
    // ★2026-09-24 FW21: URLからの予約にも、シートと同じ容量判定を適用する。
    const interval = /^interval:(\d+)$/.exec(String(p.cmd || ''));
    if (interval) {
      const err = gatewayIntervalError_(Number(interval[1]));
      if (err) return ContentService.createTextOutput(err);
      if ((p.device_id || 'default') === GATEWAY_DEVICE_ID) cancelDeferredBatch_('Gateway の個別予約');
    }
    PropertiesService.getScriptProperties()
      .setProperty('pending_cmd_' + (p.device_id || 'default'), p.cmd || '');
    refreshCmdStatusSheet();
    return ContentService.createTextOutput('ok: queued "' + (p.cmd || '') + '"');
  }

  // Gateway のステータス報告（電波強度・稼働時間・空きヒープ）。
  if (p.action === 'status_report') {
    appendGatewayStatus_(p);
    return ContentService.createTextOutput('ok');
  }

  // 診断ログの吸い上げ（gwlog.csv 末尾の hex）。現場に行かずに直近の挙動を確認できる。
  if (p.action === 'log_dump') {
    const hex = p.log || '';
    let text = '';
    for (let li = 0; li < hex.length; li += 2) {
      text += String.fromCharCode(parseInt(hex.substr(li, 2), 16));
    }
    const ss = getSpreadsheet();
    const sheet = ss.getSheetByName('gwlog') || ss.insertSheet('gwlog');
    sheet.appendRow([new Date(), p.device_id || 'default', text]);
    return ContentService.createTextOutput('ok');
  }

  // Gateway 起動時の情報行。
  if (p.row_type === 'info') {
    const ss = getSpreadsheet();
    const sheet = ss.getSheetByName('gw_status') || ss.insertSheet('gw_status');
    if (sheet.getLastRow() === 0) {
      sheet.appendRow(['受信日時', 'Gateway ID', 'CSQ', '稼働時間(分)', '空きヒープ(B)']);
      sheet.setFrozenRows(1);
    }
    sheet.appendRow([new Date(), p.gw_id || 'GW', p.csq || '',
                     'fw' + (p.gw_fw || '?') + ' xiao=' + (p.xiao_id || '') +
                     ' imei=' + (p.sim_imei || '') + ' iccid=' + (p.sim_iccid || '') +
                     ' group=' + (p.group || '')]);
    return ContentService.createTextOutput('ok');
  }

  // ★テレメトリは doPost で受ける。GET で来たら誤送信とみなして明示的に返す。
  return ContentService.createTextOutput(
    'error: telemetry must be sent via POST (JSON). GET is for control actions only.');
}


// ================================
// 予約状況シート / 履歴シート
// ================================
const CMD_STATUS_SHEET_NAME = 'cmd_status';
const CMD_STATUS_HEADER = ['デバイスID', '種別', '現在の予約', '状態', '結果', '試行回数', '最終更新日時'];
// ★2026-09-24 お客様向けの履歴名と10進の子機IDに統一する。
const DOWNLINK_LOG_SHEET_NAME = '設定変更履歴';
const DOWNLINK_LOG_HEADER = ['日時', '子機ID（10進）', 'seq', '種別', '内容', '備考', 'Gateway群'];

function getCmdStatusSheet_() {
  const ss = getSpreadsheet();
  let sheet = ss.getSheetByName(CMD_STATUS_SHEET_NAME);
  if (!sheet) {
    sheet = ss.insertSheet(CMD_STATUS_SHEET_NAME);
    sheet.appendRow(CMD_STATUS_HEADER);
    sheet.setFrozenRows(1);
  } else if (sheet.getLastColumn() < CMD_STATUS_HEADER.length) {
    sheet.getRange(1, 1, 1, CMD_STATUS_HEADER.length).setValues([CMD_STATUS_HEADER]);
  }
  return sheet;
}

function getDownlinkLogSheet_() {
  const ss = getSpreadsheet();
  let sheet = ss.getSheetByName(DOWNLINK_LOG_SHEET_NAME);
  let needsDesign = false, renamed = false;
  if (!sheet) {
    // ★2026-09-24 旧名は移行の検索だけに使い、既存の履歴を残して改名する。
    sheet = ss.getSheetByName('downlink_log');
    if (sheet) {
      sheet.setName(DOWNLINK_LOG_SHEET_NAME);
      renamed = true;
    } else {
      sheet = ss.insertSheet(DOWNLINK_LOG_SHEET_NAME);
    }
    needsDesign = true;
  }
  // ★2026-09-24 改名時か旧見出しのときだけ変換する。Gateway 名・数値はそのまま。
  if (renamed || sheet.getRange(1, 2).getValue() === '子機ID') {
    const count = sheet.getLastRow() - 1;
    if (count > 0) {
      const ids = sheet.getRange(2, 2, count, 1).getValues();
      ids.forEach(function (row) {
        if (/^0x[0-9A-Fa-f]{2}$/.test(String(row[0]))) row[0] = parseInt(row[0], 16);
      });
      sheet.getRange(2, 2, count, 1).setValues(ids);
    }
    needsDesign = true;
  }
  if (needsDesign || sheet.getLastColumn() < DOWNLINK_LOG_HEADER.length) {
    sheet.getRange(1, 1, 1, DOWNLINK_LOG_HEADER.length).setValues([DOWNLINK_LOG_HEADER]);
  }
  if (needsDesign) UI_designDownlinkLog_(sheet);
  return sheet;
}

// 履歴を1行追記する。どの経路からでも必ずここを通す（記録漏れを作らないため）。
function dlLog_(childHex, seq, kind, detail, note, group) {
  try {
    const logGroup = (group === undefined) ? (parseInt(childHex, 16) >> 5) : group;
    getDownlinkLogSheet_().appendRow([
      new Date(), parseInt(childHex, 16), seq || '', kind, detail || '', note || '', logGroup,
    ]);
  } catch (e) {
    console.error('設定変更履歴 追記に失敗: ' + e);  // 履歴の失敗で本体を止めない
  }
}

function refreshCmdStatusSheet() {
  const props = PropertiesService.getScriptProperties();
  const sheet = getCmdStatusSheet_();
  const now = new Date();
  const nCol = CMD_STATUS_HEADER.length;

  const lastRow = sheet.getLastRow();
  if (lastRow > 1) sheet.getRange(2, 1, lastRow - 1, nCol).clearContent();

  const rows = [];
  CMD_STATUS_GATEWAY_IDS.forEach(function (dev) {
    const cmd = props.getProperty('pending_cmd_' + dev.id) || 'none';
    const applied = getGatewayAppliedInterval_(dev.id);
    rows.push([dev.id, dev.label, cmd, cmd === 'none' ? '—' : '予約中',
               (applied ? ('現在の送信間隔=' + applied + '分 / ') : '') + gatewayPausedLabel_(dev.id), '', now]);
  });
  CMD_STATUS_CHILD_IDS.forEach(function (childHex) {
    const d = dlGet_(childHex);
    if (!d) {
      rows.push(['0x' + childHex, 'Flex子機', 'none', '—', '', '', now]);
      return;
    }
    const req = '間隔=' + d.sleep + '分, 平均=' + d.avg + ', メジアン=' + d.median;
    const stateLabel = { queued: '予約中', sent: '送信済み（確認待ち）', done: '完了', failed: '失敗' }[d.state] || d.state;
    rows.push(['0x' + childHex, 'Flex子機', req, stateLabel, dlStatusLabel_(d),
               d.attempts || 0, d.updated ? new Date(d.updated) : now]);
  });

  sheet.getRange(2, 1, rows.length, nCol).setValues(rows);
}


// ================================
// スプレッドシートのメニュー操作
// ================================
function triggerGatewayReset()     { setGatewayCmd_('reset',      'リセット'); }
function triggerGatewayStop()      { setGatewayCmd_('stop',       'データ送信の停止'); }
function triggerGatewayStart()     { setGatewayCmd_('start',      'データ送信の再開'); }
function triggerGatewaySendNow()   { setGatewayCmd_('send_now',   '今すぐ送信'); }
function triggerGatewayStatusNow() { setGatewayCmd_('status_now', 'ステータス報告'); }
function triggerGatewayRtcResync() { setGatewayCmd_('rtc_resync', 'RTC再同期'); }
function triggerGatewayLogDump()   { setGatewayCmd_('log_dump',   '診断ログの吸い上げ'); }

function setGatewayCmd_(cmd, label) {
  const ui = SpreadsheetApp.getUi();
  const err = queueGatewayCmd_(cmd, label, 'メニューから予約');
  ui.alert(err || gatewayCommandResult_(cmd, label));
}

function triggerGatewaySetInterval() {
  const ui = SpreadsheetApp.getUi();
  const r = ui.prompt('送信間隔の変更', '新しい送信間隔を分単位で入力してください（1〜1440分）:', ui.ButtonSet.OK_CANCEL);
  if (r.getSelectedButton() !== ui.Button.OK) return;
  const minutes = parseInt(r.getResponseText(), 10);
  if (isNaN(minutes) || minutes < 1 || minutes > 1440) {
    ui.alert('1〜1440の整数を入力してください。');
    return;
  }
  const err = queueGatewayInterval_(minutes, 'メニューから予約');
  if (err) { ui.alert(err); return; }
  ui.alert('送信間隔を' + minutes + '分に変更するよう予約しました。');
}

// 子機の現在設定を確認する（設定は変更しない）。
function triggerFlexStatusCheck() {
  const ui = SpreadsheetApp.getUi();
  const hex = promptChildHex_(ui, 'Flex ステータス確認', '対象の子機DeviceIDを16進2桁で入力してください（例: 0E）:');
  if (!hex) return;
  if (!queueDownlink_(hex, 0, 0, 0, 'メニュー（ステータス確認）から予約', 'status')) { ui.alert('予約に失敗しました。再度操作してください。'); return; }
  ui.alert('子機0x' + hex + 'のステータス確認を予約しました。\n\n' +
           '子機は省電力のため常時受信していません。次にこの子機がアップリンクを送った' +
           'タイミング（最大で子機の送信間隔ぶん）で応答します（設定は変更しません）。');
}

function triggerFlexSetInterval() {
  const ui = SpreadsheetApp.getUi();
  const hex = promptChildHex_(ui, 'Flex 送信間隔の変更 (1/2)', '対象の子機DeviceIDを16進2桁で入力してください（例: 0E）:');
  if (!hex) return;
  const known = getKnownChildSettings_(hex);
  if (!known) { ui.alert('子機0x' + hex + ' の現在値が不明です。先に「ステータス確認」を行ってください。'); return; }
  const r = ui.prompt('Flex 送信間隔の変更 (2/2)',
    '新しい送信間隔を分単位で入力してください（1〜1440分）:\n' +
    '（平均=' + known.avg + ', メジアン=' + known.median + ' は変更せず維持します）',
    ui.ButtonSet.OK_CANCEL);
  if (r.getSelectedButton() !== ui.Button.OK) return;
  const sleepMin = parseInt(r.getResponseText(), 10);
  if (isNaN(sleepMin) || sleepMin < 1 || sleepMin > 1440) {
    ui.alert('送信間隔は1〜1440の整数で入力してください。');
    return;
  }
  const gwErr = childIntervalError_(sleepMin);
  if (gwErr) { ui.alert(gwErr); return; }
  if (!queueDownlink_(hex, sleepMin, known.avg, known.median, 'メニュー（送信間隔のみ変更）から予約')) { ui.alert('予約に失敗しました。再度操作してください。'); return; }
  ui.alert('子機0x' + hex + 'の送信間隔を' + sleepMin + '分に変更するよう予約しました。');
}

function triggerFlexSetAvgMedian() {
  const ui = SpreadsheetApp.getUi();
  const hex = promptChildHex_(ui, 'Flex 平均/メジアン回数の変更 (1/2)', '対象の子機DeviceIDを16進2桁で入力してください（例: 0E）:');
  if (!hex) return;
  const known = getKnownChildSettings_(hex);
  if (!known) { ui.alert('子機0x' + hex + ' の現在値が不明です。先に「ステータス確認」を行ってください。'); return; }
  const r = ui.prompt('Flex 平均/メジアン回数の変更 (2/2)',
    '平均回数,メジアン回数をカンマ区切りで入力してください（例: 8,8）:\n' +
    '（送信間隔=' + known.sleep + '分 は変更せず維持します）',
    ui.ButtonSet.OK_CANCEL);
  if (r.getSelectedButton() !== ui.Button.OK) return;
  const parts = r.getResponseText().split(',');
  const avg = parseInt(parts[0], 10);
  const median = parseInt(parts[1], 10);
  if (isNaN(avg) || isNaN(median) || avg < 1 || avg > 255 || median < 1 || median > 255) {
    ui.alert('平均回数・メジアン回数はそれぞれ1〜255の整数で入力してください（例: 8,8）。');
    return;
  }
  if (!queueDownlink_(hex, known.sleep, avg, median, 'メニュー（平均/メジアンのみ変更）から予約')) { ui.alert('予約に失敗しました。再度操作してください。'); return; }
  ui.alert('子機0x' + hex + 'の平均回数=' + avg + ', メジアン回数=' + median + 'に変更するよう予約しました。');
}

function triggerCancelFlexReservation() {
  const ui = SpreadsheetApp.getUi();
  const hex = promptChildHex_(ui, 'Flex予約の取り消し (1/2)', '対象の子機DeviceIDを16進2桁で入力してください（例: 0E）:');
  if (!hex) return;
  const d = dlGet_(hex);
  if (!d) {
    ui.alert('子機0x' + hex + ' には現在予約がありません。');
    return;
  }
  const current = '間隔=' + d.sleep + '分, 平均=' + d.avg + ', メジアン=' + d.median +
                  '（状態: ' + (d.state || '?') + '）';
  if (ui.alert('Flex予約の取り消し (2/2)',
               '子機0x' + hex + ' の以下の予約を取り消しますか？\n\n' + current,
               ui.ButtonSet.YES_NO) !== ui.Button.YES) return;
  cancelDownlink_(hex, 'メニューから手動取消');
  ui.alert('子機0x' + hex + ' の予約を取り消しました。');
}

function triggerCancelGatewayReservation() {
  const ui = SpreadsheetApp.getUi();
  const deviceId = CMD_STATUS_GATEWAY_IDS[0].id;
  const current = PropertiesService.getScriptProperties().getProperty('pending_cmd_' + deviceId) || 'none';
  if (current === 'none') {
    ui.alert('現在予約がありません。');
    return;
  }
  if (ui.alert('Gateway予約の取り消し', '以下の予約を取り消しますか？\n\n' + current,
               ui.ButtonSet.YES_NO) !== ui.Button.YES) return;
  PropertiesService.getScriptProperties().deleteProperty('pending_cmd_' + deviceId);
  refreshCmdStatusSheet();
  ui.alert('予約を取り消しました。');
}


// ================================
// カスタムメニュー
// ================================
// ★スタンドアロンのApps Scriptでは onOpen が自動実行されない。
//   スプレッドシート側で「拡張機能 > Apps Script」から別途バインドするか、
//   メニューを使わずエディタの関数実行で操作すること。
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
    .addToUi();

  SpreadsheetApp.getUi()
    .createMenu('Flex操作')
    .addItem('送信間隔を変更', 'triggerFlexSetInterval')
    .addItem('平均/メジアン回数を変更', 'triggerFlexSetAvgMedian')
    .addItem('ステータス確認', 'triggerFlexStatusCheck')
    .addSeparator()
    .addItem('予約を取り消す', 'triggerCancelFlexReservation')
    .addToUi();

  refreshCmdStatusSheet();
}


// ================================
// エディタから実行するダウンリンク予約（スタンドアロンのスクリプト用）
// ================================
// ★スタンドアロン（スプレッドシートに紐付いていない）スクリプトでは onOpen が動かず、
//   「Flex操作」メニューが表示されない。さらにメニュー用の関数は SpreadsheetApp.getUi() で
//   ダイアログを出すため、エディタから実行すると「このコンテキストからは getUi() を呼べない」
//   エラーになる。検証ではこちらを使う（ダイアログを使わない）。
//
// 【使い方】下の値を書き換えて保存 → エディタ上部の関数選択で実行したい関数を選んで「実行」。
//   ・エディタでの実行は保存済みの最新コードで動くので、デプロイし直す必要はない
//   ・結果は「cmd_status」「設定変更履歴」シートと、実行ログ（editorDlShow）で確認する
const EDITOR_DL_CHILD     = '0E';  // 対象の子機 DeviceID（16進2桁）
const EDITOR_DL_SLEEP_MIN = 3;     // 送信間隔（分）1〜1440
const EDITOR_DL_AVG       = 5;     // 平均回数 1〜255
const EDITOR_DL_MEDIAN    = 5;     // メジアン回数 1〜20（子機の上限）

// 送信間隔を変更する（平均・メジアンは直近の既知値を維持）
function editorDlSetInterval() {
  const known = getKnownChildSettings_(EDITOR_DL_CHILD);
  if (!known) { console.log('子機0x' + EDITOR_DL_CHILD + ' の現在値が不明です。先に editorDlStatusCheck を実行してください'); return; }
  const gwErr = childIntervalError_(EDITOR_DL_SLEEP_MIN);
  if (gwErr) { console.log(gwErr); return; }
  if (!queueDownlink_(EDITOR_DL_CHILD, EDITOR_DL_SLEEP_MIN, known.avg, known.median, 'エディタから予約（送信間隔）')) { console.error('予約に失敗しました。再度実行してください。'); return; }
  editorDlShow();
}

// 平均回数・メジアン回数を変更する（送信間隔は直近の既知値を維持）
function editorDlSetAvgMedian() {
  const known = getKnownChildSettings_(EDITOR_DL_CHILD);
  if (!known) { console.log('子機0x' + EDITOR_DL_CHILD + ' の現在値が不明です。先に editorDlStatusCheck を実行してください'); return; }
  if (!queueDownlink_(EDITOR_DL_CHILD, known.sleep, EDITOR_DL_AVG, EDITOR_DL_MEDIAN, 'エディタから予約（平均/メジアン）')) { console.error('予約に失敗しました。再度実行してください。'); return; }
  editorDlShow();
}

// 設定を変えずに、子機の現在値（送信間隔・平均・メジアン・WDT）を返させる
function editorDlStatusCheck() {
  if (!queueDownlink_(EDITOR_DL_CHILD, 0, 0, 0, 'エディタから予約（ステータス確認）', 'status')) { console.error('予約に失敗しました。再度実行してください。'); return; }
  editorDlShow();
}

// 予約を取り消す
function editorDlCancel() {
  if (!cancelDownlink_(EDITOR_DL_CHILD, 'エディタから手動取消')) {
    console.log('子機0x' + EDITOR_DL_CHILD + ' には予約がありません');
    return;
  }
  console.log('子機0x' + EDITOR_DL_CHILD + ' の予約を取り消しました');
}

// 予約の現在の状態を実行ログに出す
function editorDlShow() {
  const d = dlGet_(EDITOR_DL_CHILD);
  console.log('子機0x' + EDITOR_DL_CHILD + ': ' + (d ? JSON.stringify(d) : '予約なし') +
              (d ? '\n表示: ' + (dlStatusLabel_(d) || '予約中（Gatewayの取得待ち）') : ''));
}


// ================================
// スプレッドシートからのダウンリンク予約（子機シートの「ダウンリンク設定」欄）  2026-09-23
// ================================
// ★スクリプトはスタンドアロンのまま（顧客からコードが見えない）。スタンドアロンのスクリプトからでも
//   「インストール型の編集トリガー」を特定のスプレッドシートに設定できるので、
//   シートのチェックボックスを押した瞬間に予約できる。
//
// 【最初に1回だけ（エディタから実行）】
//   1. setupDownlinkBlocks        … 「雛形」と DL_BLOCK_EXTRA_SHEETS（既定: 0014）に設定欄を作る
//                                   （mail-11〜mail-20 の列を片付けて、Q列から設定欄を置く）
//   2. installDownlinkEditTrigger … チェックボックスを押したら予約する編集トリガーを作る
//   3. 設定欄の範囲を「範囲の保護」でコクリエのアカウントだけ編集可にする（顧客に押させない）
//
// 【設定欄】（見出しの文字「ダウンリンク設定」で位置を探すので、欄ごと動かしても動く）
//        Q                R               S
//    1   ダウンリンク設定
//    2   DeviceID         0x0E（自動）
//    3   項目             子機の現在値     変更後（空欄＝変更しない）
//    4   送信間隔（分）    3               [  ]
//    5   平均回数          5               [  ]
//    6   メジアン回数      5               [  ]
//    7   WDT（分）         18
//    8   設定を変更する    [☐]
//    9   ステータス確認    [☐]            … 設定を変えずに子機の現在値を返させる
//   10   状態             完了　WDT=18分（自動）
//   11   最終更新         2026/09/23 13:38（自動）
//   「子機の現在値」は、子機が確認応答で返した実際の適用値。GAS が自動で書き込む。
const DL_BLOCK_KEYWORD = 'ダウンリンク設定';
const DL_BLOCK_DEFAULT_ROW = 1;    // setupDownlinkBlocks で置く位置（Q1）
const DL_BLOCK_DEFAULT_COL = 17;   // Q列
const DL_BLOCK_PROBE_ROWS = 40;
const DL_BLOCK_PROBE_COLS = 45;
// 見出しセルからの相対行
const DLB_ID = 1, DLB_HEAD = 2, DLB_SLEEP = 3, DLB_AVG = 4, DLB_MEDIAN = 5, DLB_WDT = 6,
      DLB_APPLY = 7, DLB_STATUS = 8, DLB_STATE = 9, DLB_UPDATED = 10, DLB_CANCEL = 11;   // ★2026-09-24 FW24: 予約取消を追加
const DL_MEDIAN_MAX = 20;          // 子機ファームのメジアン回数の上限（MEASURE_COUNT_MAX）

// 設定欄の見出しセルの位置を返す。無ければ null。
function findDownlinkBlock_(sheet) {
  const nRow = Math.min(DL_BLOCK_PROBE_ROWS, sheet.getMaxRows());
  const nCol = Math.min(DL_BLOCK_PROBE_COLS, sheet.getMaxColumns());
  const v = sheet.getRange(1, 1, nRow, nCol).getValues();
  for (let r = 0; r < v.length; r++) {
    for (let c = 0; c < v[r].length; c++) {
      if (String(v[r][c]).trim() === DL_BLOCK_KEYWORD) return { row: r + 1, col: c + 1 };
    }
  }
  return null;
}

// シート名 → DeviceID（「シート名編集」を逆引き）。無ければ null。
function deviceIdForSheet_(sheetName) {
  const map = loadDeviceSheetMapFromSheet();
  let found = null;
  Object.keys(map).forEach(function (k) { if (map[k] === sheetName) found = Number(k); });
  return found;
}

// 1枚のシートに設定欄を作る。
function setupDownlinkBlockOnSheet_(sheet) {
  if (findDownlinkBlock_(sheet)) {
    UI_designDownlink_(sheet);   // ★2026-09-24 既存値を保ったまま見た目を更新
    console.log('設定欄は既にあります: ' + sheet.getName());
    return false;
  }
  // mail-11〜mail-20 の列を片付ける（見出しが mail-11 のときだけ。別の用途の列は消さない）
  const layout = findAlertLayout_(sheet);
  if (layout) {
    const mail11Col = layout.keyCol + ALERT_COL_MAIL_FROM + 10;
    if (String(sheet.getRange(layout.settingHeaderRow, mail11Col).getValue()).trim() === 'mail-11') {
      // 設定行（d1〜d25）の範囲だけを片付ける。その下の「補正用」などの見出しは残す
      let lastRow = layout.settingHeaderRow;
      const keys = sheet.getRange(layout.settingHeaderRow + 1, layout.keyCol, ALERT_SETTING_MAX_ROWS, 1).getValues();
      for (let i = 0; i < keys.length; i++) {
        if (/^d\d+$/.test(String(keys[i][0]).trim())) lastRow = layout.settingHeaderRow + 1 + i; else break;
      }
      sheet.getRange(layout.settingHeaderRow, mail11Col, lastRow - layout.settingHeaderRow + 1, 10)
           .clearContent().clearFormat();
      console.log(sheet.getName() + ': mail-11〜mail-20 の列を片付けました');
    }
  }

  const r0 = DL_BLOCK_DEFAULT_ROW, c0 = DL_BLOCK_DEFAULT_COL;
  const labels = [
    [DL_BLOCK_KEYWORD, '', ''],
    ['DeviceID', '', ''],
    ['項目', '子機の現在値', '変更後（空欄＝変更しない）'],
    ['送信間隔（分）', '', ''],
    ['平均回数', '', ''],
    ['メジアン回数', '', ''],
    ['異常時の自動再起動（分）', '', ''],
    ['設定を変更する', '', ''],
    ['ステータス確認', '', ''],
    ['状態', '', ''],
    ['最終更新', '', ''],
    ['予約を取り消す', '', ''],
  ];
  const rng = sheet.getRange(r0, c0, labels.length, 3);
  rng.setValues(labels).setBorder(true, true, true, true, true, true);
  sheet.getRange(r0, c0, 1, 3).merge().setFontWeight('bold').setBackground('#d9ead3');
  sheet.getRange(r0 + DLB_HEAD, c0, 1, 3).setFontWeight('bold').setBackground('#f3f3f3');
  sheet.getRange(r0 + DLB_SLEEP, c0 + 2, 3, 1).setBackground('#fff2cc');    // 入力欄
  sheet.getRange(r0 + DLB_APPLY, c0 + 1, 2, 1).insertCheckboxes();
  sheet.getRange(r0 + DLB_CANCEL, c0 + 1).insertCheckboxes();
  sheet.getRange(r0 + DLB_STATE, c0 + 1, 1, 2).merge();
  sheet.getRange(r0 + DLB_UPDATED, c0 + 1, 1, 2).merge();
  const id = deviceIdForSheet_(sheet.getName());
  if (id !== null) {
    const hex = ('0' + id.toString(16).toUpperCase()).slice(-2);
    sheet.getRange(r0 + DLB_ID, c0 + 1).setValue('0x' + hex);
    syncDownlinkBlock_(hex);
  }
  UI_designDownlink_(sheet);   // ★2026-09-24 入力欄と自動欄を色・保護で区別する
  console.log(sheet.getName() + ': 設定欄を作りました（' + sheet.getRange(r0, c0).getA1Notation() + '〜）');
  return true;
}

// 設定欄を作るシート（雛形以外）。★ここに書いたシートだけが対象。mail-11〜20 の列を片付けるので、
//   それらの列を使っているシートは入れないこと。新しい子機シートは雛形の複製で作られるので追加不要。
const DL_BLOCK_EXTRA_SHEETS = ['0014'];

// 【エディタから1回実行】「雛形」と DL_BLOCK_EXTRA_SHEETS のシートに設定欄を作る。
function setupDownlinkBlocks() {
  const ss = getSpreadsheet();
  const names = [TEMPLATE_SHEET_NAME].concat(DL_BLOCK_EXTRA_SHEETS);
  names.forEach(function (name) {
    const sheet = ss.getSheetByName(name);
    if (!sheet) { console.log('シートが無いのでスキップ: ' + name); return; }
    setupDownlinkBlockOnSheet_(sheet);
  });
  upgradeDownlinkBlocks();
  // ★2026-09-24 有効登録済みの既存ブロックにも共通デザインを適用する。
  UI_downlinkSheets_().forEach(UI_designDownlink_);
}

// ★2026-09-24 FW24: 【エディタから1回実行】既存の設定欄に取消行だけ足す（入力値は残す）。
function upgradeDownlinkBlocks() {
  const ss = getSpreadsheet();
  const map = loadDeviceSheetMapFromSheet();
  const names = [TEMPLATE_SHEET_NAME].concat(Object.keys(map).map(function (k) { return map[k]; }));
  Array.from(new Set(names)).forEach(function (name) {
    const sheet = ss.getSheetByName(name);
    if (!sheet) return;
    const b = findDownlinkBlock_(sheet);
    if (!b || sheet.getRange(b.row + DLB_CANCEL, b.col).getValue() === '予約を取り消す') return;
    sheet.getRange(b.row + DLB_CANCEL, b.col, 1, 3).setValues([['予約を取り消す', '', '']])
         .setBorder(true, true, true, true, true, true);
    sheet.getRange(b.row + DLB_CANCEL, b.col + 1).insertCheckboxes();
  });
}

// 【エディタから1回実行】チェックボックスを押したら予約する編集トリガーを作る。
function installDownlinkEditTrigger() {
  const exists = ScriptApp.getProjectTriggers().some(function (t) {
    return t.getHandlerFunction() === 'onDownlinkSheetEdit';
  });
  if (exists) { console.log('編集トリガーは既に設定済みです'); return; }
  ScriptApp.newTrigger('onDownlinkSheetEdit').forSpreadsheet(SPREADSHEET_ID).onEdit().create();
  console.log('編集トリガーを設定しました（対象: ' + SPREADSHEET_ID + '）');
}

// 子機シートの設定欄に、予約の状態と子機の現在値を書き戻す。
// ★ここでの失敗で Gateway への応答や予約処理を止めない（try で囲む）。
function syncDownlinkBlock_(childHex) {
  try {
    const name = loadDeviceSheetMapFromSheet()[parseInt(childHex, 16)];
    if (!name) return;
    const sheet = getSpreadsheet().getSheetByName(name);
    if (!sheet) return;
    const b = findDownlinkBlock_(sheet);
    if (!b) return;
    const col = b.col + 1;
    sheet.getRange(b.row + DLB_ID, col).setValue('0x' + childHex);
    const d = dlGet_(childHex);
    const known = getKnownChildSettings_(childHex);
    if (known) {
      sheet.getRange(b.row + DLB_SLEEP, col, 4, 1)
           .setValues([[known.sleep], [known.avg], [known.median], [known.wdt || '']]);
    }
    let state = '—';
    if (d) {
      const base = { queued: '予約中（Gatewayの取得待ち）', sent: '送信済み（子機の確認待ち）',
                     done: '完了', failed: '失敗' }[d.state] || d.state;
      state = (d.state === 'done' || d.state === 'failed') ? dlStatusLabel_(d) : base;
      if (d.mode === 'status' && d.state !== 'done' && d.state !== 'failed') state += '（ステータス確認）';
    }
    sheet.getRange(b.row + DLB_STATE, col).setValue(state);
    sheet.getRange(b.row + DLB_UPDATED, col).setValue(d && d.updated ? new Date(d.updated) : '');
  } catch (err) {
    console.error('[DL-BLOCK] 設定欄の更新に失敗（処理は継続）: ' + err);
  }
  syncGatewayBlock_();   // 子機の間隔が変わると「子機の最短間隔」も変わるため
}

// 編集トリガーから呼ばれる。「設定を変更する」「ステータス確認」のチェックで予約する。
// ★スクリプトからの書き込みではトリガーは発火しないので、チェックを外す処理で再帰しない。
function onDownlinkSheetEdit(e) {
  if (!e || !e.range) return;
  const range = e.range;
  if (range.getNumRows() !== 1 || range.getNumColumns() !== 1) return;
  // ★2026-09-24 e.value は編集のしかた（貼り付け・元に戻す等）によって渡されないことがある。
  //   そのときに黙って抜けると「チェックが付いたまま予約されない」状態になる（0014 で発生）ので、セルの値を直接読む。
  const checked = (e.value !== undefined) ? (String(e.value).toUpperCase() === 'TRUE') : (range.getValue() === true);
  if (!checked) return;
  const sheet = range.getSheet();
  if (sheet.getName() === GW_SHEET_NAME) return onGatewaySheetEdit_(sheet, range);
  const b = findDownlinkBlock_(sheet);
  if (!b || range.getColumn() !== b.col + 1) return;
  const isApply  = range.getRow() === b.row + DLB_APPLY;
  const isStatus = range.getRow() === b.row + DLB_STATUS;
  const isCancel = range.getRow() === b.row + DLB_CANCEL;
  if (!isApply && !isStatus && !isCancel) return;

  const stateCell = sheet.getRange(b.row + DLB_STATE, b.col + 1);
  function fail(msg) { stateCell.setValue(msg); range.setValue(false); }

  const id = deviceIdForSheet_(sheet.getName());
  if (id === null) return fail('エラー: 「シート名編集」にこのシートが有効として登録されていません');
  const hex = ('0' + id.toString(16).toUpperCase()).slice(-2);
  if (CMD_STATUS_CHILD_IDS.indexOf(hex) < 0) return fail('エラー: 0x' + hex + ' はダウンリンク対象外の DeviceID です');

  if (isCancel) {
    const pending = dlGet_(hex);
    if (!pending) return fail('予約はありません');
    const ok = cancelDownlink_(hex, 'シートから手動取消');
    stateCell.setValue(ok ? '予約を取り消しました' : 'エラー: 予約の取消に失敗しました');
    range.setValue(false);
    return;
  }

  if (isStatus) {
    if (!queueDownlink_(hex, 0, 0, 0, 'シートから予約（ステータス確認）', 'status')) return fail('エラー: 予約に失敗しました。再度操作してください');
    range.setValue(false);
    return;
  }

  // 空欄の項目は「子機が確認した現在値」を使う。現在値が不明なら空欄は許さない
  //   （既定値で埋めると、意図せず送信間隔などを変えてしまうため）。
  const cur = getKnownChildSettings_(hex);
  const input = sheet.getRange(b.row + DLB_SLEEP, b.col + 2, 3, 1).getValues();
  function pick(v, curVal, min, max, label) {
    if (v === '' || v === null) {
      if (curVal === undefined) throw new Error('子機の現在値が不明なため' + label + 'を入力してください（先にステータス確認でも可）');
      return curVal;
    }
    const n = Number(v);
    if (!Number.isInteger(n) || n < min || n > max) throw new Error(label + 'は' + min + '〜' + max + 'の整数で入力してください');
    return n;
  }
  let sleep, avg, median;
  try {
    sleep  = pick(input[0][0], cur ? cur.sleep  : undefined, 1, 1440, '送信間隔');
    avg    = pick(input[1][0], cur ? cur.avg    : undefined, 1, 255,  '平均回数');
    median = pick(input[2][0], cur ? cur.median : undefined, 1, DL_MEDIAN_MAX, 'メジアン回数');
  } catch (err) {
    return fail('入力エラー: ' + err.message);
  }
  // ★2026-09-23 子機の送信間隔は Gateway の送信間隔より短くできない（短いと Gateway が送る前に上書きされ、データが抜ける）
  if (!cur || sleep !== cur.sleep) {
    const gwErr = childIntervalError_(sleep);
    if (gwErr) return fail('入力エラー: ' + gwErr);
  }
  if (!queueDownlink_(hex, sleep, avg, median, 'シートから予約')) return fail('エラー: 予約に失敗しました。再度操作してください');
  sheet.getRange(b.row + DLB_SLEEP, b.col + 2, 3, 1).clearContent();   // 受け付けたので入力欄を空に戻す
  range.setValue(false);
}


// ================================
// Gateway の送信間隔（「Gateway設定」シート）  2026-09-23（Gateway FW17〜）
// ================================
// 【ルール】Gateway の送信間隔 ≦ 子機のいちばん短い送信間隔 ×（GW_RECORDS_PER_CHILD − 1）
//   Gateway FW18 は子機1台につき最大 GW_RECORDS_PER_CHILD 件ためて、送信間隔ごとにまとめて送る。
//   それを超えると古いものから捨てる（Gateway ログの「あふれ破棄」）。そこで
//     ・Gateway の間隔を、上の上限より長くは予約できない
//     ・子機の間隔を、Gateway の間隔 ÷（GW_RECORDS_PER_CHILD − 1）より短くは予約できない
//   の両方をシート側で止める。子機の間隔は「子機が確認した値」と「予約中の値」の短いほうで判定する。
//   （FW17 までは Gateway が1台1件しか持たなかったため「Gateway ≦ 子機」だった）
//
// 【完了の判定】Gateway は予約確認（check_cmd）のたびに現在の間隔（gw_interval）を知らせてくる。
//   予約した値と一致した時点で予約を消して完了にする（Gateway からの個別の確認応答は不要）。
//
// 【最初に1回だけ（エディタから実行）】setupGatewayBlock … 「Gateway設定」シートと設定欄を作る
//   編集トリガーは子機と共通（installDownlinkEditTrigger）。範囲の保護も子機の設定欄と同様にかける。
//
//        A                    B                C
//    1   Gateway設定
//    2   Gateway ID           ipec_gw
//    3   項目                 現在値            変更後
//    4   送信間隔（分）        3                [  ]
//    5   子機の最短間隔（分）   10分（0x0E）→ Gateway は 50分まで設定可
//    6   アプリWDT（分）       7
//    7   設定を変更する        [☐]
//    8   状態                 予約中 / —
//    9   最終確認             Gateway が最後に現在値を知らせてきた日時
// Gateway が子機1台につきためられる件数（Gateway ファームの RECORDS_PER_DEVICE と同じ値）。
// Gateway の間隔は「子機の最短間隔 ×（この値 − 1）」まで許す。−1 は子機の送信ずらし（±10秒）で
// 1サイクルに1件多く入ることがあるための余裕。
const GW_RECORDS_PER_CHILD = 6;
const GW_MAX_RECORDS = 96;   // ★2026-09-24 FW21: Gateway の MAX_RECORDS と同じ全体容量
const GW_SHEET_NAME = 'Gateway設定';
const GW_BLOCK_KEYWORD = 'Gateway設定';
const GWB_ID = 1, GWB_HEAD = 2, GWB_INTERVAL = 3, GWB_CHILDMIN = 4, GWB_WDT = 5,
      GWB_APPLY = 6, GWB_STATE = 7, GWB_UPDATED = 8;

// ★2026-09-24 FW24: スタンドアロン GAS でも編集トリガーで Gateway を操作できる。
const GWOPS_KEYWORD = 'Gateway操作';
const GWOPS_PENDING = 1, GWOPS_PAUSED = 2, GWOPS_FIRST = 3, GWOPS_CANCEL = 10, GWOPS_RESULT = 11;
const GWOPS_COMMANDS = [
  ['reset', 'リモートリセット'], ['stop', 'データ送信を停止'], ['start', 'データ送信を再開'],
  ['send_now', '今すぐ送信'], ['status_now', 'ステータス確認'], ['rtc_resync', 'RTC再同期'],
  ['log_dump', '診断ログを吸い上げ'],
];

function gatewayPausedLabel_(id) {
  const paused = PropertiesService.getScriptProperties().getProperty('gw_paused_' + (id || GATEWAY_DEVICE_ID));
  return paused === '1' ? '★送信停止中' : (paused === '0' ? '送信中' : '不明（ステータス確認で更新）');
}

// 予約枠は1つ。同じ予約と interval → interval の変更だけは従来どおり許す。
function gatewayCmdConflict_(cmd, id) {
  const pending = PropertiesService.getScriptProperties().getProperty('pending_cmd_' + (id || GATEWAY_DEVICE_ID));
  if (!pending || pending === cmd || (/^interval:\d+$/.test(pending) && /^interval:\d+$/.test(cmd))) return '';
  return '予約中のコマンド（' + pending + '）があります。完了を待つか『予約を取り消す』で取り消してから操作してください';
}

function queueGatewayCmd_(cmd, label, note) {
  const lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);
    const err = gatewayCmdConflict_(cmd);
    if (err) return err;
    PropertiesService.getScriptProperties().setProperty('pending_cmd_' + GATEWAY_DEVICE_ID, cmd);
    gwLog_('Gatewayコマンド 予約', label + '（' + cmd + '）', note);
  } catch (err) {
    return '予約に失敗しました: ' + err.message;
  } finally {
    try { lock.releaseLock(); } catch (e2) {}
  }
  syncGatewayBlock_();
  refreshCmdStatusSheet();
  return '';
}

function cancelGatewayCmd_(note) {
  const props = PropertiesService.getScriptProperties();
  const lock = LockService.getScriptLock();
  let pending, deferred;
  try {
    lock.waitLock(10000);
    pending = props.getProperty('pending_cmd_' + GATEWAY_DEVICE_ID);
    const raw = props.getProperty('gw_batch_deferred');
    deferred = raw ? JSON.parse(raw) : null;
    props.deleteProperty('pending_cmd_' + GATEWAY_DEVICE_ID);
    gwLog_('Gatewayコマンド 取消', pending || '予約なし', note);
    // Gateway の適用待ちも、子機完了後に Gateway を予約する保留も中止する。
    if (deferred) cancelDeferredBatch_('Gateway の予約を手動取消');
  } finally {
    lock.releaseLock();
  }
  syncGatewayBlock_();
  refreshCmdStatusSheet();
  return pending || deferred ? '予約を取り消しました' : '予約はありません';
}

function gatewayCommandResult_(cmd, label) {
  return label + 'を予約しました。' + (cmd === 'reset'
    ? '再送待ちのデータは消えます。次の予約確認（最大で送信間隔ぶん）で再起動します'
    : '次の予約確認（最大で送信間隔ぶん）で実行します');
}

function setupGatewayOpsBlock_(sheet) {
  if (findBlockByKeyword_(sheet, GWOPS_KEYWORD)) return;
  const bb = findBlockByKeyword_(sheet, GWBATCH_KEYWORD);
  const r0 = bb.row + GWBATCH_RESULT + 2, c0 = bb.col;
  const labels = [[GWOPS_KEYWORD, ''], ['現在の予約', ''], ['送信の状態', '']]
    .concat(GWOPS_COMMANDS.map(function (c) { return [c[1], '']; }))
    .concat([['予約を取り消す', ''], ['結果', '']]);
  sheet.getRange(r0, c0, labels.length, 2).setValues(labels).setBorder(true, true, true, true, true, true);
  sheet.getRange(r0, c0, 1, 3).merge().setFontWeight('bold').setBackground('#cfe2f3');
  sheet.getRange(r0 + GWOPS_FIRST, c0 + 1, GWOPS_COMMANDS.length + 1, 1).insertCheckboxes();
  [GWOPS_PENDING, GWOPS_PAUSED, GWOPS_RESULT].forEach(function (r) {
    sheet.getRange(r0 + r, c0 + 1, 1, 2).merge().setWrap(true);
  });
}

function onGatewayOpsEdit_(sheet, b, range) {
  const row = range.getRow() - b.row;
  if (row < GWOPS_FIRST || row > GWOPS_CANCEL) return;
  let result;
  try {
    if (row === GWOPS_CANCEL) result = cancelGatewayCmd_('シートから手動取消');
    else {
      const c = GWOPS_COMMANDS[row - GWOPS_FIRST];
      const err = queueGatewayCmd_(c[0], c[1], 'シートから予約');
      result = err ? ('エラー: ' + err) : gatewayCommandResult_(c[0], c[1]);
    }
  } catch (err) {
    result = 'エラー: ' + err.message;
  }
  sheet.getRange(b.row + GWOPS_RESULT, b.col + 1).setValue(result);
  range.setValue(false);
}

function gwAppliedKey_(id) { return 'gw_applied_' + id; }

// Gateway が知らせてきた現在の送信間隔（分）。不明なら null。
function getGatewayAppliedInterval_(id) {
  const raw = PropertiesService.getScriptProperties().getProperty(gwAppliedKey_(id || GATEWAY_DEVICE_ID));
  if (!raw) return null;
  try { return JSON.parse(raw).interval || null; } catch (e) { return null; }
}

// 予約中の送信間隔（分）。無ければ null。
function getGatewayPendingInterval_(id) {
  const cmd = PropertiesService.getScriptProperties().getProperty('pending_cmd_' + (id || GATEWAY_DEVICE_ID)) || '';
  const m = /^interval:(\d+)$/.exec(cmd);
  return m ? parseInt(m[1], 10) : null;
}

function gwLog_(kind, detail, note) {
  try {
    getDownlinkLogSheet_().appendRow([new Date(), GATEWAY_DEVICE_ID, '', kind, detail || '', note || '', GATEWAY_GROUP]);
  } catch (e) {
    console.error('設定変更履歴 追記に失敗: ' + e);
  }
}

// Gateway が知らせてきた現在の送信間隔を保存し、予約と一致したら完了にする。
function recordGatewayInterval_(id, minutes) {
  if (isNaN(minutes) || minutes < 1) return;
  const props = PropertiesService.getScriptProperties();
  let changed = false;
  const lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);
    const prev = getGatewayAppliedInterval_(id);
    props.setProperty(gwAppliedKey_(id), JSON.stringify({ interval: minutes, at: new Date().toISOString() }));
    if (prev !== minutes) {
      changed = true;
      if (prev !== null) gwLog_('Gateway間隔 変更確認', prev + '分 → ' + minutes + '分', 'Gateway からの報告');
    }
    const pendingKey = 'pending_cmd_' + id;
    // ★FW21: 現在値と同じ入力で別予約を取り消した場合も、次の実機報告を2段目の確認に使う。
    const deferredRaw = id === GATEWAY_DEVICE_ID ? props.getProperty('gw_batch_deferred') : null;
    const deferred = deferredRaw ? JSON.parse(deferredRaw) : null;
    if (deferred && deferred.type === 'children_after_gw' && deferred.gw === minutes) changed = true;
    if (props.getProperty(pendingKey) === 'interval:' + minutes) {
      // 別の予約の完了で pending が消える前に、保留との不一致を確認する。
      if (deferred && deferred.type === 'children_after_gw' && deferred.gw !== minutes) {
        cancelDeferredBatch_('Gateway の予約が入れ替わりました');
      }
      props.deleteProperty(pendingKey);
      gwLog_('Gateway間隔 完了', '送信間隔=' + minutes + '分', 'Gateway が適用済みの値を報告');
      changed = true;
    }
  } catch (err) {
    console.log('Gateway interval lock error: ' + err);
    return;
  } finally {
    try { lock.releaseLock(); } catch (e2) {}
  }
  // ★毎サイクル呼ばれるので、変化があったときだけシートを書き換える（応答を遅くしないため）
  if (changed && id === GATEWAY_DEVICE_ID) {
    processDeferredBatch_();
    syncGatewayBlock_();
    refreshCmdStatusSheet();
  }
}

// Gateway の群に属し、「シート名編集」で有効な子機のうち、いちばん短い送信間隔を返す。
//   戻り値: { min: 分 または null, hex: その子機, unknown: 現在値が不明な子機の一覧 }
function minChildInterval_() {
  const map = loadDeviceSheetMapFromSheet();
  let min = null, minHex = '';
  const unknown = [];
  Object.keys(map).forEach(function (k) {
    const id = Number(k);
    if ((id >> 5) !== GATEWAY_GROUP) return;
    const hex = ('0' + id.toString(16).toUpperCase()).slice(-2);
    if (CMD_STATUS_CHILD_IDS.indexOf(hex) < 0) return;
    const vals = [];
    const known = getKnownChildSettings_(hex);
    if (known && known.sleep) vals.push(known.sleep);
    const d = dlGet_(hex);
    if (d && (d.state === 'queued' || d.state === 'sent') && d.mode !== 'status' && d.sleep) vals.push(d.sleep);
    if (vals.length === 0) { unknown.push('0x' + hex); return; }
    const v = Math.min.apply(null, vals);
    if (min === null || v < min) { min = v; minHex = hex; }
  });
  return { min: min, hex: minHex, unknown: unknown };
}

// ★2026-09-24 FW21: 確認済みと予約中の両方を集め、移行中にも厳しい側で判定する。
function childIntervalValues_(hex) {
  const vals = [];
  const known = getKnownChildSettings_(hex);
  if (known && known.sleep) vals.push(known.sleep);
  const d = dlGet_(hex);
  if (d && (d.state === 'queued' || d.state === 'sent') && d.mode !== 'status' && d.sleep) vals.push(d.sleep);
  return vals;
}

function groupChildIntervals_() {
  let vals = [];
  groupChildHexes_().forEach(function (hex) { vals = vals.concat(childIntervalValues_(hex)); });
  return vals;
}

// 子機1台ごとの上限に加え、送信ずらしの余裕を含めた群全体の件数も確認する。
function intervalRuleError_(gwMin, childSleeps) {
  if (!gwMin) return '';   // Gateway の現在値が不明（FW16以前）なら判定しない
  const k = GW_RECORDS_PER_CHILD - 1;
  let total = 0;
  for (let i = 0; i < childSleeps.length; i++) {
    const sleep = childSleeps[i];
    if (gwMin > sleep * k) {
      return 'Gateway の送信間隔（' + gwMin + '分）が子機の送信間隔（' + sleep + '分）× ' + k +
             'を超えます。Gateway を ' + (sleep * k) + '分以下にするか、子機を ' + Math.ceil(gwMin / k) + '分以上にしてください';
    }
    total += Math.ceil(gwMin / sleep) + 1;
  }
  if (total > GW_MAX_RECORDS) return 'Gateway の全体容量を超えます（見積もり' + total + '件 / 最大' + GW_MAX_RECORDS + '件）。送信間隔を短くしてください';
  return '';
}

function childIntervalError_(sleepMin) {
  const vals = [getGatewayAppliedInterval_(), getGatewayPendingInterval_()].filter(function (v) { return v; });
  if (vals.length === 0) return '';
  return intervalRuleError_(Math.max.apply(null, vals), groupChildIntervals_().concat([sleepMin]));
}

function gatewayIntervalError_(minutes) {
  if (!Number.isInteger(minutes) || minutes < 1 || minutes > 1440) return '送信間隔は1〜1440の整数で入力してください';
  return intervalRuleError_(minutes, groupChildIntervals_());
}

// Gateway の送信間隔を予約する。エラーならその文言を返す（予約はしない）。
function queueGatewayInterval_(minutes, note) {
  const err = gatewayCmdConflict_('interval:' + minutes) || gatewayIntervalError_(minutes);
  if (err) return err;
  const c = minChildInterval_();
  cancelDeferredBatch_('Gateway の個別予約');
  try {
    setGatewayPending_(minutes, note + (c.unknown.length ? '　※現在値が不明な子機: ' + c.unknown.join(', ') : ''));
  } catch (err) { return err.message; }
  return '';
}

// 容量判定済みの Gateway 間隔を予約する。★FW24: 予約枠の衝突はここでも判定する。
function setGatewayPending_(minutes, note) {
  const lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);
    const err = gatewayCmdConflict_('interval:' + minutes);
    if (err) throw new Error(err);
    PropertiesService.getScriptProperties().setProperty('pending_cmd_' + GATEWAY_DEVICE_ID, 'interval:' + minutes);
  } finally {
    lock.releaseLock();
  }
  gwLog_('Gateway間隔 予約', '送信間隔=' + minutes + '分（通信異常時の自動再起動=' + Math.floor(Math.max(minutes * 2.5, minutes + 10)) + '分）', note);
  syncGatewayBlock_();
  refreshCmdStatusSheet();
}

// 【エディタから1回実行】「Gateway設定」シートと設定欄を作る。
function setupGatewayBlock() {
  const ss = getSpreadsheet();
  let sheet = ss.getSheetByName(GW_SHEET_NAME);
  if (!sheet) sheet = ss.insertSheet(GW_SHEET_NAME);
  if (findBlockByKeyword_(sheet, GW_BLOCK_KEYWORD)) {
    console.log('Gateway の設定欄は既にあります');
    setupGatewayBatchBlock_(sheet);
    const hadOps = findBlockByKeyword_(sheet, GWOPS_KEYWORD);
    setupGatewayOpsBlock_(sheet);
    // ★2026-09-24 再実行では既存値を更新しない。今回追加した操作欄だけ初期表示する。
    if (!hadOps) {
      const ops = findBlockByKeyword_(sheet, GWOPS_KEYWORD);
      sheet.getRange(ops.row + GWOPS_PENDING, ops.col + 1).setValue(
        PropertiesService.getScriptProperties().getProperty('pending_cmd_' + GATEWAY_DEVICE_ID) || '—');
      sheet.getRange(ops.row + GWOPS_PAUSED, ops.col + 1).setValue(gatewayPausedLabel_());
    }
    UI_designGateway_(sheet);
    return;
  }
  const labels = [
    [GW_BLOCK_KEYWORD, '', ''],
    ['Gateway ID', GATEWAY_DEVICE_ID, ''],
    ['項目', '現在値', '変更後（分）'],
    ['送信間隔（分）', '', ''],
    ['子機の最短間隔（分）', '', ''],
    ['通信異常時の自動再起動（分）', '', ''],
    ['設定を変更する', '', ''],
    ['状態', '', ''],
    ['最終確認', '', ''],
  ];
  sheet.getRange(1, 1, labels.length, 3).setValues(labels).setBorder(true, true, true, true, true, true);
  sheet.getRange(1, 1, 1, 3).merge().setFontWeight('bold').setBackground('#cfe2f3');
  sheet.getRange(1 + GWB_HEAD, 1, 1, 3).setFontWeight('bold').setBackground('#f3f3f3');
  sheet.getRange(1 + GWB_INTERVAL, 3).setBackground('#fff2cc');   // 入力欄
  sheet.getRange(1 + GWB_APPLY, 2).insertCheckboxes();
  sheet.getRange(1 + GWB_STATE, 2, 1, 2).merge();
  sheet.getRange(1 + GWB_UPDATED, 2, 1, 2).merge();
  sheet.getRange(1 + GWB_CHILDMIN, 2, 1, 2).merge();
  sheet.setColumnWidth(1, 160); sheet.setColumnWidth(2, 160); sheet.setColumnWidth(3, 120);
  setupGatewayBatchBlock_(sheet);
  setupGatewayOpsBlock_(sheet);
  syncGatewayBlock_();
  UI_designGateway_(sheet);   // ★2026-09-24 3ブロックを共通の見た目にする
  console.log('「' + GW_SHEET_NAME + '」シートに設定欄を作りました');
}

// 見出し文字で設定欄の位置を探す。無ければ null。
function findBlockByKeyword_(sheet, keyword) {
  const nRow = Math.min(DL_BLOCK_PROBE_ROWS, sheet.getMaxRows());
  const nCol = Math.min(DL_BLOCK_PROBE_COLS, sheet.getMaxColumns());
  const v = sheet.getRange(1, 1, nRow, nCol).getValues();
  for (let r = 0; r < v.length; r++) {
    for (let c = 0; c < v[r].length; c++) {
      if (String(v[r][c]).trim() === keyword) return { row: r + 1, col: c + 1 };
    }
  }
  return null;
}

// 「Gateway設定」シートに現在値・子機の最短間隔・予約状態を書き戻す。失敗しても処理は止めない。
function syncGatewayBlock_() {
  try {
    const sheet = getSpreadsheet().getSheetByName(GW_SHEET_NAME);
    if (!sheet) return;
    const ops = findBlockByKeyword_(sheet, GWOPS_KEYWORD);
    if (ops) {
      sheet.getRange(ops.row + GWOPS_PENDING, ops.col + 1).setValue(
        PropertiesService.getScriptProperties().getProperty('pending_cmd_' + GATEWAY_DEVICE_ID) || '—');
      sheet.getRange(ops.row + GWOPS_PAUSED, ops.col + 1).setValue(gatewayPausedLabel_());
    }
    const b = findBlockByKeyword_(sheet, GW_BLOCK_KEYWORD);
    if (!b) return;
    const col = b.col + 1;
    const raw = PropertiesService.getScriptProperties().getProperty(gwAppliedKey_(GATEWAY_DEVICE_ID));
    let applied = null, at = null;
    if (raw) { try { const o = JSON.parse(raw); applied = o.interval; at = o.at; } catch (e) {} }
    const pending = getGatewayPendingInterval_(GATEWAY_DEVICE_ID);
    const c = minChildInterval_();
    let childText = (c.min !== null)
      ? (c.min + '分（0x' + c.hex + '）→ Gateway は ' + (c.min * (GW_RECORDS_PER_CHILD - 1)) + '分まで設定可')
      : '不明';
    if (c.unknown.length) childText += '　※現在値不明: ' + c.unknown.join(', ');

    sheet.getRange(b.row + GWB_ID, col).setValue(GATEWAY_DEVICE_ID);
    sheet.getRange(b.row + GWB_INTERVAL, col).setValue(applied || '不明（FW17以降で表示）');
    sheet.getRange(b.row + GWB_CHILDMIN, col).setValue(childText);
    sheet.getRange(b.row + GWB_WDT, col).setValue(applied ? Math.floor(Math.max(applied * 2.5, applied + 10)) : '');
    let state = '—';
    if (pending !== null) {
      state = '予約中: 送信間隔 ' + pending + '分（Gateway の次の予約確認で適用。最大で現在の送信間隔ぶん待ちます）';
    }
    const deferred = PropertiesService.getScriptProperties().getProperty('gw_batch_deferred');
    if (deferred) {
      const d = JSON.parse(deferred);
      state += '　一括変更の2段目待ち: ' + (d.type === 'gw_after_children'
        ? ('子機の完了後に Gateway ' + d.gw + '分を予約') : ('Gateway ' + d.gw + '分の確認後に子機を予約'));
    }
    if (applied && intervalRuleError_(applied, groupChildIntervals_())) {
      state += '　⚠ Gateway の間隔が長すぎるため、ためきれずに古い計測が捨てられます';
    }
    sheet.getRange(b.row + GWB_STATE, col).setValue(state);
    sheet.getRange(b.row + GWB_UPDATED, col).setValue(at ? new Date(at) : '');
  } catch (err) {
    console.error('[GW-BLOCK] 設定欄の更新に失敗（処理は継続）: ' + err);
  }
}

// 「Gateway設定」シートの「設定を変更する」チェックで予約する（onDownlinkSheetEdit から呼ばれる）。
function onGatewaySheetEdit_(sheet, range) {
  const ops = findBlockByKeyword_(sheet, GWOPS_KEYWORD);
  if (ops && range.getColumn() === ops.col + 1 &&
      range.getRow() >= ops.row + GWOPS_FIRST && range.getRow() <= ops.row + GWOPS_CANCEL) {
    return onGatewayOpsEdit_(sheet, ops, range);
  }
  const bb = findBlockByKeyword_(sheet, GWBATCH_KEYWORD);
  if (bb && range.getColumn() === bb.col + 1 && range.getRow() === bb.row + GWBATCH_APPLY) {
    return onGatewayBatchEdit_(sheet, bb, range);
  }
  const b = findBlockByKeyword_(sheet, GW_BLOCK_KEYWORD);
  if (!b || range.getColumn() !== b.col + 1 || range.getRow() !== b.row + GWB_APPLY) return;
  const stateCell = sheet.getRange(b.row + GWB_STATE, b.col + 1);
  const inputCell = sheet.getRange(b.row + GWB_INTERVAL, b.col + 2);
  const v = inputCell.getValue();
  const n = Number(v);
  let err = '';
  if (v === '' || v === null) err = '変更後の送信間隔（分）を入力してください';
  else err = queueGatewayInterval_(n, 'シートから予約');
  if (err) { stateCell.setValue('入力エラー: ' + err); range.setValue(false); return; }
  inputCell.clearContent();
  range.setValue(false);
}

// エディタから Gateway の送信間隔を予約する（シートを使わない検証用）。
const EDITOR_GW_INTERVAL = 3;   // 分
function editorGwSetInterval() {
  const err = queueGatewayInterval_(EDITOR_GW_INTERVAL, 'エディタから予約');
  console.log(err ? ('✗ ' + err) : ('Gateway の送信間隔を ' + EDITOR_GW_INTERVAL + '分に予約しました'));
}


// ================================
// 一括変更（Gateway ＋ 群の全子機）  2026-09-23
// ================================
// 子機と Gateway を別々に変えると、間隔のルール（Gateway ≦ 子機の最短 × 5）を「今の値」で判定するため、
// 変える順番によっては入力エラーになる（例: 両方を長くするとき、子機を先に変えないと Gateway が弾かれる）。
// ★2026-09-24 FW21: 変更後と途中状態を判定し、必要なら先行側の完了後に2段目を予約する。
//   ・空欄の項目は変更しない（子機の平均・メジアンは各子機の現在値を維持）
//   ・対象の子機は「シート名編集」で有効な、この Gateway の群の子機すべて
//   ・適用されるのは、Gateway は次の予約確認のとき、子機はそれぞれの次の送信のとき
//
//        A                         B
//   12   一括変更（Gateway＋全子機）
//   13   子機の送信間隔（分）        [  ]
//   14   子機の平均回数              [  ]
//   15   子機のメジアン回数          [  ]
//   16   Gateway の送信間隔（分）    [  ]
//   17   まとめて変更する            [☐]
//   18   結果                        （自動）
const GWBATCH_KEYWORD = '一括変更（Gateway＋全子機）';
const GWBATCH_SLEEP = 1, GWBATCH_AVG = 2, GWBATCH_MEDIAN = 3, GWBATCH_GW = 4,
      GWBATCH_APPLY = 5, GWBATCH_RESULT = 6;

function setupGatewayBatchBlock_(sheet) {
  if (findBlockByKeyword_(sheet, GWBATCH_KEYWORD)) return;
  const main = findBlockByKeyword_(sheet, GW_BLOCK_KEYWORD);
  const r0 = (main ? main.row : 1) + GWB_UPDATED + 3;
  const c0 = main ? main.col : 1;
  const labels = [
    [GWBATCH_KEYWORD, ''],
    ['子機の送信間隔（分）', ''],
    ['子機の平均回数', ''],
    ['子機のメジアン回数', ''],
    ['Gateway の送信間隔（分）', ''],
    ['まとめて変更する', ''],
    ['結果', ''],
  ];
  sheet.getRange(r0, c0, labels.length, 2).setValues(labels).setBorder(true, true, true, true, true, true);
  sheet.getRange(r0, c0, 1, 3).merge().setFontWeight('bold').setBackground('#fce5cd');
  sheet.getRange(r0 + GWBATCH_SLEEP, c0 + 1, 4, 1).setBackground('#fff2cc');   // 入力欄
  sheet.getRange(r0 + GWBATCH_APPLY, c0 + 1).insertCheckboxes();
  sheet.getRange(r0 + GWBATCH_RESULT, c0 + 1, 1, 2).merge().setWrap(true);
  sheet.getRange(r0 + GWBATCH_SLEEP, c0 + 2).setValue('空欄の項目は変更しない');
  console.log('一括変更の欄を作りました（' + sheet.getRange(r0, c0).getA1Notation() + '〜）');
}

// この Gateway の群で「シート名編集」に有効登録されている子機（16進2桁）の一覧
function groupChildHexes_() {
  const map = loadDeviceSheetMapFromSheet();
  const out = [];
  Object.keys(map).forEach(function (k) {
    const id = Number(k);
    if ((id >> 5) !== GATEWAY_GROUP) return;
    const hex = ('0' + id.toString(16).toUpperCase()).slice(-2);
    if (CMD_STATUS_CHILD_IDS.indexOf(hex) >= 0) out.push(hex);
  });
  return out.sort();
}

// 一括変更の本体。エラーなら文言を返し、何も予約しない（一部だけ予約された状態を作らない）。
function batchChangeGatewayAndChildren_(sleepIn, avgIn, medianIn, gwIn, note) {
  // ★2026-09-24 FW24: 子機側に予約を入れる前に Gateway の別コマンドを検出する。
  const conflict = gatewayCmdConflict_('interval:' + gwIn);
  if (conflict && getGatewayPendingInterval_() === null) throw new Error(conflict);
  function num(v, min, max, label) {
    if (v === '' || v === null || v === undefined) return null;
    const n = Number(v);
    if (!Number.isInteger(n) || n < min || n > max) throw new Error(label + 'は' + min + '〜' + max + 'の整数で入力してください');
    return n;
  }
  const sleep  = num(sleepIn, 1, 1440, '子機の送信間隔');
  const avg    = num(avgIn, 1, 255, '子機の平均回数');
  const median = num(medianIn, 1, DL_MEDIAN_MAX, '子機のメジアン回数');
  const gw     = num(gwIn, 1, 1440, 'Gateway の送信間隔');
  const childChange = (sleep !== null || avg !== null || median !== null);
  if (!childChange && gw === null) throw new Error('変更する値を入力してください');

  // ★2026-09-24 FW21: 現在値だけでなく未完了の予約も、変更前の実効値に含める。
  const gwCurrent = getGatewayAppliedInterval_(), gwPending = getGatewayPendingInterval_();
  const gwBefore = Math.max(gwCurrent || 0, gwPending || 0) || null;
  const gwAfter = gw !== null ? gw : (gwPending || gwCurrent);
  const plans = [], childBefore = [], childAfter = [], unknown = [];
  groupChildHexes_().forEach(function (hex) {
    const cur = getKnownChildSettings_(hex);
    const d = dlGet_(hex);
    const pending = d && (d.state === 'queued' || d.state === 'sent') && d.mode !== 'status' ? d : null;
    const vals = childIntervalValues_(hex);
    vals.forEach(function (v) { childBefore.push(v); });
    if (childChange) {
      if (!cur && (sleep === null || avg === null || median === null)) { unknown.push('0x' + hex); return; }
      const p = { hex: hex,
                  sleep:  sleep  !== null ? sleep  : cur.sleep,
                  avg:    avg    !== null ? avg    : cur.avg,
                  median: median !== null ? median : cur.median };
      // 現在と同じ入力でも異なる予約があれば置き換える（後から別の値が適用されないように）。
      if (!cur || p.sleep !== cur.sleep || p.avg !== cur.avg || p.median !== cur.median ||
          (pending && (p.sleep !== pending.sleep || p.avg !== pending.avg || p.median !== pending.median))) plans.push(p);
      childAfter.push(p.sleep);
    } else if (vals.length) {
      childAfter.push(Math.min.apply(null, vals));
    }
  });
  if (unknown.length) {
    throw new Error('現在値が不明な子機があります（' + unknown.join(', ') + '）。' +
                    '子機の3項目をすべて入力するか、先にその子機でステータス確認をしてください');
  }

  const finalError = intervalRuleError_(gwAfter, childAfter);
  if (finalError) throw new Error('変更後: ' + finalError);
  const gwFirstError = intervalRuleError_(gwAfter, childBefore);
  const childrenFirstError = intervalRuleError_(gwBefore, childAfter);
  if (gwFirstError && childrenFirstError) throw new Error('途中状態で容量を超えます。2回に分けて変更してください');

  const props = PropertiesService.getScriptProperties();
  cancelDeferredBatch_('新しい一括変更');
  if (gw !== null && gw === gwCurrent && gwPending !== null && gwPending !== gw) {
    props.deleteProperty('pending_cmd_' + GATEWAY_DEVICE_ID);
    gwLog_('Gateway間隔 取消', '予約中の' + gwPending + '分を取消（現在値' + gwCurrent + '分を維持）', note);
  }
  let second = '';
  if (childrenFirstError) {
    // Gateway の短縮を確認するまで、子機の短縮は予約しない。
    props.setProperty('gw_batch_deferred', JSON.stringify({ type: 'children_after_gw', gw: gwAfter,
      plans: plans, note: note, at: new Date().toISOString() }));
    if (gwAfter !== gwCurrent) setGatewayPending_(gwAfter, note);
    second = '（2段目: Gateway ' + gwAfter + '分の確認後に子機 ' + plans.length + '台を予約します）';
  } else {
    // 両方を今すぐ予約できる場合も子機が先。1台でも失敗したら今回分を取り消す。
    const queued = queueBatchChildren_(plans, note);
    if (gwFirstError) {
      // ★FW21: 既存のGateway予約も保留へ移す。残すと子機の完了前に適用されてしまう。
      if (gwPending !== null) {
        props.deleteProperty('pending_cmd_' + GATEWAY_DEVICE_ID);
        gwLog_('Gateway間隔 保留', '既存の' + gwPending + '分予約を取消し、子機の完了を待ちます', note);
      }
      props.setProperty('gw_batch_deferred', JSON.stringify({ type: 'gw_after_children', gw: gwAfter,
        children: queued, note: note, at: new Date().toISOString() }));
      second = '（2段目: 子機の完了後に Gateway ' + gwAfter + '分を予約します）';
    } else if (gw !== null && gw !== gwCurrent) {
      setGatewayPending_(gw, note);
    }
  }
  syncGatewayBlock_();
  refreshCmdStatusSheet();
  const parts = [];
  if (gw !== null) parts.push('Gateway ' + gw + '分');
  if (childChange) parts.push('子機 ' + plans.length + '台（' + (plans.map(function (p) { return '0x' + p.hex; }).join(', ') || '変更なし') + '）');
  gwLog_('一括変更 予約', parts.join(' / ') + second,
         note + '　子機: 間隔=' + (sleep !== null ? sleep : '維持') + ', 平均=' + (avg !== null ? avg : '維持') +
         ', メジアン=' + (median !== null ? median : '維持'));
  return '予約しました: ' + parts.join(' / ') + second +
         '（' + Utilities.formatDate(new Date(), 'Asia/Tokyo', 'MM/dd HH:mm') + '）';
}

// ★2026-09-24 FW21: 子機の予約失敗を一括変更へ伝え、今回入れた分だけ取り消す。
function queueBatchChildren_(plans, note) {
  const queued = [];
  for (let i = 0; i < plans.length; i++) {
    const p = plans[i];
    if (!queueDownlink_(p.hex, p.sleep, p.avg, p.median, note)) {
      queued.forEach(function (q) { cancelDownlink_(q.hex, '一括変更の途中失敗で取消'); });
      throw new Error('子機0x' + p.hex + 'の予約に失敗しました。今回の子機予約を取り消しました');
    }
    queued.push({ hex: p.hex, seq: dlGet_(p.hex).seq });
  }
  return queued;
}

function cancelDeferredBatch_(reason) {
  const props = PropertiesService.getScriptProperties();
  if (!props.getProperty('gw_batch_deferred')) return;
  props.deleteProperty('gw_batch_deferred');
  gwLog_('一括変更の2段目を中止（' + reason + '）', '', '');
  syncGatewayBlock_();
}

// ★FW21: 報告のロックを解放してから呼ぶ。2段目の失敗で Gateway への応答を止めない。
function processDeferredBatch_() {
  try {
    const props = PropertiesService.getScriptProperties();
    const raw = props.getProperty('gw_batch_deferred');
    if (!raw) return;
    const d = JSON.parse(raw);
    if (d.type === 'gw_after_children') {
      let waiting = false;
      for (let i = 0; i < d.children.length; i++) {
        const q = d.children[i], rec = dlGet_(q.hex);
        if (!rec || rec.seq !== q.seq) { cancelDeferredBatch_('子機0x' + q.hex + 'の予約が入れ替わりました'); return; }
        if (rec.state === 'failed') { cancelDeferredBatch_('子機0x' + q.hex + 'が失敗しました'); return; }
        if (rec.state !== 'done') waiting = true;
      }
      if (waiting) return;
      setGatewayPending_(d.gw, '一括変更の2段目');
      props.deleteProperty('gw_batch_deferred');
      syncGatewayBlock_();
    } else if (d.type === 'children_after_gw') {
      const pending = props.getProperty('pending_cmd_' + GATEWAY_DEVICE_ID);
      if (pending && pending !== 'interval:' + d.gw) { cancelDeferredBatch_('Gateway の予約が入れ替わりました'); return; }
      if (getGatewayAppliedInterval_() !== d.gw) return;
      try {
        queueBatchChildren_(d.plans, '一括変更の2段目');
      } catch (err) {
        cancelDeferredBatch_('子機の予約失敗: ' + err.message);
        return;
      }
      props.deleteProperty('gw_batch_deferred');
      gwLog_('一括変更の2段目 予約', '子機 ' + d.plans.length + '台', d.note);
      syncGatewayBlock_();
    }
  } catch (err) {
    console.error('一括変更の2段目の処理に失敗: ' + err);
  }
}

function onGatewayBatchEdit_(sheet, bb, range) {
  const col = bb.col + 1;
  const v = sheet.getRange(bb.row + GWBATCH_SLEEP, col, 4, 1).getValues();
  const resultCell = sheet.getRange(bb.row + GWBATCH_RESULT, col);
  let msg;
  try {
    msg = batchChangeGatewayAndChildren_(v[0][0], v[1][0], v[2][0], v[3][0], 'シートから一括予約');
    sheet.getRange(bb.row + GWBATCH_SLEEP, col, 4, 1).clearContent();   // 受け付けたので入力欄を空に戻す
  } catch (err) {
    msg = '入力エラー: ' + err.message;
  }
  resultCell.setValue(msg);
  range.setValue(false);
}


// ================================
// Gateway 起動ログ（「Gateway起動ログ」シート）  2026-09-24（Gateway FW23〜）
// ================================
// Gateway が起動するたびに1行追記する（本番 v1.21 の info 行と同じ考え方）。
// 「いつ・どの機体が・どの FW で・どの SIM で起動したか」「なぜ再起動したか（前回のリセット）」を
// 現場に行かずに追えるようにする。備考列は人が書く欄（GAS は書かない）。
const GW_BOOT_SHEET_NAME = 'Gateway起動ログ';
const GW_BOOT_HEADER = [
  '受信日時', 'Gateway名', '群番号', 'FWバージョン',
  'XIAO-ID\nマイコン(XIAO nRF52840)固有の識別番号',
  'SIM IMEI\n通信モジュール(SIM7080G)固有の識別番号',
  'SIM ICCID\nSIMカード自体の固有番号',
  'SIM名',
  'CSQ\n電波強度の指標。0〜31で大きいほど強い（目安: 10以下=弱い、15〜20=普通、20以上=良好、99=圏外）',
  '送信間隔(分)', 'アプリWDT(分)', '顧客送信先',
  '前回のリセット\npower_on=電源投入・電圧低下 / soft_reset=ソフトリセット(書き込み直後・アプリWDT・LTE再接続) / hard_wdt=処理の固着 / reset_pin=リセットボタン',
  'runId\nInfluxDB の run タグ', 'Gateway時刻(起動時)', '備考',
];
// 数字だけの列は、スプレッドシートが数値に変えて桁が落ちる（ICCID は20桁）ので文字列として書く
const GW_BOOT_TEXT_COLS = [5, 6, 7, 14];   // 1起点: XIAO-ID, IMEI, ICCID, runId

// ★2026-09-24 FW24: 手動のステータス確認・診断ログ吸い上げの記録先。
const GW_STATUS_SHEET_NAME = 'Gatewayステータス';
const GW_EVENT_LOG_SHEET_NAME = 'Gateway診断ログ';
const GW_STATUS_HEADER = [
  '受信日時', 'Gateway名', 'FW', '群', 'Gateway時刻', '送信間隔(分)', '送信の状態', 'CSQ',
  '稼働時間(分)', '空きヒープ(B)', 'バッファ件数', '再送待ち(GAS)', '再送待ち(顧客)',
  'LoRa受信OK', 'LoRa受信NG', '群外破棄', 'あふれ破棄', 'GAS OK', 'GAS NG',
  '顧客 OK', '顧客 NG', '予約確認 OK', '予約確認 NG', 'runId',
];

function getGatewayStatusSheet_() {
  const ss = getSpreadsheet();
  let sheet = ss.getSheetByName(GW_STATUS_SHEET_NAME);
  if (sheet) return sheet;
  sheet = ss.insertSheet(GW_STATUS_SHEET_NAME);
  sheet.getRange(1, 1, 1, GW_STATUS_HEADER.length).setValues([GW_STATUS_HEADER])
       .setFontWeight('bold').setBackground('#9fc5e8').setWrap(true);
  sheet.setFrozenRows(1);
  sheet.getRange(2, 24, sheet.getMaxRows() - 1, 1).setNumberFormat('@');
  return sheet;
}

function appendGatewayStatus_(p) {
  const lock = LockService.getScriptLock();
  lock.waitLock(10000);
  try {
    const sheet = getGatewayStatusSheet_();
    const id = p.device_id || 'GW';
    const paused = String(p.paused);
    const state = paused === '1' ? '★送信停止中' : (paused === '0' ? '送信中' : '不明（ステータス確認で更新）');
    const row = [new Date(), id, p.fw || '', p.group || '', String(p.rtc || '').replace('_', ' '),
      p.interval || '', state, p.csq || '', p.uptime_min || '', p.free_heap || '',
      p.buf || '', p.retry_gas || '', p.retry_cust || '', p.lora_ok || '', p.lora_ng || '',
      p.rejected || '', p.dropped || '', p.gas_ok || '', p.gas_ng || '', p.cust_ok || '', p.cust_ng || '',
      p.dl_ok || '', p.dl_ng || '', String(p.run || '')];
    const r = sheet.getLastRow() + 1;
    sheet.getRange(r, 24).setNumberFormat('@');
    sheet.getRange(r, 1, 1, row.length).setValues([row]);
    sheet.getRange(r, 1).setNumberFormat('yyyy/MM/dd HH:mm:ss');   // ★2026-09-24 日付だけの表示になっていたため時刻まで出す
    if (paused === '0' || paused === '1') PropertiesService.getScriptProperties().setProperty('gw_paused_' + id, paused);
  } finally {
    lock.releaseLock();
  }
  syncGatewayBlock_();
  refreshCmdStatusSheet();
}

function getGatewayEventLogSheet_() {
  const ss = getSpreadsheet();
  let sheet = ss.getSheetByName(GW_EVENT_LOG_SHEET_NAME);
  if (sheet) return sheet;
  sheet = ss.insertSheet(GW_EVENT_LOG_SHEET_NAME);
  sheet.getRange(1, 1, 1, 4).setValues([['受信日時', 'Gateway名', 'runId', '内容']])
       .setFontWeight('bold').setBackground('#9fc5e8').setWrap(true);
  sheet.setFrozenRows(1);
  sheet.getRange(2, 3, sheet.getMaxRows() - 1, 1).setNumberFormat('@');
  return sheet;
}

function getGatewayBootLogSheet_() {
  const ss = getSpreadsheet();
  let sheet = ss.getSheetByName(GW_BOOT_SHEET_NAME);
  if (sheet) return sheet;
  sheet = ss.insertSheet(GW_BOOT_SHEET_NAME);
  const n = GW_BOOT_HEADER.length;
  sheet.getRange(1, 1, 1, n).setValues([GW_BOOT_HEADER])
       .setFontWeight('bold').setBackground('#9fc5e8').setWrap(true)
       .setHorizontalAlignment('center').setVerticalAlignment('middle');
  sheet.setFrozenRows(1);
  sheet.setRowHeight(1, 90);
  const widths = [140, 90, 60, 80, 170, 170, 190, 70, 150, 80, 80, 80, 200, 110, 140, 200];
  widths.forEach(function (w, i) { sheet.setColumnWidth(i + 1, w); });
  GW_BOOT_TEXT_COLS.forEach(function (c) { sheet.getRange(2, c, sheet.getMaxRows() - 1, 1).setNumberFormat('@'); });
  return sheet;
}

function appendGatewayBootLog_(p) {
  const sheet = getGatewayBootLogSheet_();
  const row = [
    new Date(), p.gw || '', p.group || '', p.fw || '',
    String(p.xiao || ''), String(p.imei || ''), String(p.iccid || ''), p.sim || '',
    p.csq || '', p.interval || '', p.appwdt || '', p.sink || '',
    p.reset || '', String(p.run || ''), String(p.rtc || '').replace('_', ' '), '',
  ];
  const lock = LockService.getScriptLock();
  lock.waitLock(10000);
  try {
    const r = sheet.getLastRow() + 1;
    GW_BOOT_TEXT_COLS.forEach(function (c) { sheet.getRange(r, c).setNumberFormat('@'); });
    sheet.getRange(r, 1, 1, row.length).setValues([row]);
    sheet.getRange(r, 1).setNumberFormat('yyyy/MM/dd HH:mm:ss');   // ★2026-09-24 日付だけの表示になっていたため時刻まで出す
    if (p.reset === 'hard_wdt' || p.reset === 'lockup') {
      sheet.getRange(r, 1, 1, row.length).setBackground('#f4cccc');   // 異常な再起動は赤で目立たせる
    }
  } finally {
    lock.releaseLock();
  }
}


// ================================
// 手動テスト用
// ================================
// エディタから実行して、Gateway を動かさずに doPost の書き込みだけを確認する。
function testDoPost() {
  const fake = {
    postData: {
      contents: JSON.stringify({
        gw: 'ipec_caopen_test',
        csq: 21,
        d: [
          { id: 14, t: '2026-09-22 12:02:00', ch: [-2345, -1665, -7206, 0], batt: 3300, rssi: -25, fw: 10 },
          { id: 15, t: '2026-09-22 12:02:00', ch: [100, 200, 300, 288], batt: 3280, rssi: -31, fw: 2 },
        ],
      }),
    },
  };
  const res = doPost(fake);
  console.log('doPost 応答: ' + res.getContent());
  console.log('スプレッドシートに2行入っていれば成功');
}

// ================================
// ★2026-09-24 設定欄の共通デザイン（予約・報告・判定とは分けて、セットアップ時だけ適用）
// ================================
const UI_ = {
  navy: '#1f4e79', sub: '#d9e2f3', label: '#f2f2f2', labelText: '#404040',
  input: '#fff2cc', inputBorder: '#bf9000', check: '#ddebf7',
  auto: '#efefef', autoText: '#595959', note: '#7f6000', line: '#bfbfbf',
  error: '#fce4e4', errorText: '#c00000', ok: '#e6f4ea', okText: '#38761d',
  waiting: '#fff4e5', waitingText: '#b45f06', font: 'Noto Sans JP',
  autoDescription: '自動で書き込まれる欄です',
};
const OPERATION_GUIDE_SHEET_NAME = '操作ガイド';

function UI_downlinkSheets_() {
  const ss = getSpreadsheet(), map = loadDeviceSheetMapFromSheet();
  const names = [TEMPLATE_SHEET_NAME].concat(Object.keys(map).map(function (k) { return map[k]; }));
  return Array.from(new Set(names)).map(function (name) { return ss.getSheetByName(name); }).filter(Boolean);
}

// ★2026-09-24 dateKey と既定本文だけの行は未設定。閾値 0 や本文だけの編集も残す。
function isAlertSettingConfigured_(row) {
  return row.some(function (value, col) {
    if (col === ALERT_COL_DATEKEY) return false;
    const text = String(value).trim();
    return text !== '' && (col !== ALERT_COL_BODY || String(value) !== ALERT_DEFAULT_BODY);
  });
}

function compactAlertSettings_(sheet) {
  const layout = findAlertLayout_(sheet);
  if (!layout) return;
  const start = layout.settingHeaderRow + 1;
  const keys = sheet.getRange(start, layout.keyCol, layout.mapRow - start, 1).getValues();
  let count = 0;
  while (count < keys.length && /^d\d+/.test(String(keys[count][0]).trim())) count++;
  // 圧縮後は空の dateKey が混ざるため、再実行でも先頭10行の入力を取りこぼさない。
  const rows = Math.max(ALERT_SETTING_ROWS, count);
  if (rows > keys.length) return;
  const width = ALERT_COL_MAIL_FROM + ALERT_MAIL_COUNT;
  const settings = sheet.getRange(start, layout.keyCol, rows, width).getValues();
  const configured = settings.filter(isAlertSettingConfigured_);
  if (configured.length > ALERT_SETTING_ROWS) {
    console.log('[ALERT-UI] 設定済みが10行を超えるため詰めません: ' + sheet.getName());
    return false;
  }
  while (configured.length < ALERT_SETTING_ROWS) {
    const blank = Array(width).fill('');
    blank[ALERT_COL_BODY] = ALERT_DEFAULT_BODY;
    configured.push(blank);
  }
  sheet.getRange(start, layout.keyCol, ALERT_SETTING_ROWS, width).setValues(configured);
  // 3〜12行目はダウンリンク欄と共用。削除するのは11番目以降の設定行だけ。
  const deleteStart = Math.max(13, start + ALERT_SETTING_ROWS);
  const deleteCount = start + count - deleteStart;
  if (deleteCount > 0) sheet.deleteRows(deleteStart, deleteCount);
}

function setupAlertSettingsUi_(sheet) {
  const layout = findAlertLayout_(sheet);
  if (!layout) return;
  const start = layout.settingHeaderRow + 1, col = layout.keyCol;
  const width = ALERT_COL_MAIL_FROM + ALERT_MAIL_COUNT;
  const nCol = sheet.getLastColumn();
  const map = sheet.getRange(layout.mapRow, 1, 1, nCol).getValues()[0];
  const headers = sheet.getRange(layout.dataHeaderRow, 1, 1, nCol).getValues()[0];
  const labels = {}, choices = [];
  map.forEach(function (value, c) {
    const key = String(value).trim();
    if (!/^d\d+$/.test(key)) return;
    const heading = String(headers[c]).replace(/\s+/g, ' ').trim().slice(0, 20);
    const label = key + '｜' + (heading || '（補正用 ' + key + '）');
    labels[key] = label;
    choices.push(label);
  });
  const settings = sheet.getRange(start, col, ALERT_SETTING_ROWS, width).getValues();
  const keyRange = sheet.getRange(start, col, ALERT_SETTING_ROWS, 1);
  keyRange.setValues(settings.map(function (row) {
    const key = String(row[ALERT_COL_DATEKEY]).trim();
    return [isAlertSettingConfigured_(row) ? (labels[key] || row[ALERT_COL_DATEKEY]) : ''];
  }));
  keyRange.setDataValidation(SpreadsheetApp.newDataValidation().requireValueInList(choices, true)
    .setAllowInvalid(false)
    .setHelpText('監視する列を選んでください（d番号｜列名）。同じ列を複数の行で選べます（例: 上限と下限）').build());
  sheet.getRange(start, col + ALERT_COL_CONDITION, ALERT_SETTING_ROWS, 1)
    .setDataValidation(SpreadsheetApp.newDataValidation().requireValueInList(['絶対値', '上回る', '下回る'], true)
      .setAllowInvalid(false).setHelpText('条件の空欄は絶対値として扱います。').build());
  sheet.getRange(start, col + ALERT_COL_THRESHOLD, ALERT_SETTING_ROWS, 1)
    .setDataValidation(SpreadsheetApp.newDataValidation().requireNumberBetween(-1e9, 1e9)
      .setAllowInvalid(false).setHelpText('閾値は数値で入力してください。').build());
  sheet.getRange(start, col + ALERT_COL_MAIL_FROM, ALERT_SETTING_ROWS, ALERT_MAIL_COUNT)
    .setDataValidation(SpreadsheetApp.newDataValidation().requireTextIsEmail().setAllowInvalid(false).build());
  sheet.getRange(layout.settingHeaderRow, col, ALERT_SETTING_ROWS + 1, width)
    .setFontFamily(UI_.font).setFontSize(10).setFontWeight('normal').setVerticalAlignment('middle');
  UI_subheading_(sheet.getRange(layout.settingHeaderRow, col, 1, width));
  const inputs = sheet.getRange(start, col, ALERT_SETTING_ROWS, width)
    .setBackground(UI_.input).setFontColor('#000000');
  UI_frame_(inputs, UI_.inputBorder);
  // ★2026-09-24 タイトルの横に選び方を示し、同じ列を複数行で監視できることを伝える。
  sheet.getRange(layout.settingHeaderRow - 1, col + 2, 1, width - 2).merge()
    .setValue('dateKey はプルダウンで選ぶ（d番号｜列名）。同じ列を2行に分けて上限・下限を監視できる。条件の空欄は絶対値')
    .setFontFamily(UI_.font).setFontSize(9).setFontColor(UI_.autoText).setFontWeight('normal').setWrap(true);
}

// 【エディタから実行】★2026-09-24 雛形と有効な子機の閾値欄・注意書きを移行する。
function setupAlertSettings() {
  UI_downlinkSheets_().forEach(function (sheet) {
    // 10行を超える既存設定は dateKey も含めてそのまま残す。
    if (compactAlertSettings_(sheet) !== false) {
      setupAlertSettingsUi_(sheet);
      UI_setupAlertGuide_(sheet);
    }
    UI_moveDownlinkNotes_(sheet);
  });
}

function UI_frame_(range, color) {
  range.setBorder(true, true, true, true, true, true, UI_.line, SpreadsheetApp.BorderStyle.SOLID);
  range.setBorder(true, true, true, true, null, null, color || UI_.navy, SpreadsheetApp.BorderStyle.SOLID_MEDIUM);
}

function UI_block_(sheet, b, last, headingColor) {
  const range = sheet.getRange(b.row, b.col, last + 1, 3);
  range.setFontFamily(UI_.font).setFontSize(10).setFontWeight('normal')
       .setFontColor('#000000').setBackground('#ffffff').setVerticalAlignment('middle');
  sheet.getRange(b.row + 1, b.col, last, 1).setBackground(UI_.label).setFontColor(UI_.labelText).setWrap(true);
  sheet.getRange(b.row, b.col, 1, 3).merge().setBackground(headingColor || UI_.navy)
       .setFontColor('#ffffff').setFontWeight('bold').setFontSize(11).setHorizontalAlignment('center');
  UI_frame_(range);
}

function UI_subheading_(range) {
  range.setBackground(UI_.sub).setFontColor(UI_.navy).setFontWeight('bold').setWrap(true);
}

function UI_warningProtection_(range, description) {
  const sheet = range.getSheet();
  description = description || UI_.autoDescription;
  // ★2026-09-24 同じ警告を増やさず、管理者が設定した別の保護は変更しない。
  const existing = sheet.getProtections(SpreadsheetApp.ProtectionType.RANGE).filter(function (p) {
    return p.getDescription() === description && p.isWarningOnly() &&
           p.getRange().getA1Notation() === range.getA1Notation();
  });
  if (!existing.length) range.protect().setDescription(description).setWarningOnly(true);
}

// ★2026-09-24 案内は設定行と対応行の間だけを探す。データ中の同じ文言は対象外。
const UI_ALERT_GUIDE = [
  ALERT_GUIDE_KEYWORD,
  '1. dateKey（A列）: 監視する列をプルダウンで選ぶ（d番号｜列名）。同じ列を2行に分けて、上限と下限を別々に監視できる',
  '2. 条件（B列）: 絶対値＝プラス・マイナスどちらの向きでも閾値を超えたら／上回る＝閾値より大きくなったら／下回る＝閾値より小さくなったら（電池電圧の低下など）。空欄は「絶対値」として扱う',
  '3. 閾値（C列）: 数値（小数も可。例: 150.5）。「上回る」「下回る」ではマイナスの値も使える。「絶対値」ではプラスの値を入れる',
  '4. メールタイトル（D列）・メール本文（E列）: 本文の {{value}} は、送るときに実際の測定値に置き換わる',
  '5. mail-1〜mail-10（F〜O列）: 送り先。1行につき10件まで',
  '6. 通知のタイミング: Gateway からデータが届いたときに判定する（シートに手で入力しても通知されない）。同じ子機・同じ列・同じ条件の通知は、60分に1回まで',
  '7. 通知されないとき: 閾値・タイトル・本文・送り先のどれかが空の行は判定しない。センサーが欠測（空欄）の回も判定しない',
];

function UI_findAlertGuideRow_(sheet, layout) {
  if (!layout) return 0;
  const start = layout.settingHeaderRow + 1 + ALERT_SETTING_ROWS;
  if (layout.mapRow <= start) return 0;
  const rows = sheet.getRange(start, 1, layout.mapRow - start, 1).getValues();
  const index = rows.findIndex(function (row) { return String(row[0]).trim() === ALERT_GUIDE_KEYWORD; });
  return index < 0 ? 0 : start + index;
}

function UI_protectAlertGuide_(sheet) {
  const row = UI_findAlertGuideRow_(sheet, findAlertLayout_(sheet));
  if (row) UI_warningProtection_(sheet.getRange(row, 1, UI_ALERT_GUIDE.length, 15), 'アラートの使い方の案内です');
}

function UI_setupAlertGuide_(sheet) {
  const layout = findAlertLayout_(sheet);
  if (!layout) return;
  const start = layout.settingHeaderRow + 1 + ALERT_SETTING_ROWS;
  if (layout.mapRow < start) return;
  let row = UI_findAlertGuideRow_(sheet, layout);
  if (!row) {
    sheet.insertRowsAfter(start - 1, UI_ALERT_GUIDE.length);
    row = start;
  }
  // ★2026-09-24 行を挿入すると直前の行（ダウンリンク欄の「予約を取り消す」行）の書式が引き継がれ、
  //   Q〜S 列に灰色とチェックボックスが並んだ。案内の行の P 列から右は使わないので、書式と入力規則を毎回消す
  //   （既存シートに残ったものも、setupAllSheets の再実行で消える）。
  const extraCols = sheet.getMaxColumns() - 15;
  if (extraCols > 0) {
    sheet.getRange(row, 16, UI_ALERT_GUIDE.length, extraCols).breakApart().clearContent().clearFormat().clearDataValidations();
  }
  const range = sheet.getRange(row, 1, UI_ALERT_GUIDE.length, 15);
  range.breakApart().clearContent().clearDataValidations()
    .setFontFamily(UI_.font).setFontSize(9).setFontWeight('normal').setFontColor(UI_.autoText)
    .setBackground('#ffffff').setWrap(true).setHorizontalAlignment('left').setVerticalAlignment('middle')
    .setBorder(true, true, true, true, false, false, UI_.line, SpreadsheetApp.BorderStyle.SOLID);
  let width = 0;
  for (let col = 1; col <= 15; col++) width += sheet.getColumnWidth(col);
  UI_ALERT_GUIDE.forEach(function (text, i) {
    const line = sheet.getRange(row + i, 1, 1, 15).merge().setValue(text);
    if (i === 0) UI_subheading_(line);
    // ★2026-09-24 横結合セルは自動調整に頼らず、列幅と9ptの文字幅から折り返しの高さを確保する。
    const pixels = Array.from(text).reduce(function (n, c) { return n + (/[ -~]/.test(c) ? 7 : 13); }, 0);
    sheet.setRowHeight(row + i, Math.max(24, Math.ceil(pixels / Math.max(1, width - 16)) * 16 + 8));
  });
  UI_protectAlertGuide_(sheet);
  UI_groupAlertGuide_(sheet, row);
}

// ★2026-09-24 案内の本文（見出しの下の7行）は常に見る必要がないので、行のグループにして折りたたむ。
//   見出しの行は表示したまま、その行の「＋」で開く（グループの開閉ボタンを見出し側＝上に出す）。
//   既にグループになっていれば、作り直さず開閉の状態もそのまま（利用者が開いていても閉じない）。
function UI_groupAlertGuide_(sheet, row) {
  const first = row + 1, count = UI_ALERT_GUIDE.length - 1;
  if (count <= 0) return;
  sheet.setRowGroupControlPosition(SpreadsheetApp.GroupControlTogglePosition.BEFORE);
  if (sheet.getRowGroupDepth(first) > 0) return;
  sheet.getRange(first, 1, count, 1).shiftRowGroupDepth(1);
  const group = sheet.getRowGroup(first, 1);
  if (group) group.collapse();
}

function UI_auto_(range) {
  range.setBackground(UI_.auto).setFontColor(UI_.autoText);
  UI_warningProtection_(range);
}

function UI_input_(range, max, label) {
  const cell = range.getA1Notation();
  // ★2026-09-24 範囲指定だけでは小数も通るため、整数条件も式で確認する。空欄＝変更しない。
  const formula = '=OR(' + cell + '="",AND(ISNUMBER(' + cell + '),' + cell + '>=1,' +
                  cell + '<=' + max + ',MOD(' + cell + ',1)=0))';
  range.setBackground(UI_.input).setFontColor('#000000').setHorizontalAlignment('center')
       .setBorder(true, true, true, true, null, null, UI_.inputBorder, SpreadsheetApp.BorderStyle.SOLID_MEDIUM)
       .setDataValidation(SpreadsheetApp.newDataValidation().requireFormulaSatisfied(formula)
         .setAllowInvalid(false).setHelpText(label + 'は1〜' + max + 'の整数で入力してください。空欄は変更しません。').build());
}

function UI_checkbox_(range) {
  // insertCheckboxes は値を false に戻すので、既存チェックの書式だけ変える。
  range.setBackground(UI_.check).setHorizontalAlignment('center');
}

function UI_replaceRules_(sheet, tag, rules) {
  const kept = sheet.getConditionalFormatRules().filter(function (rule) {
    const condition = rule.getBooleanCondition();
    return !condition || !condition.getCriteriaValues().some(function (v) { return String(v).indexOf(tag) >= 0; });
  });
  sheet.setConditionalFormatRules(rules.concat(kept));
}

function UI_statusRules_(sheet, ranges) {
  const rules = [];
  ranges.forEach(function (range) {
    const cell = range.getCell(1, 1).getA1Notation();
    [
      ['⚠', null, UI_.errorText, true],
      ['失敗|エラー|入力エラー', UI_.error, UI_.errorText, false],
      ['完了|予約しました|取り消しました', UI_.ok, UI_.okText, false],
      ['予約中|送信済み|2段目待ち', UI_.waiting, UI_.waitingText, false],
    ].forEach(function (entry) {
      const builder = SpreadsheetApp.newConditionalFormatRule().whenFormulaSatisfied(
        '=AND(N("UI_STATUS")=0,REGEXMATCH(TO_TEXT(' + cell + '),"' + entry[0] + '"))')
        .setRanges([range]).setFontColor(entry[2]);
      if (entry[1]) builder.setBackground(entry[1]);
      if (entry[3]) builder.setBold(true);
      rules.push(builder.build());
    });
  });
  UI_replaceRules_(sheet, 'UI_STATUS', rules);
}

function UI_downlinkAutoRanges_(sheet, b) {
  return [sheet.getRange(b.row + DLB_ID, b.col + 1),
          sheet.getRange(b.row + DLB_SLEEP, b.col + 1, 4, 1),
          sheet.getRange(b.row + DLB_STATE, b.col + 1, 1, 2),
          sheet.getRange(b.row + DLB_UPDATED, b.col + 1, 1, 2)];
}

function UI_protectDownlink_(sheet) {
  const b = findDownlinkBlock_(sheet);
  if (b) UI_downlinkAutoRanges_(sheet, b).forEach(function (range) { UI_warningProtection_(range); });
}

function UI_legend_(sheet, row, col, width) {
  [['■ 入力できる欄', UI_.input], ['■ チェックで実行', UI_.check],
   ['■ 自動で表示（編集しない）', UI_.auto]].forEach(function (item, i) {
    sheet.getRange(row + i, col, 1, width).merge().setValue(item[0]).setBackground(item[1])
         .setFontFamily(UI_.font).setFontSize(10).setFontColor(UI_.labelText).setFontWeight('normal').setWrap(true);
  });
}

function UI_notes_(sheet, row, col, width, notes, rowBudget) {
  let offset = 0;
  notes.forEach(function (text, i) {
    const important = /必ず|消える/.test(text);
    const desired = text.split('\n').reduce(function (n, line) { return n + Math.max(1, Math.ceil(line.length / 37)); }, 0);
    // ★2026-09-24 縦方向も結合して折り返しの高さを確保する。A〜O の行高は変えない。
    const rows = rowBudget === undefined ? desired : Math.min(desired, rowBudget - offset - (notes.length - i - 1));
    sheet.getRange(row + offset, col, rows, width).merge().setValue(text).setBackground('#ffffff')
         .setFontFamily(UI_.font).setFontSize(10).setWrap(true).setVerticalAlignment('top')
         .setFontColor(important ? UI_.errorText : UI_.note).setFontWeight(important ? 'bold' : 'normal');
    offset += rows;
  });
}

// ★2026-09-24 注意書きは U列に統一。旧配置の全文一致だけを消し、データ表は残す。
const UI_DOWNLINK_NOTES = [
  '1. 初めての子機は、まず「ステータス確認」',
  '2. 予約は子機1台につき1つ。新しい予約は前の予約を上書き',
  '3. 反映まで 最大「Gateway の送信間隔×2 ＋ 子機の送信間隔」。途中経過は「設定変更履歴」シート',
  '4. 送信間隔を変えると子機は自動で再起動（正常）',
  '5. 詳しくは「操作ガイド」シート',
];

function UI_moveDownlinkNotes_(sheet) {
  const b = findDownlinkBlock_(sheet);
  if (!b) return;
  const oldTexts = ['■ 入力できる欄', '■ チェックで実行', '■ 自動で表示（編集しない）']
    .concat(UI_DOWNLINK_NOTES);
  const last = sheet.getLastRow();
  if (last >= 13) {
    sheet.getRange(13, 17, last - 12, 3).getValues().forEach(function (row, r) {
      row.forEach(function (value, c) {
        const text = String(value).trim();
        if (!text || !text.split('\n').every(function (line) { return oldTexts.indexOf(line) >= 0; })) return;
        const cell = sheet.getRange(r + 13, c + 17);
        const merged = cell.getMergedRanges();
        const range = merged.length ? merged[0] : cell;
        // Q:S の旧注意書き以外にまたがる結合は触らない。
        if (range.getRow() < 13 || range.getColumn() < 17 ||
            range.getColumn() + range.getNumColumns() > 20) return;
        range.breakApart().clearContent().clearFormat();
      });
    });
  }
  const start = b.row + 2, col = 21;
  sheet.getRange(start, col, 9, 3).breakApart().clearContent().clearFormat();
  UI_legend_(sheet, start, col, 3);
  // 注意3だけ2行に分け、U:W は各行で横結合する。行高は入力欄でも使える30pxに抑える。
  const notes = UI_DOWNLINK_NOTES.slice(0, 2)
    .concat(UI_DOWNLINK_NOTES[2].replace('。途中経過', '。\n途中経過').split('\n'))
    .concat(UI_DOWNLINK_NOTES.slice(3));
  UI_notes_(sheet, start + 3, col, 3, notes, notes.length);
  sheet.setRowHeights(start, 9, 30);
  [col, col + 1, col + 2].forEach(function (c) { sheet.setColumnWidth(c, 120); });
}

function UI_designDownlink_(sheet) {
  const b = findDownlinkBlock_(sheet);
  if (!b) return;
  // ★2026-09-24 既存シートの旧項目名と完了表示だけを移行する。
  const label = sheet.getRange(b.row + DLB_WDT, b.col);
  if (label.getValue() === 'WDT（分）') label.setValue('異常時の自動再起動（分）');
  const status = sheet.getRange(b.row + DLB_STATE, b.col + 1);
  const oldStatus = String(status.getValue());
  if (/^完了.*　WDT=\d+分$/.test(oldStatus)) status.setValue(oldStatus.replace(/　WDT=\d+分$/, ''));
  UI_block_(sheet, b, DLB_CANCEL);
  UI_subheading_(sheet.getRange(b.row + DLB_HEAD, b.col, 1, 3));
  UI_downlinkAutoRanges_(sheet, b).forEach(UI_auto_);
  [[DLB_SLEEP, 1440, '送信間隔'], [DLB_AVG, 255, '平均回数'], [DLB_MEDIAN, DL_MEDIAN_MAX, 'メジアン回数']]
    .forEach(function (v) { UI_input_(sheet.getRange(b.row + v[0], b.col + 2), v[1], v[2]); });
  [DLB_APPLY, DLB_STATUS, DLB_CANCEL].forEach(function (r) { UI_checkbox_(sheet.getRange(b.row + r, b.col + 1)); });
  const state = sheet.getRange(b.row + DLB_STATE, b.col + 1, 1, 2).setWrap(true);
  sheet.getRange(b.row + DLB_UPDATED, b.col + 1, 1, 2).setNumberFormat('yyyy/MM/dd HH:mm:ss');
  UI_statusRules_(sheet, [state]);
  [150, 170, 210].forEach(function (w, i) { sheet.setColumnWidth(b.col + i, w); });
  UI_moveDownlinkNotes_(sheet);
}

function UI_designGateway_(sheet) {
  sheet.setHiddenGridlines(true);
  const states = [], b = findBlockByKeyword_(sheet, GW_BLOCK_KEYWORD);
  if (b) {
    // ★2026-09-24 保存値は変えず、既存ブロックの旧項目名だけを移行する。
    const label = sheet.getRange(b.row + GWB_WDT, b.col);
    if (label.getValue() === 'アプリWDT（分）') label.setValue('通信異常時の自動再起動（分）');
    UI_block_(sheet, b, GWB_UPDATED);
    UI_subheading_(sheet.getRange(b.row + GWB_HEAD, b.col, 1, 3));
    [GWB_ID, GWB_INTERVAL, GWB_CHILDMIN, GWB_WDT, GWB_STATE, GWB_UPDATED].forEach(function (r) {
      UI_auto_(sheet.getRange(b.row + r, b.col + 1, 1, [GWB_CHILDMIN, GWB_STATE, GWB_UPDATED].indexOf(r) >= 0 ? 2 : 1));
    });
    UI_input_(sheet.getRange(b.row + GWB_INTERVAL, b.col + 2), 1440, 'Gateway の送信間隔');
    UI_checkbox_(sheet.getRange(b.row + GWB_APPLY, b.col + 1));
    states.push(sheet.getRange(b.row + GWB_STATE, b.col + 1, 1, 2).setWrap(true));
    sheet.getRange(b.row + GWB_CHILDMIN, b.col + 1, 1, 2).setWrap(true);
    sheet.getRange(b.row + GWB_UPDATED, b.col + 1, 1, 2).setNumberFormat('yyyy/MM/dd HH:mm:ss');
  }
  const batch = findBlockByKeyword_(sheet, GWBATCH_KEYWORD);
  if (batch) {
    UI_block_(sheet, batch, GWBATCH_RESULT, '#7f6000');
    [[GWBATCH_SLEEP, 1440, '子機の送信間隔'], [GWBATCH_AVG, 255, '平均回数'],
     [GWBATCH_MEDIAN, DL_MEDIAN_MAX, 'メジアン回数'], [GWBATCH_GW, 1440, 'Gateway の送信間隔']]
      .forEach(function (v) { UI_input_(sheet.getRange(batch.row + v[0], batch.col + 1), v[1], v[2]); });
    UI_checkbox_(sheet.getRange(batch.row + GWBATCH_APPLY, batch.col + 1));
    const result = sheet.getRange(batch.row + GWBATCH_RESULT, batch.col + 1, 1, 2).setWrap(true);
    UI_auto_(result); states.push(result);
    sheet.getRange(batch.row + GWBATCH_SLEEP, batch.col + 2).setFontColor(UI_.note).setWrap(true);
  }
  const ops = findBlockByKeyword_(sheet, GWOPS_KEYWORD);
  if (ops) {
    UI_block_(sheet, ops, GWOPS_RESULT, '#274e13');
    [GWOPS_PENDING, GWOPS_PAUSED, GWOPS_RESULT].forEach(function (r) {
      const range = sheet.getRange(ops.row + r, ops.col + 1, 1, 2).setWrap(true);
      UI_auto_(range); states.push(range);
    });
    UI_checkbox_(sheet.getRange(ops.row + GWOPS_FIRST, ops.col + 1, GWOPS_CANCEL - GWOPS_FIRST + 1, 1));
  }
  UI_statusRules_(sheet, states);
  // ブロックが移動済みでも検出位置を尊重する。右側の説明は全ブロックより右に置く。
  [b, batch, ops].filter(Boolean).forEach(function (block) {
    [210, 190, 230].forEach(function (w, i) { sheet.setColumnWidth(block.col + i, w); });
  });
  const side = Math.max(5, ...[b, batch, ops].filter(Boolean).map(function (block) { return block.col + 4; }));
  sheet.setColumnWidth(side, 340); sheet.setColumnWidth(side + 1, 220);
  UI_legend_(sheet, 1, side, 2);
  UI_notes_(sheet, 5, side, 2, [
    'Gateway 宛ての予約は1つだけ。完了を待つか、取り消してから次の操作をしてください。',
    '送信間隔のルール: Gateway の送信間隔 ≦ いちばん短い子機の送信間隔 ×5',
    'データ送信を停止すると、再起動しても停止が続きます。必ず「データ送信を再開」してください。',
    'リモートリセットで送信待ちのデータが消えるため、実行前に確認してください。',
    '詳しくは「操作ガイド」シート',
  ]);
}

function UI_designDownlinkLog_(sheet) {
  sheet.getRange(1, 1, sheet.getMaxRows(), 7).setFontFamily(UI_.font).setFontSize(10);
  sheet.getRange(1, 1, 1, 7).setBackground(UI_.navy).setFontColor('#ffffff').setFontWeight('bold').setWrap(true);
  sheet.setFrozenRows(1);
  [150, 80, 50, 150, 320, 320, 80].forEach(function (w, i) { sheet.setColumnWidth(i + 1, w); });
  sheet.getRange('A2:A').setNumberFormat('yyyy/MM/dd HH:mm:ss');
  sheet.getRange('B2:B').setNumberFormat('0');
  sheet.getRange('E2:F').setWrap(true);
  const range = sheet.getRange('A2:G');
  const rules = [
    ['=AND(N("UI_LOG")=0,OR(REGEXMATCH(TO_TEXT($D2),"失敗|エラー|不一致"),REGEXMATCH(TO_TEXT($F2),"失敗|エラー|不一致")))', UI_.error],
    ['=AND(N("UI_LOG")=0,$D2="結果",REGEXMATCH(TO_TEXT($F2),"^完了"))', UI_.ok],
  ].map(function (entry) {
    return SpreadsheetApp.newConditionalFormatRule().whenFormulaSatisfied(entry[0]).setBackground(entry[1]).setRanges([range]).build();
  });
  UI_replaceRules_(sheet, 'UI_LOG', rules);
}

// 【エディタから1回実行】★2026-09-24 既存の値を保って、対象シートの書式だけを更新する。
function applySheetDesign() {
  UI_downlinkSheets_().forEach(UI_designDownlink_);
  const gateway = getSpreadsheet().getSheetByName(GW_SHEET_NAME);
  if (gateway) UI_designGateway_(gateway);
  UI_designDownlinkLog_(getDownlinkLogSheet_());
}

// 【エディタから1回実行】★2026-09-24 初期設定をまとめる。編集トリガーの作成は別途行う。
function setupAllSheets() {
  setupDownlinkBlocks();
  setupGatewayBlock();
  upgradeDownlinkBlocks();
  setupAlertSettings();
  applySheetDesign();
  setupOperationGuide();
}

// ★2026-09-24 操作ガイド原稿（2026-09-24版）§0〜§8の全文。実行時に外部ファイルは読まない。
// bold は Markdown の強調を除いた文字列内の開始・終了位置（RichText の書式用）。
const UI_GUIDE_DATA = [
  {"type": "h1","text": "0. 最初に知っておくこと（必読）"},
  {"type": "h2","text": "0.1 設定は「予約」→ 順番に届く","important": false},
  {"type": "p","text": {"text": "子機は電池を長持ちさせるため、ふだんは電波を受けていない。送信した直後の2秒間だけ受信する。そのため、シートでの操作は「その場で変わる」のではなく、次の順に進む。","bold": [[29,41]]},"important": false},
  {"type": "table","header": ["段階","いつ起きるか","状態欄の表示"],"rows": [["① 予約する","チェックを付けた瞬間","予約中（Gatewayの取得待ち）"],["② Gateway が予約を受け取る",{"text": "Gateway の次の送信サイクル（最大で Gateway の送信間隔ぶん）","bold": [[18,37]]},"予約中（Gatewayの取得待ち）"],["③ 子機に届く",{"text": "その子機が次に送信した直後（最大で子機の送信間隔ぶん）","bold": [[0,13]]},"送信済み（子機の確認待ち）"],["④ 結果がシートに出る","Gateway の次の送信サイクル（最大で Gateway の送信間隔ぶん）","完了／失敗"]],"important": false},
  {"type": "p","text": {"text": "反映までの目安＝ Gateway の送信間隔 × 2 ＋ 子機の送信間隔\n（例: Gateway 25分・子機 60分 → 最大で約1時間50分）","bold": [[0,7]]},"important": false},
  {"type": "h2","text": "0.2 「完了」になるまでは変わっていない","important": true},
  {"type": "p","text": "状態欄が「完了」になり、「子機の現在値」が新しい値になって、はじめて子機に反映されている。「予約中」「送信済み」の間は、子機はまだ前の設定で動いている。","important": true},
  {"type": "h2","text": "0.3 子機の「現在値」はシートの値ではなく子機の報告","important": false},
  {"type": "p","text": {"text": "「子機の現在値」は、子機が確認応答で送り返してきた実際の値。GAS が自動で書く（人が書き換えても意味がない）。","bold": [[25,29]]},"important": false},
  {"type": "h2","text": "0.4 予約の状況は「設定変更履歴」シートで確認できる","important": false},
  {"type": "p","text": {"text": "予約した内容と、その後の進み具合（予約 → 送信 → 結果、取消）は、「設定変更履歴」シートに1行ずつ時刻付きで残る。子機シートの状態欄は「今の状態」だけだが、設定変更履歴 を見れば「いつ予約して、いつ子機に届き、どうなったか」を追える。お客様にも見てもらってよいシート（§7）。","bold": [[35,58]]},"important": false},
  {"type": "h2","text": "0.5 Gateway が止まっていると何も進まない","important": true},
  {"type": "p","text": "予約は Gateway 経由で届く。Gateway が止まっている・圏外のときは②から先に進まない（予約は消えずに残る）。","important": true},
  {"type": "h1","text": "1. 子機の設定変更（各子機シート Q列「ダウンリンク設定」）"},
  {"type": "h2","text": "1.1 欄の見方","important": false},
  {"type": "table","header": ["項目","内容","入力"],"rows": [["DeviceID","この子機の ID（16進）","自動"],["子機の現在値","子機が報告した今の値（送信間隔・平均回数・メジアン回数・異常時の自動再起動）","自動"],["異常時の自動再起動（分）","子機の処理が止まってしまったときに、自動で再起動して立ち直るまでの時間。送信間隔から自動で決まる（送信間隔＋15分）。入力は不要","自動"],["変更後",{"text": "変えたい値。空欄の項目は変えない（現在値のまま）","bold": [[6,16]]},"手入力"],["設定を変更する","チェックすると「変更後」の値で予約する","☑"],["ステータス確認","設定を変えずに、子機に今の値を報告させる","☑"],["予約を取り消す","まだ完了していない予約を取り消す","☑"],["状態","予約の進み具合と結果","自動"],["最終更新","状態が最後に変わった日時","自動"]],"important": false},
  {"type": "h2","text": "1.2 入力できる値","important": false},
  {"type": "table","header": ["項目","範囲"],"rows": [["送信間隔（分）","1〜1440"],["平均回数","1〜255"],["メジアン回数","1〜20"]],"important": false},
  {"type": "h2","text": "1.3 手順","important": false},
  {"type": "p","text": {"text": "1. 初めて操作する子機は、まず「ステータス確認」（子機の現在値が分からないと、空欄＝「現在値のまま」が使えないため）","bold": [[3,25]]},"important": false},
  {"type": "p","text": "2. 「変更後」に変えたい項目だけ入れる","important": false},
  {"type": "p","text": {"text": "3. 「設定を変更する」にチェック → チェックはすぐ外れ、入力欄は空に戻り、状態が「予約中」になる","bold": [[20,50]]},"important": false},
  {"type": "p","text": "4. 状態が「完了」になるのを待つ（0.1 の目安）。途中経過は「設定変更履歴」シートでも確認できる（予約・送信・結果の行が順に増える）","important": false},
  {"type": "h2","text": "1.4 知っておくこと","important": false},
  {"type": "p","text": {"text": "・1台の子機に予約できるのは1つだけ。 完了前に新しい予約をすると、前の予約は上書きされる（前の予約は取り消し扱い。設定変更履歴 に「未完了の予約を上書きしました」と残る）","bold": [[1,19],[34,45]]},"important": false},
  {"type": "p","text": {"text": "・送信間隔を変えると、子機は自動で再起動する（新しい間隔に合わせて見張り役のタイマーを張り直すため。正常な動作）","bold": [[1,22]]},"important": false},
  {"type": "p","text": "・チェックを付けても何も変わらない（チェックが付いたまま・状態も変わらない）ときは、チェックを外してもう一度付ける","important": false},
  {"type": "p","text": {"text": "・送信間隔は Gateway の送信間隔とのルール（§3.2）を満たす必要がある","bold": [[22,31]]},"important": false},
  {"type": "h2","text": "1.5 状態欄の表示一覧（子機）","important": false},
  {"type": "table","header": ["表示","意味","対処"],"rows": [["予約中（Gatewayの取得待ち）","Gateway がまだ予約を受け取っていない","待つ（最大で Gateway の送信間隔ぶん）"],["送信済み（子機の確認待ち）","Gateway は子機へ送った。子機の確認応答待ち","待つ（子機の次の送信のあと、次の Gateway 送信サイクルで結果が出る）"],["（ステータス確認） が付いている","ステータス確認の予約","同上"],["完了","子機が要求どおり適用した","「子機の現在値」を確認"],["完了（値を丸めた: …）",{"text": "子機が制約に合わせて要求と違う値を適用した","bold": [[10,16]]},"表示された値が実際の値。問題があれば入れ直す"],["失敗（子機が値域エラーで拒否。設定は変更されていない）","子機が受け付けられない値だった","範囲（1.2）を確認して入れ直す"],["失敗（子機の flash 保存に失敗。設定は変更されていない）","子機が設定を保存できなかった","もう一度予約。繰り返す場合は子機の点検"],["失敗（未達。○回試行しても確認が返らず）","Gateway が3回送っても子機から応答が無かった","子機が動いているか（電池・電波）を確認して、もう一度予約"],["予約を取り消しました","取り消しが済んだ","—"]],"important": false},
  {"type": "h1","text": "2. 同時にできないこと・順番の決まり（まとめ）"},
  {"type": "table","header": ["決まり","理由","こうする"],"rows": [[{"text": "Gateway 宛ての予約は1つだけ（送信間隔の変更・Gateway操作の各コマンド・一括変更の Gateway 分はすべて同じ枠）","bold": [[0,18]]},"Gateway は1回に1つの指示しか受け取らない","予約が「完了」してから次の操作をする。急ぐときは「予約を取り消す」→ 新しい操作"],[{"text": "子機は1台に1つ。新しい予約は前の予約を上書き","bold": [[3,8]]},"同上（子機ごと）","上書きしてよいか確認してから操作する"],[{"text": "子機の送信間隔を短くするときは、Gateway の送信間隔が先に短くなっている必要がある（個別の欄の場合）","bold": [[8,10]]},"子機が短くなると Gateway がためきれずにデータを捨てる",{"text": "一括変更（§4）を使うと、順番は自動で調整される","bold": [[0,11]]}],[{"text": "Gateway の送信間隔を長くするときは、子機の送信間隔が先に長くなっている必要がある（個別の欄の場合）","bold": [[14,16]]},"同上","同上"],["一括変更の「2段目待ち」の間に、Gateway の個別予約や新しい一括変更をすると、2段目は中止される","古い2段目が後から実行されて、新しい設定を上書きしないため","2段目が済むまで待つ。中止されたら改めて一括変更する"]],"important": true},
  {"type": "h1","text": "3. Gateway の送信間隔（「Gateway設定」シート 上段）"},
  {"type": "h2","text": "3.1 欄の見方","important": false},
  {"type": "table","header": ["項目","内容"],"rows": [["送信間隔（分） 現在値","Gateway が報告してきた今の送信間隔"],["子機の最短間隔（分）","いちばん短い子機の送信間隔と、Gateway に設定できる上限（「→ Gateway は○分まで設定可」）。「※現在値不明」が付いた子機はステータス確認をすると判定に入る"],["通信異常時の自動再起動（分）","クラウドへ送れない状態が続いたときに、Gateway が自動で再起動して立ち直るまでの時間。送信間隔から自動で決まる（送信間隔の2.5倍、最低でも送信間隔＋10分）"],["変更後（分）","新しい送信間隔を入れて「設定を変更する」にチェック"],["状態","予約中／— と注意書き"],["最終確認","Gateway が最後に今の値を知らせてきた日時"]],"important": false},
  {"type": "h2","text": "3.2 送信間隔のルール","important": false},
  {"type": "p","text": {"text": "・Gateway の送信間隔 ≦ いちばん短い子機の送信間隔 × 5\n（Gateway は子機1台につき最大6件までためて送る。子機の送信のずれで1件多く入ることがあるので、余裕を1件分とって×5）","bold": [[1,34]]},"important": false},
  {"type": "p","text": "・子機の台数が多いときは、Gateway 全体でためられる件数（96件）も確認される","important": false},
  {"type": "p","text": "・ルールを外れる操作は「入力エラー」になり、予約されない","important": false},
  {"type": "p","text": "・状態欄に「⚠ Gateway の間隔が長すぎるため…」が出たら、今の設定でデータが捨てられている。すぐに直す","important": false},
  {"type": "h1","text": "4. 一括変更（「Gateway設定」シート 中段「一括変更（Gateway＋全子機）」）"},
  {"type": "p","text": {"text": "Gateway と、その群のすべての子機の設定を1回で予約する。 個別の欄と違い、変更後の値どうしでルールを確認し、順番も自動で調整するので、「どちらを先に変えるか」を考えなくてよい。","bold": [[0,32],[41,68]]},"important": false},
  {"type": "table","header": ["項目","内容"],"rows": [["子機の送信間隔・平均回数・メジアン回数",{"text": "全子機に同じ値を入れる。空欄は変えない（子機ごとの現在値のまま）","bold": [[12,19]]}],["Gateway の送信間隔","空欄なら変えない"],["まとめて変更する","チェックで予約"],["結果","予約した内容、または入力エラー"]],"important": false},
  {"type": "p","text": {"text": "・2段階で反映されることがある: データが抜けないよう、Gateway を長くするときは「子機が全部完了してから Gateway」、子機を短くするときは「Gateway が完了してから子機」の順に自動で予約する。結果欄に「（2段目: …）」と出て、「Gateway設定」の状態欄に「一括変更の2段目待ち」と表示される","bold": [[1,16]]},"important": false},
  {"type": "p","text": "・現在値が分からない子機があるときは、子機の3項目すべての入力が必要（またはその子機でステータス確認をしてから）","important": false},
  {"type": "p","text": {"text": "・エラーのときは何も予約されない（一部の子機だけ予約された状態にはならない）","bold": [[8,16]]},"important": false},
  {"type": "h1","text": "5. Gateway操作（「Gateway設定」シート 下段「Gateway操作」）"},
  {"type": "table","header": ["操作","何が起きるか","注意"],"rows": [["リモートリセット","Gateway が再起動する",{"text": "Gateway が持っている送信待ちのデータは消える","bold": [[0,26]]}],["データ送信を停止","子機からの受信・予約の確認は続けたまま、クラウドへの送信だけ止める",{"text": "止めた状態は Gateway に保存され、再起動しても止まったまま。必ず「再開」まで行う。止めている間のデータは Gateway にたまり、多すぎると古いものから捨てられる","bold": [[0,33]]}],["データ送信を再開","送信を再開する","—"],["今すぐ送信","停止中でも、Gateway が持っているデータをその場で送る","—"],["ステータス確認","「Gatewayステータス」シートに Gateway の状態を1行書く（電波・稼働時間・メモリ・たまっている件数・送信の成否の累計など）","—"],["RTC再同期","携帯回線の時刻で Gateway の時計を合わせ直し、ステータスを報告する","—"],["診断ログを吸い上げ","「Gateway診断ログ」シートに、Gateway の直近のできごと（最大24件）を書く","再起動すると消える記録なので、再起動の前に吸い上げる"],["予約を取り消す","Gateway 宛ての予約（送信間隔の変更・上の各操作・一括変更の2段目待ち）を取り消す","—"]],"important": false},
  {"type": "p","text": "・実行されるのは Gateway の次の送信サイクル（最大で送信間隔ぶん）","important": false},
  {"type": "p","text": "・「現在の予約」に予約中の操作、「送信の状態」に送信中／★送信停止中（直近のステータス確認の結果）が出る","important": false},
  {"type": "h1","text": "6. エラー・メッセージ一覧"},
  {"type": "h2","text": "6.1 子機の欄","important": false},
  {"type": "table","header": ["表示","原因","対処"],"rows": [["エラー: 「シート名編集」にこのシートが有効として登録されていません","このシートと子機の対応が「シート名編集」に無い、または「無効」","「シート名編集」に DeviceID・シート名・「有効」を入れる"],["エラー: 0x○○ はダウンリンク対象外の DeviceID です","子機の群が、この Gateway の群と違う","DeviceID と Gateway の群を確認する"],["入力エラー: 子機の現在値が不明なため○○を入力してください（先にステータス確認でも可）","子機の今の値を GAS が知らないのに、空欄の項目がある","先に「ステータス確認」をする。または3項目すべて入れる"],["入力エラー: ○○は○〜○の整数で入力してください","範囲外・整数でない","範囲（1.2）で入れ直す"],["入力エラー: Gateway の送信間隔（○分）が子機の送信間隔（○分）× 5を超えます。…","子機を短くしすぎ（§3.2 のルール）","表示どおり子機を長くするか、先に Gateway を短くする。または一括変更を使う"],["入力エラー: Gateway の全体容量を超えます（見積もり○件 / 最大96件）。…","子機の台数×短い間隔で Gateway がためきれない","送信間隔を長くする（子機）か短くする（Gateway）"],["エラー: 予約に失敗しました。再度操作してください","同時に別の処理が動いていて予約できなかった","少し待ってもう一度"],["予約はありません","取り消す予約が無い","—"],["エラー: 予約の取消に失敗しました","同時に別の処理が動いていた","少し待ってもう一度"]],"important": false},
  {"type": "h2","text": "6.2 Gateway設定・一括変更・Gateway操作","important": false},
  {"type": "table","header": ["表示","原因","対処"],"rows": [["予約中のコマンド（○○）があります。完了を待つか『予約を取り消す』で取り消してから操作してください","Gateway 宛ての予約枠が使用中（§2）","完了を待つか、取り消してから操作"],["入力エラー: 変更後の送信間隔（分）を入力してください","変更後が空欄","値を入れる"],["入力エラー: Gateway の送信間隔（○分）が子機の送信間隔（○分）× 5を超えます。…","Gateway を長くしすぎ","表示どおり Gateway を短くするか、先に子機を長くする。または一括変更"],["入力エラー: Gateway の全体容量を超えます …","同上（台数が多い場合）","同上"],["入力エラー: 変更する値を入力してください（一括変更）","何も入っていない","値を入れる"],["入力エラー: 現在値が不明な子機があります（0x○○）。…","一括変更で空欄の項目があり、現在値の分からない子機がある","子機の3項目をすべて入れるか、その子機でステータス確認"],["入力エラー: 変更後: …","変更後の組み合わせがルールを外れる","表示どおりに値を直す"],["入力エラー: 途中状態で容量を超えます。2回に分けて変更してください","片方だけ先に変わった途中の状態でもデータが抜けてしまう組み合わせ","2回に分けて変更する（まず片方、完了後にもう片方）"],["エラー: 予約に失敗しました: …","同時に別の処理が動いていた","少し待ってもう一度"]],"important": false},
  {"type": "h1","text": "7. 結果・履歴を見る場所"},
  {"type": "table","header": ["シート","何が分かるか","お客様への表示"],"rows": [[{"text": "設定変更履歴","bold": [[0,6]]},{"text": "予約状況と履歴。すべての予約・送信・結果・取消が時刻付きで並ぶ（いつ、どの操作で、何が、どうなったか）。今どの予約が進行中かも、ここで確認できる（「予約」の行のあとに「結果」の行がまだ無いものが進行中）","bold": [[0,7],[52,72]]},{"text": "見せてよい","bold": [[0,5]]}],["cmd_status","今の予約の一覧（Gateway と全子機を1行ずつ）","社内のみ（非表示）"],["Gatewayステータス","「ステータス確認」の結果","社内のみ（非表示）"],["Gateway診断ログ","「診断ログを吸い上げ」の結果","社内のみ（非表示）"],["Gateway起動ログ","Gateway が起動するたびの記録（再起動の理由・機器の識別番号を含む）","社内のみ（非表示）"],["invalid_payload_log","Gateway から壊れたデータが届いた記録（通常は空）","社内のみ（非表示）"]],"important": false},
  {"type": "p","text": "設定変更履歴 の見方（「子機ID」は10進数。「シート名編集」の DeviceID と同じ番号。Gateway の行は Gateway 名）:","important": false},
  {"type": "table","header": ["種別","意味"],"rows": [["予約 / ステータス確認要求","シートから予約した"],["送信","Gateway が子機へ送った（○回目）"],["結果","子機の確認応答が届いた（要求した値 → 実際に適用された値、完了／失敗）"],["取消","予約を取り消した"],["結果(期限切れ)","予約を入れ直す前の古い予約の結果が遅れて届いた（子機には実際に適用されている）"],["Gateway間隔 予約 / 変更確認 / 完了","Gateway の送信間隔の変更"],["Gatewayコマンド 予約 / 完了 / 取消","Gateway操作（リセット・停止など）"],["一括変更 予約 / 一括変更の2段目 …","一括変更とその2段目"]],"important": false},
  {"type": "h1","text": "8. 困ったとき"},
  {"type": "table","header": ["症状","考えられること","対処"],"rows": [["「予約中（Gatewayの取得待ち）」から進まない","Gateway が止まっている・圏外、または Gateway の送信間隔が長い","Gateway の送信間隔ぶん待つ。設定変更履歴 に「送信」の行が出ていないかも確認。それでも進まなければ「Gateway起動ログ」「Gatewayステータス」で Gateway が動いているか確認"],["「送信済み（子機の確認待ち）」から進まない","子機がまだ送信していない（送信間隔が長い）","子機の送信間隔ぶん待つ。現場にいれば、v3.20 基板は D0 ボタン短押しですぐ送信させられる"],["「失敗（未達…）」になった","子機が止まっている・電波が届かない・電池切れ","子機の状態を確認してもう一度予約"],["チェックを付けても何も起きない","操作がうまく伝わらなかった","チェックを外して付け直す。続く場合は管理者へ"],["データ行が急に来なくなった","「データ送信を停止」のまま、または Gateway の停止","「Gateway操作」の「送信の状態」と「Gatewayステータス」を確認"]],"important": false},
];

function UI_guidePlain_(value) {
  return typeof value === 'string' ? value : value.text;
}

function UI_guideText_(range, value) {
  if (typeof value === 'string') { range.setValue(value); return; }
  const builder = SpreadsheetApp.newRichTextValue().setText(value.text);
  const bold = SpreadsheetApp.newTextStyle().setBold(true).build();
  value.bold.forEach(function (span) { builder.setTextStyle(span[0], span[1], bold); });
  range.setRichTextValue(builder.build());
}

function UI_guideImportant_(text) {
  return /必読|注意|消える|止まったまま|リモートリセット|データ送信を停止/.test(text);
}

function UI_guideHeight_(text, width) {
  // ★2026-09-24 結合セルは自動調整だけで高さが足りないことがあるので、折り返し分を補う。
  const chars = Math.max(1, Math.floor((width - 24) / 14));
  const lines = text.split('\n').reduce(function (n, line) { return n + Math.max(1, Math.ceil(line.length / chars)); }, 0);
  return Math.max(28, lines * 20 + 12);
}

// 【エディタから実行】★2026-09-24 原稿を読みやすい表として収録。繰り返し実行しても同じ内容にする。
function setupOperationGuide() {
  const ss = getSpreadsheet();
  let sheet = ss.getSheetByName(OPERATION_GUIDE_SHEET_NAME);
  if (!sheet) sheet = ss.insertSheet(OPERATION_GUIDE_SHEET_NAME);
  sheet.getRange(1, 1, sheet.getMaxRows(), sheet.getMaxColumns()).breakApart();
  sheet.clear();
  sheet.setConditionalFormatRules([]);
  const widths = [230, 350, 240, 180];
  const needed = UI_GUIDE_DATA.reduce(function (n, item) {
    return n + (item.type === 'table' ? item.rows.length + 2 : item.type === 'h1' ? 2 : 1);
  }, 4);
  if (sheet.getMaxRows() < needed) sheet.insertRowsAfter(sheet.getMaxRows(), needed - sheet.getMaxRows());
  sheet.setRowHeights(1, needed, 28);
  sheet.setColumnWidth(1, 20);
  widths.forEach(function (w, i) { sheet.setColumnWidth(i + 2, w); });
  sheet.getRange(1, 2, needed, 4).setFontFamily(UI_.font).setFontSize(10)
       .setFontColor(UI_.labelText).setFontWeight('normal').setVerticalAlignment('top').setWrap(true);
  sheet.getRange(1, 2, 1, 4).merge().setValue('スプレッドシートからの設定変更（ダウンリンク）操作ガイド')
       .setFontSize(18).setFontWeight('bold').setFontColor(UI_.navy);
  sheet.setRowHeight(1, 42);
  sheet.getRange(2, 2, 1, 4).merge().setValue(
    '対象: 設定欄を操作する社内担当者（設定欄はコクリエのアカウントだけが編集できる）\n' +
    '対象の仕組み: 案件0013 雛形（Gateway FW24 / 子機 FW13・v3.20 FW14 / 雛形GAS）／更新日: 2026-09-24')
    .setFontSize(9).setFontColor(UI_.autoText);
  sheet.setRowHeight(2, 46);
  let row = 3;
  UI_GUIDE_DATA.forEach(function (item) {
    if (item.type === 'h1') row++;   // 大見出しの上に1行空ける
    if (item.type === 'table') {
      const tableStart = row, cells = [item.header].concat(item.rows);
      cells.forEach(function (values, index) {
        let height = 28;
        const texts = values.map(UI_guidePlain_);
        const important = item.important || texts.some(UI_guideImportant_);
        values.forEach(function (value, col) {
          const span = col === values.length - 1 ? 4 - col : 1;
          const range = sheet.getRange(row, col + 2, 1, span);
          if (span > 1) range.merge();
          UI_guideText_(range, value);
          range.setBackground(index === 0 ? UI_.sub : index % 2 ? '#ffffff' : '#f7f9fc');
          if (index === 0) range.setFontWeight('bold');
          if (important) range.setFontColor(UI_.errorText).setFontWeight('bold');
          const width = widths.slice(col, col + span).reduce(function (a, w) { return a + w; }, 0);
          height = Math.max(height, UI_guideHeight_(texts[col], width));
        });
        sheet.autoResizeRows(row, 1);
        sheet.setRowHeight(row, height);
        row++;
      });
      UI_frame_(sheet.getRange(tableStart, 2, cells.length, 4));
      row++;
      return;
    }
    const range = sheet.getRange(row, 2, 1, 4).merge();
    UI_guideText_(range, item.text);
    const text = UI_guidePlain_(item.text);
    if (item.type === 'h1') {
      range.setBackground(UI_.navy).setFontColor('#ffffff').setFontSize(12).setFontWeight('bold');
    } else if (item.type === 'h2') {
      range.setFontSize(11).setFontWeight('bold').setFontColor(UI_.navy)
           .setBorder(null, null, true, null, null, null, UI_.line, SpreadsheetApp.BorderStyle.SOLID);
    }
    // 大見出しは白文字の帯を優先し、重要な本文・小見出し・表を赤く強調する。
    if (item.type !== 'h1' && (item.important || UI_guideImportant_(text))) range.setFontColor(UI_.errorText).setFontWeight('bold');
    sheet.autoResizeRows(row, 1);
    sheet.setRowHeight(row, Math.max(item.type === 'h1' ? 34 : 28, UI_guideHeight_(text, 1000)));
    row++;
  });
  sheet.setHiddenGridlines(true);
  sheet.setFrozenRows(2);
  sheet.setTabColor(UI_.navy);
  const description = '操作ガイド（編集は管理者のみ）';
  const protections = sheet.getProtections(SpreadsheetApp.ProtectionType.SHEET);
  const own = protections.filter(function (p) { return p.getDescription() === description && p.isWarningOnly(); });
  if (!own.length) sheet.protect().setDescription(description).setWarningOnly(true);
  // 移動前に自身を除いて数えると、現在位置が前後どちらでも Gateway設定 の直前になる。
  const others = ss.getSheets().filter(function (s) { return s.getName() !== OPERATION_GUIDE_SHEET_NAME; });
  const gatewayIndex = others.map(function (s) { return s.getName(); }).indexOf(GW_SHEET_NAME);
  ss.setActiveSheet(sheet);
  ss.moveActiveSheet(gatewayIndex < 0 ? 1 : gatewayIndex + 1);
}
