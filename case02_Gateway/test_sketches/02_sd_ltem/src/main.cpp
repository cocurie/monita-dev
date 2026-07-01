/**
 * Monita Gateway — テスト Step02: SD + LTE-M（SIM7080G / 1NCE）統合テスト
 *
 * MCU  : Seeed XIAO nRF52840
 * RTC  : DS3231（D4=SDA、D5=SCL）— SD ファイルのタイムスタンプに使用
 * SD   : D3=CS、D8=SCK、D9=MISO、D10=MOSI（標準SDライブラリ）
 * LTE-M: M5Stamp SIM7080G（D6=TX→SIM RX、D7=RX←SIM TX、5V供給必須）
 * SIM  : 1NCE IoT SIM（APN: iot.1nce.net）
 *
 * テスト内容:
 *   Step1: DS3231 初期化（失敗時はタイムスタンプを millis 基準に切り替え）
 *   Step2: SD 初期化（DS3231 コールバック登録でファイル日時を正確に記録）
 *   Step3: SIM7080G AT 疎通確認
 *   Step4: LTE-M ネットワーク登録（1NCE）
 *   Step5: GAS へダミーデータを HTTP GET 送信
 *   Step6: 送信結果を SD へ CSV 保存・読み返し
 *
 * 配線:
 *   XIAO D4 (SDA) → DS3231 SDA
 *   XIAO D5 (SCL) → DS3231 SCL
 *   XIAO 3V3      → DS3231 VCC
 *   XIAO D6 (TX)  → SIM7080G RX
 *   XIAO D7 (RX)  ← SIM7080G TX
 *   XIAO 5V       → SIM7080G 5V    ← 必須（3.3V では起動しない）
 *   XIAO GND      → SIM7080G GND / DS3231 GND
 *   SD CS         → XIAO D3
 *   SD CLK        → XIAO D8 (SCK)
 *   SD DAT0       → XIAO D9 (MISO)
 *   SD CMD        → XIAO D10 (MOSI)
 *   SD VDD        → XIAO 3V3
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <RTClib.h>
#include <SPI.h>
#include <SD.h>

// ══════════════════════════════════════════════
// ▼ ユーザー設定
// ══════════════════════════════════════════════

// GAS スクリプトID（gateway_v1.0 と同じエンドポイントを使用）
const char* GAS_SCRIPT_ID =
  "AKfycbw2IIQ1GyxtGh2Uis_zmwXW3VhftDy9HWKw5tSsUbwNOhNo6p9PnNv3ftfs3MvcMDT5ww/exec";

// 1NCE SIM 設定
const char* APN      = "iot.1nce.net";
const char* SIM_NAME = "1NCE";

// SD カード CS ピン
#define PIN_SD_CS D3

// SD ログファイル名
#define LOG_FILE "test_log.csv"

// ══════════════════════════════════════════════
// ▼ 内部状態
// ══════════════════════════════════════════════
static bool sdAvailable  = false;
static bool netAvailable = false;
static bool rtcAvailable = false;
static RTC_DS3231 rtc;

// ══════════════════════════════════════════════
// ▼ RTC タイムスタンプ
// ══════════════════════════════════════════════

// SDライブラリがファイル作成・更新時に呼ぶコールバック
// これを登録することでFATのファイル日時がRTCの実時刻になる
void sdDateTimeCallback(uint16_t* date, uint16_t* time) {
  DateTime now = rtcAvailable ? rtc.now() : DateTime(2000, 1, 1, 0, 0, 0);
  *date = FAT_DATE(now.year(), now.month(), now.day());
  *time = FAT_TIME(now.hour(), now.minute(), now.second());
}

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
// ▼ ユーティリティ
// ══════════════════════════════════════════════
static void printSeparator(const char* title) {
  Serial.println();
  Serial.print(F("=== "));
  Serial.print(title);
  Serial.println(F(" ==="));
}

// SD へ1行追記（タイムスタンプ付き）
static void sdAppend(const String& line) {
  if (!sdAvailable) return;
  File f = SD.open(LOG_FILE, FILE_WRITE);
  if (f) { f.println(getTimestamp() + "," + line); f.close(); }
}

// ══════════════════════════════════════════════
// ▼ AT コマンド送受信
// ══════════════════════════════════════════════
static String sendAT(const String& cmd, int waitMs = 5000) {
  Serial.print(F(">> ")); Serial.println(cmd);
  Serial1.print(cmd + "\r\n");
  uint32_t start = millis();
  String res = "";
  while (millis() - start < (uint32_t)waitMs) {
    while (Serial1.available()) res += (char)Serial1.read();
    yield();
  }
  if (res.length() > 0) { Serial.print(F("<< ")); Serial.println(res); }
  else                   { Serial.println(F("<< (応答なし)")); }
  return res;
}

// ══════════════════════════════════════════════
// ▼ Step1: DS3231 初期化
// ══════════════════════════════════════════════
static void stepRTCInit() {
  printSeparator("Step1: DS3231 初期化");

  if (!rtc.begin()) {
    Serial.println(F("[WARN] DS3231 が見つかりません（D4=SDA / D5=SCL を確認）"));
    Serial.println(F("  → タイムスタンプは millis 基準で続行"));
    return;
  }
  rtcAvailable = true;

  if (rtc.lostPower()) {
    Serial.println(F("[WARN] RTC が電源喪失 → ビルド時刻でセット"));
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  Serial.println(F("[OK] DS3231 初期化完了"));
  Serial.print(F("  現在時刻: ")); Serial.println(getTimestamp());
}

// ══════════════════════════════════════════════
// ▼ Step2: SD 初期化
// ══════════════════════════════════════════════
static bool stepSDInit() {
  printSeparator("Step2: SD 初期化");

  // RTCコールバックを登録（SD.begin より前に必須）
  SdFile::dateTimeCallback(sdDateTimeCallback);

  if (!SD.begin(PIN_SD_CS)) {
    Serial.println(F("[FAIL] SD.begin() failed"));
    Serial.println(F("  → カードが挿さっているか確認 / D3=CS D8=SCK D9=MISO D10=MOSI"));
    return false;
  }
  sdAvailable = true;
  Serial.println(F("[OK] SD 初期化完了"));

  // ヘッダ行がなければ書く
  if (!SD.exists(LOG_FILE)) {
    File f = SD.open(LOG_FILE, FILE_WRITE);
    if (f) { f.println(F("timestamp,step,result,detail")); f.close(); }
  }
  sdAppend("step2,OK,SD init");
  return true;
}

// ══════════════════════════════════════════════
// ▼ Step3: SIM7080G 起動・AT 疎通
// ══════════════════════════════════════════════
static bool stepModemInit() {
  printSeparator("Step3: SIM7080G 起動・AT 疎通");

  Serial1.setPins(7, 6);   // RX=D7, TX=D6
  Serial1.begin(115200);

  // 起動待ち
  Serial.print(F("  起動待ち (15秒)"));
  for (int i = 0; i < 15; i++) {
    delay(1000); Serial.print('.');
    while (Serial1.available()) Serial.write(Serial1.read());
  }
  Serial.println();

  // AT 疎通確認（最大20回リトライ）
  bool atOk = false;
  Serial.print(F("  AT 疎通確認"));
  for (int t = 0; t < 20; t++) {
    Serial.print('.');
    Serial1.print("AT\r\n");
    delay(500);
    String r = "";
    uint32_t s = millis();
    while (millis() - s < 500) { while (Serial1.available()) r += (char)Serial1.read(); yield(); }
    if (r.indexOf("OK") >= 0) { atOk = true; break; }
    while (Serial1.available()) Serial1.read();
  }
  Serial.println();

  if (!atOk) {
    Serial.println(F("[FAIL] AT 疎通失敗"));
    Serial.println(F("  → D6(TX)→SIM RX / D7(RX)←SIM TX の配線を確認"));
    Serial.println(F("  → SIM7080G に 5V が供給されているか確認"));
    sdAppend("step3,FAIL,AT no response");
    return false;
  }
  Serial.println(F("[OK] AT 疎通確認"));

  // エコーオフ・工場設定リセット
  sendAT("ATE0", 2000);
  sendAT("AT&F", 3000); delay(500);
  sendAT("AT+CFUN=1,1", 3000); delay(5000);

  // リセット後の再疎通
  bool atOk2 = false;
  for (int t = 0; t < 10; t++) {
    Serial1.print("AT\r\n"); delay(500);
    String r = "";
    uint32_t s = millis();
    while (millis() - s < 500) { while (Serial1.available()) r += (char)Serial1.read(); yield(); }
    if (r.indexOf("OK") >= 0) { atOk2 = true; break; }
    while (Serial1.available()) Serial1.read();
  }
  if (!atOk2) {
    Serial.println(F("[FAIL] リセット後の AT 疎通失敗"));
    sdAppend("step3,FAIL,AT lost after reset");
    return false;
  }

  // SIM 認識確認
  sendAT("ATE0", 2000);
  String cpin = sendAT("AT+CPIN?", 3000);
  if (cpin.indexOf("READY") < 0) {
    Serial.println(F("[FAIL] SIM カード未認識 (CPIN≠READY)"));
    Serial.println(F("  → SIM カードが正しく挿さっているか確認"));
    sdAppend("step3,FAIL,SIM not ready");
    return false;
  }

  Serial.println(F("[OK] SIM 認識完了"));
  sdAppend("step3,OK,modem+SIM ready");
  return true;
}

// ══════════════════════════════════════════════
// ▼ Step4: LTE-M ネットワーク登録（1NCE）
// ══════════════════════════════════════════════
static bool stepNetwork() {
  printSeparator("Step4: LTE-M ネットワーク登録 (1NCE)");

  // バンド全開放・LTE-M 設定・APN
  sendAT("AT+CREG=0", 2000); delay(200);
  sendAT("AT+CBANDCFG=\"CAT-M\",1,2,3,4,5,8,12,13,18,19,20,25,26,28,66,71,85", 3000); delay(500);
  sendAT("AT+CNMP=38", 2000); delay(500);   // LTE only
  sendAT("AT+CMNB=1",  2000); delay(500);   // Cat-M1
  sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\"", 2000); delay(500);
  Serial.println(F("[OK] LTE-M モード & APN 設定完了"));

  // CREG: ネットワーク登録待ち（最大60秒）
  bool cregOk = false;
  Serial.print(F("  ネットワーク登録待ち (最大60秒)"));
  for (int i = 0; i < 12; i++) {
    Serial.print('.');
    String reg = sendAT("AT+CREG?", 3000);
    int idx = reg.indexOf("+CREG: ");
    if (idx >= 0) {
      int sc = reg.indexOf(",", idx + 7);
      if (sc >= 0) {
        char stat = reg.charAt(sc + 1);
        if (stat == '1' || stat == '5') { cregOk = true; break; }
      }
    }
    if (i < 11) delay(5000);
  }
  Serial.println();

  if (!cregOk) {
    // 電波診断
    String csq = sendAT("AT+CSQ", 3000);
    String cpsi = sendAT("AT+CPSI?", 3000);
    Serial.println(F("[FAIL] ネットワーク登録失敗"));
    Serial.println(F("  → アンテナが接続されているか確認"));
    Serial.println(F("  → 1NCE ポータル(sim.1nce.net)で SIM が Active か確認"));
    sdAppend("step4,FAIL,CREG timeout");
    return false;
  }
  Serial.println(F("[OK] ネットワーク登録完了"));

  // CGATT: データ Attach 待ち（最大60秒）
  bool attachOk = false;
  Serial.print(F("  データ Attach 待ち (最大60秒)"));
  for (int i = 0; i < 12; i++) {
    Serial.print('.');
    delay(5000);
    String att = sendAT("AT+CGATT?", 3000);
    if (att.indexOf("+CGATT: 1") >= 0) { attachOk = true; break; }
  }
  // 手動アクティベート
  if (!attachOk) {
    Serial.print(F(" (CGACT 試行)"));
    sendAT("AT+CGACT=1,1", 10000); delay(3000);
    String att2 = sendAT("AT+CGATT?", 3000);
    if (att2.indexOf("+CGATT: 1") >= 0) attachOk = true;
  }
  Serial.println();

  if (!attachOk) {
    Serial.println(F("[FAIL] データ Attach 失敗（CGATT≠1）"));
    sdAppend("step4,FAIL,CGATT timeout");
    return false;
  }
  Serial.println(F("[OK] データ Attach 完了"));

  // CNACT: IP アドレス取得
  sendAT("AT+CNACT=0,1", 15000); delay(3000);
  String cnact = sendAT("AT+CNACT?", 3000);
  if (cnact.indexOf("0,1") < 0) {
    Serial.println(F("[FAIL] IP アドレス取得失敗（CNACT）"));
    sdAppend("step4,FAIL,CNACT no IP");
    return false;
  }
  Serial.println(F("[OK] IP アドレス取得完了"));
  sdAppend("step4,OK,network ready");
  netAvailable = true;
  return true;
}

// ══════════════════════════════════════════════
// ▼ Step5: GAS へダミーデータを HTTP GET 送信
// ══════════════════════════════════════════════
static bool stepSendDummy() {
  printSeparator("Step5: GAS ダミーデータ送信");

  if (!netAvailable) {
    Serial.println(F("[SKIP] ネットワーク未接続のためスキップ"));
    sdAppend("step5,SKIP,no network");
    return false;
  }

  // ダミーペイロード（テスト用: 1台分の固定データ）
  String params = "ts=TEST&sim=";
  params += SIM_NAME;
  params += "&n=1&m0=AA-BB-CC-DD-EE-FF&p0=DEADBEEF&r0=-70";

  String path = "/macros/s/";
  path += GAS_SCRIPT_ID;
  path += "?";
  path += params;

  Serial.println(F("  送信パラメータ:"));
  Serial.println("  " + params);

  // HTTPS 接続設定（gateway_v1.0 と同じシーケンス）
  sendAT("AT+SHDISC", 2000); delay(300);
  sendAT("AT+CSSLCFG=\"ignorertctime\",1,1"); delay(200);
  sendAT("AT+CSSLCFG=\"sslversion\",1,3");    delay(200);
  sendAT("AT+CSSLCFG=\"sni\",1,\"script.google.com\""); delay(200);
  sendAT("AT+SHSSL=1,\"\""); delay(200);
  sendAT("AT+SHCONF=\"BODYLEN\",1024");  delay(200);
  sendAT("AT+SHCONF=\"HEADERLEN\",350"); delay(200);
  sendAT("AT+SHCONF=\"URL\",\"https://script.google.com\""); delay(200);

  String conn = sendAT("AT+SHCONN", 15000);
  if (conn.indexOf("OK") < 0) {
    Serial.println(F("[FAIL] HTTPS 接続失敗"));
    sdAppend("step5,FAIL,SHCONN failed");
    return false;
  }

  String result = sendAT("AT+SHREQ=\"" + path + "\",1", 60000);

  // ステータスコード取得（GAS は 302 を返す）
  int statusCode = 0;
  int si = result.indexOf("+SHREQ: ");
  if (si >= 0) {
    String s = result.substring(si + 8);
    int c1 = s.indexOf(","), c2 = s.indexOf(",", c1 + 1);
    if (c1 >= 0 && c2 > c1) statusCode = s.substring(c1 + 1, c2).toInt();
  }
  Serial.print(F("  ステータスコード: ")); Serial.println(statusCode);

  sendAT("AT+SHDISC", 2000);

  // 200 または 302 を成功とみなす（GAS は書き込み後 302 を返す）
  bool ok = (statusCode == 200 || statusCode == 302);
  if (ok) {
    Serial.println(F("[OK] GAS 送信成功"));
    Serial.println(F("  → スプレッドシートに TEST 行が追加されているか確認してください"));
    sdAppend("step5,OK,status=" + String(statusCode));
  } else {
    Serial.print(F("[FAIL] 予期しないレスポンス: ")); Serial.println(statusCode);
    sdAppend("step5,FAIL,status=" + String(statusCode));
  }
  return ok;
}

// ══════════════════════════════════════════════
// ▼ Step6: SD ログ確認（書き込んだ内容を読み返し）
// ══════════════════════════════════════════════
static void stepReadLog() {
  printSeparator("Step6: SD ログ読み返し");

  if (!sdAvailable) {
    Serial.println(F("[SKIP] SD 未初期化"));
    return;
  }

  File f = SD.open(LOG_FILE, FILE_READ);
  if (!f) {
    Serial.println(F("[FAIL] ログファイルを開けません"));
    return;
  }
  Serial.println(F("--- " LOG_FILE " ---"));
  while (f.available()) {
    Serial.write(f.read());
  }
  f.close();
  Serial.println(F("--- ここまで ---"));
}

// ══════════════════════════════════════════════
// ▼ setup / loop
// ══════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  delay(500);

  Serial.println(F("============================================="));
  Serial.println(F("  Gateway Step02: SD + LTE-M 統合テスト"));
  Serial.print(F("  SIM: ")); Serial.println(SIM_NAME);
  Serial.print(F("  APN: ")); Serial.println(APN);
  Serial.println(F("============================================="));

  stepRTCInit();    // 失敗しても後続は続ける（millis 基準で続行）
  stepSDInit();     // 失敗しても後続は続ける（SD なしで続行）

  if (!stepModemInit()) {
    Serial.println(F("\n[ABORT] Step3 失敗 — モデム/SIM を確認してください"));
    stepReadLog();
    return;
  }

  if (!stepNetwork()) {
    Serial.println(F("\n[ABORT] Step4 失敗 — ネットワーク接続できませんでした"));
    stepReadLog();
    return;
  }

  stepSendDummy();  // 失敗しても SD ログは確認する
  stepReadLog();

  Serial.println();
  Serial.println(F("============================================="));
  Serial.println(F("  テスト完了"));
  Serial.println(F("============================================="));
}

void loop() {
  // シリアルから手動 AT コマンドを送れるようにしておく
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) sendAT(line);
  }
  while (Serial1.available()) Serial.write(Serial1.read());
}
