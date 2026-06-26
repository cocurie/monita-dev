/**
 * Monita Gateway v1 — BLE スキャナ + LTE-M → GAS 送信
 *
 * MCU    : Seeed XIAO nRF52840
 * 通信   : M5Stamp CAT-M（SIM7080G）
 * RTC    : DS3231（I2C: D4=SDA, D5=SCL）
 * SD     : microSD SPI（D3=CS, D8=SCK, D9=MISO, D10=MOSI）
 *
 * 配線:
 *   XIAO D6 (TX) → SIM7080G RX
 *   XIAO D7 (RX) ← SIM7080G TX
 *   XIAO 5V      → SIM7080G 5V
 *   XIAO GND     → SIM7080G GND
 *   XIAO D4(SDA) → DS3231 SDA
 *   XIAO D5(SCL) → DS3231 SCL
 *   XIAO 3V3     → DS3231 VCC / SD VDD
 *   XIAO D3      → SD CS
 *   XIAO D8(SCK) → SD CLK
 *   XIAO D9(MISO)→ SD DAT0
 *   XIAO D10(MOSI)→ SD CMD
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

// ── デバッグレベル ─────────────────────────────
// 0: 無効  1: ステージ結果のみ  2: AT コマンド生ログも表示
#define DEBUG_LEVEL 2

#if DEBUG_LEVEL >= 1
  #define DLOG(msg)       Serial.println(F(msg))
  #define DLOGV(msg, val) do { Serial.print(F(msg)); Serial.println(val); } while(0)
#else
  #define DLOG(msg)
  #define DLOGV(msg, val)
#endif

#if DEBUG_LEVEL >= 2
  #define DLOG2(msg)       Serial.println(F(msg))
  #define DLOGV2(msg, val) do { Serial.print(F(msg)); Serial.println(val); } while(0)
#else
  #define DLOG2(msg)
  #define DLOGV2(msg, val)
#endif

  #include <bluefruit.h>
#include <RTClib.h>
#include <SD.h>
#include <SPI.h>

// ══════════════════════════════════════════════
// ▼ ユーザー設定
// ══════════════════════════════════════════════

// GAS スクリプトID（デプロイURLの "AKfycb..." 部分）
const char* GAS_SCRIPT_ID = "AKfycbw2IIQ1GyxtGh2Uis_zmwXW3VhftDy9HWKw5tSsUbwNOhNo6p9PnNv3ftfs3MvcMDT5ww/exec";

// テストモード — true にするとネットワーク接続直後にダミーデータを GAS へ送信する
#define TEST_MODE false

// SIM 切り替え — 使う方のブロックだけ有効にする
// ── 1NCE SIM ──────────────────────────────────
#define SIM_1NCE
// ── Plan-D SIM ────────────────────────────────
// #define SIM_PLAN_D

#if defined(SIM_1NCE)
  const char* APN      = "iot.1nce.net";
  const char* SIM_NAME = "1NCE";
  const char* APN_USER = "";
  const char* APN_PASS = "";
#elif defined(SIM_PLAN_D)
  const char* APN      = "planex.net";   // ← Plan-D の正式 APN に変更すること
  const char* SIM_NAME = "Plan-D";
  const char* APN_USER = "";             // ← 必要に応じて設定
  const char* APN_PASS = "";
#else
  #error "SIM_1NCE または SIM_PLAN_D のどちらかを define してください"
#endif

// BLE スキャン / 送信設定
static uint16_t const SCAN_INTERVAL_MS   = 300;     // スキャンインターバル (ms)
static uint16_t const SCAN_WINDOW_MS     = 30;      // スキャンウィンドウ (ms)
static uint32_t const SEND_INTERVAL_MS   = 300000;  // GAS 送信インターバル (ms) ★ここを変える
static uint8_t  const MFR_COMPANY_ID_H   = 0xFF;    // Flex の Company ID (上位)
static uint8_t  const MFR_COMPANY_ID_L   = 0xFF;    // Flex の Company ID (下位)

// SD カード
static int const SD_CS_PIN = 3;  // D3

// ══════════════════════════════════════════════
// BLE 受信バッファ
// ══════════════════════════════════════════════
#define MAX_DEVICES 20
#define MAX_PAYLOAD 16

struct FlexRecord {
  uint8_t  mac[6];
  uint8_t  payload[MAX_PAYLOAD];
  uint8_t  payloadLen;
  int      rssi;
  uint32_t lastSeen; // millis()
};

static FlexRecord records[MAX_DEVICES];
static int        recordCount = 0;
static SemaphoreHandle_t recordMutex;

// ══════════════════════════════════════════════
// RTC
// ══════════════════════════════════════════════
static RTC_DS3231 rtc;
static bool rtcAvailable = false;

String getTimestamp() {
  if (!rtcAvailable) return String(millis() / 1000UL) + "s";
  DateTime now = rtc.now();
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute(), now.second());
  return String(buf);
}

// ══════════════════════════════════════════════
// AT コマンド送受信
// ══════════════════════════════════════════════
String sendAT(String cmd, int waitMs = 5000) {
  DLOG2("--");
  DLOGV2(">> ", cmd);
  Serial1.print(cmd + "\r\n");
  long start = millis();
  String res = "";
  while (millis() - start < waitMs) {
    while (Serial1.available()) res += (char)Serial1.read();
    yield();
  }
  if (res.length() > 0) { DLOG2("<< "); DLOG2(res.c_str()); }
  else                   { DLOG2("<< (応答なし)"); }
  return res;
}

// ステージ結果を 1 行で出力
void simStage(const char* name, bool ok) {
  Serial.print(ok ? F("[OK] ") : F("[NG] "));
  Serial.println(name);
}

// ══════════════════════════════════════════════
// ネットワーク初期化（ltem_signal_test から流用）
// ══════════════════════════════════════════════
bool initNetwork() {
  // CREG モードをデフォルト（n=0）にリセット
  sendAT("AT+CREG=0", 2000); delay(200);

  // バンド設定を全バンドにリセット（AT&F では CBANDCFG がリセットされないため明示的に設定）
  sendAT("AT+CBANDCFG=\"CAT-M\",1,2,3,4,5,8,12,13,18,19,20,25,26,28,66,71,85", 3000); delay(500);

  // LTE-M モード・APN 設定
  sendAT("AT+CNMP=38"); delay(500);  // LTE only
  sendAT("AT+CMNB=1");  delay(500);  // Cat-M1
  sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\""); delay(500);
  if (strlen(APN_USER) > 0) {
    sendAT("AT+CGAUTH=1,1,\"" + String(APN_PASS) + "\",\"" + String(APN_USER) + "\""); delay(500);
  }
  simStage("NET1: LTE-M モード & APN 設定", true);

  // CREG: ネットワーク登録確認（最大 60 秒）
  bool cregOk = false;
  Serial.print(F("[   ] NET2: ネットワーク登録待ち (最大60秒)"));
  for (int i = 0; i < 12; i++) {
    Serial.print('.');
    String reg = sendAT("AT+CREG?", 3000);
    // stat=1（登録済みホーム）または stat=5（ローミング）を検出
    // n=0: "+CREG: 0,1"  n=2: "+CREG: 2,1,..." どちらにも対応
    int cregComma = reg.indexOf("+CREG: ");
    if (cregComma >= 0) {
      int statComma = reg.indexOf(",", cregComma + 7);
      if (statComma >= 0) {
        char stat = reg.charAt(statComma + 1);
        if (stat == '1' || stat == '5') { cregOk = true; break; }
      }
    }
    if (i < 11) delay(5000);
  }
  Serial.println();
  simStage("NET2: ネットワーク登録 (CREG=1 or 5)", cregOk);

  if (!cregOk) {
    Serial.println(F("\n--- NET2 NG: 原因診断 ---"));

    // 電波強度確認
    String csq = sendAT("AT+CSQ", 3000);
    int csqVal = -1;
    int csqIdx = csq.indexOf("+CSQ: ");
    if (csqIdx >= 0) csqVal = csq.substring(csqIdx + 6, csq.indexOf(",", csqIdx)).toInt();
    Serial.print(F("  CSQ: "));
    if (csqVal == 99 || csqVal < 0) Serial.println(F("99 → 圏外またはアンテナ未接続"));
    else if (csqVal >= 20)          Serial.println(String(csqVal) + " → 電波良好");
    else if (csqVal >= 10)          Serial.println(String(csqVal) + " → 電波普通");
    else                            Serial.println(String(csqVal) + " → 電波弱い");

    // 詳細ネットワーク状態
    String cpsi = sendAT("AT+CPSI?", 3000);
    Serial.print(F("  CPSI: "));
    if (cpsi.indexOf("NO SERVICE") >= 0) Serial.println(F("NO SERVICE → 電波なし / アンテナ未接続"));
    else {
      int pi = cpsi.indexOf("+CPSI:");
      Serial.println(pi >= 0 ? cpsi.substring(pi) : cpsi);
    }

    // 詳細 CREG
    sendAT("AT+CREG=2", 2000);
    String creg2 = sendAT("AT+CREG?", 3000);
    Serial.print(F("  CREG詳細: ")); Serial.println(creg2);

    // バンド設定確認
    String bandCfg = sendAT("AT+CBANDCFG?", 3000);
    Serial.print(F("  CBANDCFG: ")); Serial.println(bandCfg);

    // Cat-M1 Band 18（KDDI 日本）を追加してリスキャン
    Serial.println(F("  → Cat-M1 Band 18 (KDDI) を設定してリスキャン..."));
    sendAT("AT+CBANDCFG=\"CAT-M\",18", 3000); delay(500);
    sendAT("AT+CMNB=1", 3000); delay(500);  // Cat-M1 のみ

    // リスキャン待ち（最大 30 秒）
    bool rescanOk = false;
    Serial.print(F("  リスキャン中"));
    for (int i = 0; i < 6; i++) {
      Serial.print('.');
      delay(5000);
      String reg2 = sendAT("AT+CREG?", 3000);
      int cregComma2 = reg2.indexOf("+CREG: ");
      if (cregComma2 >= 0) {
        int statComma2 = reg2.indexOf(",", cregComma2 + 7);
        if (statComma2 >= 0) {
          char stat2 = reg2.charAt(statComma2 + 1);
          if (stat2 == '1' || stat2 == '5') { rescanOk = true; break; }
        }
      }
    }
    Serial.println();
    simStage("NET2-RETRY: Band18 + CMNB=3 でリスキャン", rescanOk);

    String cpsi2 = sendAT("AT+CPSI?", 3000);
    Serial.print(F("  CPSI(再): "));
    int pi2 = cpsi2.indexOf("+CPSI:");
    Serial.println(pi2 >= 0 ? cpsi2.substring(pi2) : cpsi2);

    if (!rescanOk) {
      // 無線スタックリセット後にリスキャン
      Serial.println(F("  → 無線スタックをリセット (CFUN=0→1)..."));
      sendAT("AT+CFUN=0", 5000); delay(2000);
      sendAT("AT+CFUN=1", 5000); delay(5000);
      sendAT("AT+CNMP=38", 2000); delay(500); // LTE のみ
      sendAT("AT+CMNB=1",  2000); delay(500); // Cat-M1 のみ

      bool cfunOk = false;
      Serial.print(F("  CFUN リセット後リスキャン中"));
      for (int i = 0; i < 12; i++) {
        Serial.print('.');
        delay(5000);
        String reg3 = sendAT("AT+CREG?", 3000);
        int ci = reg3.indexOf("+CREG: ");
        if (ci >= 0) {
          int sc = reg3.indexOf(",", ci + 7);
          if (sc >= 0 && (reg3.charAt(sc + 1) == '1' || reg3.charAt(sc + 1) == '5')) {
            cfunOk = true; break;
          }
        }
      }
      Serial.println();
      simStage("NET2-CFUN: 無線リセット後リスキャン", cfunOk);

      // 利用可能ネットワーク全スキャン（AT+COPS=?）
      Serial.println(F("  → 利用可能ネットワークをスキャン中（最大3分）..."));
      String cops = sendAT("AT+COPS=?", 180000);
      Serial.print(F("  COPS: "));
      int copsIdx = cops.indexOf("+COPS:");
      if (copsIdx >= 0) Serial.println(cops.substring(copsIdx));
      else              Serial.println(F("(応答なし / ネットワーク見つからず)"));

      if (cfunOk) rescanOk = true;
    }

    Serial.println(F("  ▶ 確認事項:"));
    Serial.println(F("    1. アンテナが M5Stamp SIM7080G に接続されているか"));
    Serial.println(F("    2. 1NCE ポータル(sim.1nce.net)で SIM が Active か確認"));
    Serial.println(F("    3. 電波の弱い場所にいないか"));
    Serial.println(F("-------------------------"));

    if (rescanOk) cregOk = true;
  }

  // CGATT: データ Attach 確認
  // 登録直後は Attach 完了待ちが必要。5秒×12回=最大60秒リトライ
  // まだ 0 なら AT+CGACT=1,1 で手動アクティベートを試みる
  bool attachOk = false;
  Serial.print(F("[   ] NET3: データ Attach 待ち"));
  for (int i = 0; i < 12; i++) {
    Serial.print('.');
    delay(5000);
    String att = sendAT("AT+CGATT?", 3000);
    if (att.indexOf("+CGATT: 1") >= 0) { attachOk = true; break; }
  }
  if (!attachOk) {
    // 手動で PDP コンテキストをアクティベート
    Serial.print(F(" (CGACT試行)"));
    sendAT("AT+CGACT=1,1", 10000);
    delay(3000);
    String att2 = sendAT("AT+CGATT?", 3000);
    if (att2.indexOf("+CGATT: 1") >= 0) attachOk = true;
  }
  Serial.println();
  simStage("NET3: データ Attach (CGATT=1)", attachOk);
  if (!attachOk) {
    Serial.println(F("  → CGATT=0 のまま。APN 設定・SIM 契約を確認してください"));
    return false;
  }
  delay(1000);

  // CNACT: IP アドレス取得
  sendAT("AT+CNACT=0,1", 15000); delay(3000);
  String cnact = sendAT("AT+CNACT?", 3000);
  bool ipOk = cnact.indexOf("0,1") >= 0;
  simStage("NET4: IP アドレス取得 (CNACT)", ipOk);
  if (!ipOk) { DLOGV("  → CNACT 応答: ", cnact); return false; }

  return true;
}

// ══════════════════════════════════════════════
// GAS 送信（ltem_signal_test の postToSheet を踏襲）
// ══════════════════════════════════════════════


// 全 Flex レコードを1回の GET でまとめて GAS へ送信
// クエリ形式: ts=...&sim=...&n=3&mac0=...&payload0=...&rssi0=...&mac1=...
bool postToGAS(String queryParams) {
  Serial.println(F("\n--- GAS 送信 ---"));
  String scriptPath = "/macros/s/";
  scriptPath += GAS_SCRIPT_ID;
  scriptPath += "?";
  scriptPath += queryParams;

  sendAT("AT+SHDISC", 2000); delay(300);
  sendAT("AT+CSSLCFG=\"ignorertctime\",1,1"); delay(200);
  sendAT("AT+CSSLCFG=\"sslversion\",1,3");    delay(200);
  sendAT("AT+CSSLCFG=\"sni\",1,\"script.google.com\""); delay(200);
  sendAT("AT+SHSSL=1,\"\""); delay(200);
  sendAT("AT+SHCONF=\"BODYLEN\",1024");  delay(200);
  sendAT("AT+SHCONF=\"HEADERLEN\",350"); delay(200);
  sendAT("AT+SHCONF=\"URL\",\"https://script.google.com\""); delay(200);

  String conn = sendAT("AT+SHCONN", 15000);
  if (conn.indexOf("OK") < 0) { Serial.println(F("✗ 接続失敗")); return false; }

  String result = sendAT("AT+SHREQ=\"" + scriptPath + "\",1", 60000);

  int statusCode = 0;
  int si = result.indexOf("+SHREQ: ");
  if (si >= 0) {
    String s = result.substring(si + 8);
    int c1 = s.indexOf(","), c2 = s.indexOf(",", c1 + 1);
    if (c1 >= 0 && c2 > c1) statusCode = s.substring(c1 + 1, c2).toInt();
  }
  Serial.print(F("ステータスコード: ")); Serial.println(statusCode);

  // GAS は書き込み完了後に 302 を返す（200 は来ない）。302 も成功とみなす
  if (statusCode == 200 || statusCode == 302) {
    Serial.println(F("✓ GAS 送信成功！"));
    sendAT("AT+SHDISC"); return true;
  }
  Serial.println(F("✗ 予期しないレスポンス"));
  sendAT("AT+SHDISC"); return false;
}

// ══════════════════════════════════════════════
// SD カード操作
// ══════════════════════════════════════════════
static bool sdAvailable = false;

void sdLog(String line) {
  if (!sdAvailable) return;
  File f = SD.open("gateway.csv", FILE_WRITE);
  if (f) { f.println(line); f.close(); }
}

// ══════════════════════════════════════════════
// BLE スキャンコールバック
// ══════════════════════════════════════════════
void scanCallback(ble_gap_evt_adv_report_t* report) {
  // Manufacturer Specific Data を探す
  uint8_t* data = report->data.p_data;
  uint16_t len  = report->data.len;
  uint8_t* msd  = nullptr;
  uint8_t  msdLen = 0;

  for (uint16_t i = 0; i + 1 < len; ) {
    uint8_t fieldLen  = data[i];
    uint8_t fieldType = data[i + 1];
    if (fieldType == 0xFF && fieldLen >= 3) {  // AD Type: Manufacturer Specific
      if (data[i + 2] == MFR_COMPANY_ID_L && data[i + 3] == MFR_COMPANY_ID_H) {
        msd    = &data[i + 4];               // Company ID の後ろがペイロード
        msdLen = fieldLen - 3;
        if (msdLen > MAX_PAYLOAD) msdLen = MAX_PAYLOAD;
      }
    }
    if (fieldLen == 0) break;
    i += fieldLen + 1;
  }

  if (msd == nullptr) { Bluefruit.Scanner.resume(); return; }
  DLOG2("[BLE] MSD パケット受信");

  // レコード更新
  if (xSemaphoreTake(recordMutex, 0) == pdTRUE) {
    int idx = -1;
    for (int i = 0; i < recordCount; i++) {
      if (memcmp(records[i].mac, report->peer_addr.addr, 6) == 0) { idx = i; break; }
    }
    if (idx < 0 && recordCount < MAX_DEVICES) idx = recordCount++;
    if (idx >= 0) {
      memcpy(records[idx].mac, report->peer_addr.addr, 6);
      memcpy(records[idx].payload, msd, msdLen);
      records[idx].payloadLen = msdLen;
      records[idx].rssi       = report->rssi;
      records[idx].lastSeen   = millis();
    }
    xSemaphoreGive(recordMutex);
  }

  Bluefruit.Scanner.resume();
}

// ══════════════════════════════════════════════
// バッファを GAS へ送信＆SD へ記録（全台を1回の POST にまとめる）
// ══════════════════════════════════════════════
void flushRecords() {
  if (xSemaphoreTake(recordMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

  int n = recordCount;
  FlexRecord snap[MAX_DEVICES];
  memcpy(snap, records, sizeof(FlexRecord) * n);
  recordCount = 0;

  xSemaphoreGive(recordMutex);

  if (n == 0) { Serial.println(F("送信対象レコードなし")); return; }

  String ts = getTimestamp();
  Serial.print(F("フラッシュ: ")); Serial.print(n); Serial.println(F(" 件"));

  // クエリ文字列を1本に組み立てる
  // 形式: ts=...&sim=...&n=3&mac0=...&payload0=...&rssi0=...&mac1=...
  String params = "ts=";
  params += ts;
  params += "&sim=";
  params += SIM_NAME;
  params += "&n=";
  params += String(n);

  for (int i = 0; i < n; i++) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X-%02X-%02X-%02X-%02X-%02X",
             snap[i].mac[5], snap[i].mac[4], snap[i].mac[3],
             snap[i].mac[2], snap[i].mac[1], snap[i].mac[0]);

    String hex = "";
    for (int j = 0; j < snap[i].payloadLen; j++) {
      if (snap[i].payload[j] < 0x10) hex += '0';
      hex += String(snap[i].payload[j], HEX);
    }

    params += "&m"; params += i; params += "="; params += mac;
    params += "&p"; params += i; params += "="; params += hex;
    params += "&r"; params += i; params += "="; params += String(snap[i].rssi);

    // SD ログ（デバイスごとに1行）
    sdLog(ts + "," + String(mac) + "," + hex + "," + String(snap[i].rssi));
  }

  postToGAS(params);
}

// ══════════════════════════════════════════════
// setup
// ══════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Serial.println(F("\n===================================="));
  Serial.println(F("  Monita Gateway v1"));
  Serial.println(F("===================================="));
  Serial.print(F("SIM: ")); Serial.println(SIM_NAME);
  Serial.print(F("APN: ")); Serial.println(APN);

  // RTC 初期化
  if (rtc.begin()) {
    rtcAvailable = true;
    if (rtc.lostPower()) {
      Serial.println(F("△ RTC が電源喪失 → 時刻を再設定してください"));
      // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));  // ビルド時刻でセット（必要に応じて有効化）
    }
    Serial.println(F("✓ DS3231 初期化完了"));
    Serial.print(F("  現在時刻: ")); Serial.println(getTimestamp());
  } else {
    Serial.println(F("✗ DS3231 が見つかりません（タイムスタンプは millis 基準）"));
  }

  // SD カード初期化
  if (SD.begin(SD_CS_PIN)) {
    sdAvailable = true;
    Serial.println(F("✓ SD カード初期化完了"));
    // ヘッダ行がなければ書く
    if (!SD.exists("gateway.csv")) {
      File f = SD.open("gateway.csv", FILE_WRITE);
      if (f) { f.println("timestamp,mac,payload_hex,rssi"); f.close(); }
    }
  } else {
    Serial.println(F("✗ SD カード初期化失敗（SD なしで続行）"));
  }

  // BLE 初期化
  recordMutex = xSemaphoreCreateMutex();
  Bluefruit.begin(0, 1);  // 0 peripheral, 1 central
  Bluefruit.setName("MonitaGateway");
  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.useActiveScan(false);
  Bluefruit.Scanner.setInterval(
    (uint16_t)(SCAN_INTERVAL_MS * 1000 / 625),
    (uint16_t)(SCAN_WINDOW_MS   * 1000 / 625)
  );
  Bluefruit.Scanner.start(0);
  Serial.println(F("✓ BLE スキャン開始"));

  // ── SIM7080G 初期化シーケンス ──────────────────
  Serial.println(F("\n========== SIM7080G 初期化 =========="));

  // [STAGE 1] UART 設定
  Serial1.setPins(7, 6);  // RX=D7, TX=D6
  Serial1.begin(115200);
  simStage("STAGE1: UART 設定 (D6=TX, D7=RX, 115200bps)", true);

  // [STAGE 2] 起動待ち
  Serial.print(F("[   ] STAGE2: SIM7080G 起動待ち (15秒)"));
  for (int i = 0; i < 15; i++) {
    delay(1000); Serial.print('.');
    while (Serial1.available()) Serial.write(Serial1.read());
  }
  Serial.println(F(" 完了"));

  // [STAGE 3] AT 疎通確認
  bool atOk = false;
  Serial.print(F("[   ] STAGE3: AT 疎通確認"));
  for (int t = 0; t < 20; t++) {
    Serial.print('.');
    Serial1.print("AT\r\n"); delay(500);
    String r = "";
    unsigned long s = millis();
    while (millis() - s < 500) { while (Serial1.available()) r += (char)Serial1.read(); yield(); }
    if (r.indexOf("OK") >= 0) { atOk = true; break; }
    while (Serial1.available()) Serial1.read();
  }
  Serial.println();
  simStage("STAGE3: AT 疎通", atOk);
  if (!atOk) {
    Serial.println(F("  → 配線・電源を確認してください"));
    Serial.println(F("  → D6(TX)→SIM RX / D7(RX)←SIM TX / 5V 供給 OK?"));
    return;
  }

  // [STAGE 4] 工場出荷設定にリセット（デバッグ等で変更された NVM 設定を初期化）
  Serial.println(F("[   ] STAGE4: モデム設定をリセット中..."));
  sendAT("AT&F", 3000);        // 工場出荷設定に戻す
  delay(500);
  sendAT("AT+CFUN=1,1", 3000); // ソフトリセット（SIM 再初期化）
  delay(5000);                  // リセット完了待ち

  // リセット後の AT 疎通再確認
  bool atOk2 = false;
  for (int t = 0; t < 10; t++) {
    Serial1.print("AT\r\n"); delay(500);
    String r = "";
    unsigned long s = millis();
    while (millis() - s < 500) { while (Serial1.available()) r += (char)Serial1.read(); yield(); }
    if (r.indexOf("OK") >= 0) { atOk2 = true; break; }
    while (Serial1.available()) Serial1.read();
  }
  simStage("STAGE4: モデムリセット (AT&F + CFUN=1,1)", atOk2);
  if (!atOk2) { Serial.println(F("  → リセット後に応答なし")); return; }

  // [STAGE 5] エコーオフ・SIM 確認
  sendAT("ATE0", 2000);
  String cpinRes = sendAT("AT+CPIN?", 3000);
  bool simReady = cpinRes.indexOf("READY") >= 0;
  simStage("STAGE5: SIM カード認識 (CPIN=READY)", simReady);
  if (!simReady) {
    DLOGV("  → CPIN 応答: ", cpinRes);
    Serial.println(F("  → SIM カードが刺さっているか確認してください"));
  }

  // [STAGE 6] ネットワーク初期化
  Serial.println(F("[   ] STAGE6: ネットワーク初期化..."));
  bool netOk = initNetwork();
  simStage("STAGE6: ネットワーク接続", netOk);
  if (!netOk) {
    Serial.println(F("  → APN 設定・SIM 契約・電波状況を確認してください"));
    Serial.println(F("  → BLE スキャンは継続します（LTE-M なしで SD 保存のみ）"));
  }

  Serial.println(F("=====================================\n"));

#if TEST_MODE
  if (netOk) {
    Serial.println(F("===== テストモード: ダミーデータ送信（2台分バッチ） ====="));
    String ts = getTimestamp();
    // バッチ形式のダミーデータ（2台分）
    String params = "ts="; params += ts;
    params += "&sim="; params += SIM_NAME;
    params += "&n=2";
    params += "&m0=FF-FF-FF-FF-FF-01&p0=DEADBEEF&r0=-70";
    params += "&m1=FF-FF-FF-FF-FF-02&p1=CAFEBABE&r1=-65";
    bool testOk = postToGAS(params);
    simStage("TEST: GAS ダミー送信", testOk);
    if (testOk) Serial.println(F("  → スプレッドシートに2行追加されているか確認してください"));
    Serial.println(F("=======================================================\n"));
  }
#endif
}

// ══════════════════════════════════════════════
// loop
// ══════════════════════════════════════════════
static uint32_t lastSend = 0;

void loop() {
  uint32_t now = millis();

  if (now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;
    Serial.println(F("\n=== 定期送信 ==="));
    Serial.print(F("時刻: ")); Serial.println(getTimestamp());
    Serial.print(F("受信済み Flex 台数: ")); Serial.println(recordCount);
    flushRecords();
  }

  // 手動 AT コマンドモード（シリアルから入力）
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) sendAT(line);
  }
  while (Serial1.available()) Serial.write(Serial1.read());
}
