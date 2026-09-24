/**
 * project13_ipec/firmware_gateway — アイペック案件 Gateway ファーム（CAOPEN統一版）
 *
 * 【このファームの位置づけ】
 *   gateway_v1.21 が AT+HTTPTOFS（GET専用）で行っている通信を、すべて
 *   AT+CAOPEN（生TCP+SSL）へ置き換えた版。**case02_Gateway 配下は一切変更しない。**
 *
 * 【なぜ CAOPEN に統一したか】
 *   アイペック案件では Gateway が2種類の相手と通信する必要がある。
 *     ・iPEC サーバー … JSON をボディで POST。AT+SHBOD は JSON内のダブルクォートを
 *                       扱えないため CAOPEN で手書きするしかない
 *     ・GAS / Drive  … 応答に Content-Length が無く、AT+HTTPTOFS でないと読めなかった
 *   この2つを混在させると壊れる懸念があった（v1.21 に「AT+HTTPTOFS の後に
 *   AT+SHCONN が ERROR になる」実機記録あり）。
 *
 *   2026-09-22 の実機検証で、**CAOPEN 一本で GET も POST も、ホストを切り替えながら
 *   通ることを確認した**（Drive GET 200 / InfluxDB POST 204 / GAS POST）。
 *   混在させる必要が無くなったので、HTTPTOFS を捨てた。
 *
 * 【CAOPEN 統一で得られたもの】
 *   ・URL 512バイト制限からの解放（POST でボディに載せられる）
 *   ・16進blobの切り分けが不要（JSONをそのまま送れる）
 *   ・GET時代に捨てていた項目を復活（計測日時・BATT・FW・LoRa RSSI）
 *
 * 【対象ハード】Gateway ver1.1 / 1.20 / 1.21 基板（回路・ピン割当は同一）
 *   LTE-M (SIM7080G) : D6=TX / D7=RX（Serial1）
 *   LoRa  (E220)     : D0=RX / D1=TX / D2=M0M1（UARTE1）
 *   RTC   (DS3231)   : D4=SDA / D5=SCL
 *
 * 【まだ実装していないこと】
 *   ・SDカードへのバックアップ記録
 *   ・段階的復旧（モデムソフトリセット）
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <RTClib.h>
#include <nrf.h>
#include "utility/debug.h"   // dbgHeapUsed / dbgHeapTotal（長時間試験のメモリ監視）
#include <InternalFileSystem.h>   // ★FW17: GAS から変更した送信間隔を再起動後も保持する
using namespace Adafruit_LittleFS_Namespace;

// ★2026-09-23 FW15: 案件設定と認証情報を分離する。
#include "project_config.h"
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef INFLUX_TOKEN
#define INFLUX_TOKEN "REPLACE_WITH_YOUR_INFLUXDB_WRITE_TOKEN"
#endif

#ifndef IPEC_API_KEY
#define IPEC_API_KEY "REPLACE_WITH_IPEC_API_KEY"
#endif

// ══════════════════════════════════════════════
// ファームウェアバージョン
// ══════════════════════════════════════════════
// ★main.cpp を変更して commit するたびに +1 する（CLAUDE.md §6）。
//   コメント・ログ文言のみの修正は +1 不要。送信間隔・WDT・ペイロード形式など
//   実機の挙動に影響する変更は必ず +1。
//   本ファームは case02_Gateway とは別系統の連番（1から開始）。
// ★2026-09-24 FW24: Gateway コマンドを ack 成功後に実行。停止状態を flash に保存し、停止中も予約確認を続ける。
//   ステータス報告と RAM の診断ログ（直近24件）を GAS へ送れるようにした。
// ★2026-09-24 FW23: 起動時に機器情報（XIAO-ID・IMEI・ICCID・CSQ・送信間隔・リセット要因など）を GAS へ送り、
//   スプレッドシートの「Gateway起動ログ」シートに1行残す（本番 v1.21 の info 行と同じ考え方）。
// ★2026-09-24 FW22: 起動時に LTE-M へつながらなかったら、モジュールの返事（SIM・電波・登録状態）を表示し、
//   SIM7080G を再起動してからマイコンも再起動してやり直す（従来はアプリ層WDTが切れるまで LTE なしのままだった）。
// ★2026-09-24 FW21: GAS の転送先と結果本文を確認し、再送上限を96件へ拡大。空のPOSTを抑止する。
//   冗長フレームは独立した履歴で判定し、設定は置換renameで保存。アプリWDTに間隔＋10分の下限を設ける。
// ★2026-09-24 FW20: GAS の結果本文（ok:件数）を読んで成否を判定する。失敗した分は GAS・顧客送信先
//   それぞれに残し、次のサイクルで送り直す（GAS 側は子機ID＋受信時刻で重複を捨てる）。
// ★2026-09-23 FW19: InfluxDB・iPEC へ送る時刻を UTC に直す（RTC は日本時間。FW18 は9時間未来だった）。
// ★2026-09-23 FW18: 子機ごとに最大 RECORDS_PER_DEVICE 件ためて送る（上書きで計測が抜けないように）。
//   InfluxDB は1サイクル1回の POST にまとめ、各点に Gateway の受信時刻を付ける。
//   seq は「送信を試みたサイクル」だけで数える（空のサイクルで欠番にならないように）。
// ★2026-09-23 FW17: GAS（スプレッドシート）から Gateway の送信間隔を変更できるようにした。
//   想定外の HTTP 応答の先頭をログに出す（InfluxDB status=2 の切り分け用）。
// ★2026-09-23 FW15: 雛形化・Flex ダウンリンク・網時刻補正を追加。
// ★2026-09-23 FW14: 通信受信とLoRa並行処理の実機対策を反映。
static const uint8_t GATEWAY_FW_VERSION = 24;

// ★2026-09-23 FW14: 通信待機中もLoRa受信を処理するため前方宣言。
static void loraPoll();
static void logEvent(const String& msg);
static void flushRecords();

// ★2026-09-23 FW15: 案件設定は project_config.h に集約。
// ハードWDT。loop() から常時給餌するため、送信間隔とは無関係に短く取る。
static uint32_t const WDT_TIMEOUT_MS = 120000UL;   // 120秒

// アプリ層WDT。「一定時間クラウド送信が1回も成功しなかったら強制再起動」。
// ★ハードWDTでは捕捉できない「モデム接続は維持されたまま送信だけ失敗し続ける
//   ソフトハング」への対策（有野川現場 2026-07-17 の教訓）。
// ★2026-09-23 FW14: 2.5倍で1回の全失敗を許容し、次サイクルの送信処理
//   （最大1分程度）の完了まで待つ。検証間隔3分でも再試行後に1.5分の余裕を取る。
// ★2026-09-24 FW21: 短い送信間隔でも、通信タイムアウトが重なった1サイクルの途中で再起動しない。
static uint32_t computeAppWdtMs(uint32_t intervalMs) {
  return max(intervalMs * 2 + intervalMs / 2, intervalMs + 10UL * 60000UL);
}

// ★2026-09-23 FW15: GAS / Drive のサービス共通ホスト。
static const char* GAS_HOST = "script.google.com";
static const char* DRIVE_HOST = "drive.usercontent.google.com";

// ══════════════════════════════════════════════
static int const LORA_RX_PIN   = 0;   // D0: E220 TXD → XIAO RX
static int const LORA_TX_PIN   = 1;   // D1: XIAO TX → E220 RXD
static int const LORA_M0M1_PIN = 2;   // D2: E220 M0・M1 共通駆動

// ★UARTE1 を自前で使う場合、割り込みハンドラを手動で転送しないと送受信の完了通知が
//   届かず、write() が2バイト目以降で永久にブロックする（gateway_v1.1 で実機確認済みの罠）。
static Uart loraSerial(NRF_UARTE1, UARTE1_IRQn, LORA_RX_PIN, LORA_TX_PIN);
extern "C" void UARTE1_IRQHandler(void) { loraSerial.IrqHandler(); }

static int const CA_CID = 0;          // 生TCPソケットの接続ID

// ══════════════════════════════════════════════
// LoRa 設定（子機 v3.10/v3.20 と同一値。全台共通）
// ══════════════════════════════════════════════
#define LORA_MODE_SWITCH_DELAY_MS 100U
#define LORA_CFG_REG_START 0x00
#define LORA_CFG_REG_LEN   6
// ★2026-09-23 FW15(0x06): 可変長ペイロード 0x06（最大 8+2×12=32 バイト）を受けられるよう 24→32 に拡大。
#define MAX_PAYLOAD        32
// 子機ペイロードのチャネル上限（0x06 の n の上限）。スプレッドシートの CH 列（CH1〜CH15）以内。
#define LORA_MAX_CH        12
#define LORA_PKT_TYPE_FIXED 0x04   // 従来: 19バイト固定・4ch（v3.10 / v3.20 / firmware_child）
#define LORA_PKT_TYPE_VAR   0x06   // 新: 可変長・nch・欠測/時刻無効を表現できる（03_template/child）
#define LORA_CH_MISSING     ((int16_t)0x8000)   // 0x06 の欠測値
#define LORA_TIME_INVALID   0xFF               // 0x06 の時刻無効
// ★2026-09-23 FW15(0x06): 0x04 は時刻の無効値を持たないため、子機は RTC 失敗時に 0:0 を送ってくる。
//   1 なら 0x04 の 0:0 を「無効」とみなす（0x06 には適用しない。0x06 の 0:0 は本物の0時0分）。
#ifndef TREAT_ZERO_TIME_AS_INVALID
#define TREAT_ZERO_TIME_AS_INVALID 1
#endif
static const uint8_t LORA_CFG_ADDH = 0x00;
static const uint8_t LORA_CFG_ADDL = 0x00;
static const uint8_t LORA_CFG_REG0 = 0x68;  // UART9600bps + エア速度(SF7/BW125kHz)
static const uint8_t LORA_CFG_REG1 = 0x01;  // 送信出力13dBm
static const uint8_t LORA_CFG_REG2 = 0x00;  // チャンネル0
static const uint8_t LORA_CFG_REG3 = 0x80;  // RSSIバイト有効化ON/透過送信モード

// ══════════════════════════════════════════════
// 受信レコード
// ══════════════════════════════════════════════
// ★子機は LORA_TX_REPEAT=2 で**同一フレームを2回送る**（電波障害での取りこぼし対策）。
//   電波が良ければ両方届くので、受信側で重複排除しないとクラウドに二重記録される。
//   ★2026-09-24 FW21: DeviceID ごとの直近フレームを送信バッファとは別に保持して判定する。
//
// ★2026-09-23 FW18: 「1台1件」だと、Gateway の1サイクルに同じ子機の計測が2回入ったとき
//   前の計測が上書きで消える（子機は送信ごとに±10秒ずらすので、同じ間隔でも時々起きる）。
//   そこで子機ごとに最大 RECORDS_PER_DEVICE 件ためる方式に変えた。2回送りの冗長フレームは
//   ★FW21: 「同じ子機から DUP_WINDOW_MS 以内に来た同じ内容」を破棄する。
//   ためられる件数を超えたら、その子機のいちばん古い1件を捨てて s_recordsDropped に数える。
#define MAX_DEVICES 32
#define RECORDS_PER_DEVICE 6        // ★GAS の GW_RECORDS_PER_CHILD と同じ値にする
#define MAX_RECORDS 96              // 全体の上限（1件あたり約50バイト）
#define DUP_WINDOW_MS 10000UL       // 子機の冗長送信（2回目は約0.3秒後）をまとめる時間幅

struct FlexRecord {
  bool     used;
  uint8_t  deviceId;
  uint8_t  fwVersion;
  // ★2026-09-23 FW15(0x06): チャネル数を子機ごとに持つ。欠測は LORA_CH_MISSING。
  uint8_t  nCh;
  int16_t  ch[LORA_MAX_CH];
  uint16_t battMv;
  uint8_t  hour, minute;
  bool     timeValid;   // ★2026-09-23 FW15(0x06): false なら計測日時を空欄にする
  int      rssiDbm;
  uint32_t rtcEpoch;    // Gateway の DS3231 で付けた受信時刻
  uint32_t rxMs;        // ★FW18: 受信時の millis()（冗長フレームの判定用）
  uint32_t seq;         // ★FW20: 最初に送ったサイクルの seq（再送しても変えない）
};
// ★FW18: 受信順に詰めて並べる（s_records[0] がいちばん古い）
static FlexRecord s_records[MAX_RECORDS];
// ★2026-09-24 FW21: flushRecords() の直後に届く2回目も、同じ計測として捨てる。
struct LastFrame { bool used; uint8_t deviceId; uint32_t rxMs; uint32_t hash; };
static LastFrame s_lastFrame[MAX_DEVICES];
static int        s_recordCount = 0;
static uint32_t   s_recordsDropped = 0;   // あふれて捨てた件数（0 以外なら Gateway の間隔が長すぎる）

// ★2026-09-24 FW20: 送信に失敗した分の再送待ち（送信先ごと）。
//   2026-09-23 の長期試験で、GAS が本文の壊れた POST を受けて `error` を返したのに 302 だったため
//   成功と数え、1サイクル分がシートから消えた。InfluxDB も2サイクル分を失った（再送なし）。
//   ・InfluxDB は同じ点（同じタグ・同じ時刻）を送り直しても上書きになるだけなので安全
//   ・GAS は子機ID＋Gateway受信時刻（rx）で重複を捨てる（Code.gs の doPost）ので安全
//   上限を超えたら古いものから捨て、s_retryDropped に数える。
#define RETRY_MAX 96   // ★2026-09-24 FW21: 新規の最大件数（MAX_RECORDS）を1回は再送できるようにする
struct RetryQueue {
  FlexRecord recs[RETRY_MAX];
  int n = 0;
  uint32_t dropped = 0;
};
static RetryQueue s_gasRetry, s_custRetry;

// 送信に失敗した分を再送待ちへ戻す（古い順を保つ。あふれたら古いものから捨てる）
static void retryPush(RetryQueue& q, const FlexRecord* recs, int n, const __FlashStringHelper* name) {
  uint32_t before = q.dropped;
  for (int i = 0; i < n; i++) {
    if (q.n >= RETRY_MAX) {
      for (int k = 0; k + 1 < q.n; k++) q.recs[k] = q.recs[k + 1];
      q.n--;
      q.dropped++;
    }
    q.recs[q.n++] = recs[i];
  }
  if (q.dropped != before) logEvent(String(name) + " 再送あふれ: " + String(q.dropped - before) + "件破棄");
  Serial.print(F("  [RETRY] ")); Serial.print(name); Serial.print(F(": ")); Serial.print(n);
  Serial.print(F(" 件を次のサイクルで再送（再送待ち ")); Serial.print(q.n);
  Serial.print(F(" 件、あふれ累計 ")); Serial.print(q.dropped); Serial.println(F(" 件）"));
}

// 再送待ち＋今回分を1つの配列にまとめる（再送待ちは空にする。失敗したら呼び出し側で retryPush）
static int retryMerge(RetryQueue& q, const FlexRecord* fresh, int n, FlexRecord* out) {
  int m = 0;
  for (int i = 0; i < q.n; i++) out[m++] = q.recs[i];
  for (int i = 0; i < n; i++) out[m++] = fresh[i];
  if (q.n) { Serial.print(F("  [RETRY] 前回までの未送信 ")); Serial.print(q.n); Serial.println(F(" 件を含めて送ります")); }
  q.n = 0;
  return m;
}

// ══════════════════════════════════════════════
// 状態
// ══════════════════════════════════════════════
static RTC_DS3231 s_rtc;
static bool       s_rtcAvailable = false;

static uint32_t s_lastSendMs = 0;
// ★2026-09-23 FW17: 送信間隔は実行時の値を使う。project_config.h の SEND_INTERVAL_MS は初期値。
//   GAS から interval:N コマンドで変更でき、flash に保存して再起動後も保持する。
static uint32_t s_sendIntervalMs = SEND_INTERVAL_MS;
static bool s_sendPaused = false;   // ★2026-09-24 FW24: 停止中も LoRa 受信と予約確認は続ける
static uint32_t s_lastCloudSuccessMs = 0;   // アプリ層WDTの起点
static int      s_lastCsq = 0;
static uint32_t s_cycle = 0;
static uint32_t s_sendSeq = 0;   // ★FW18: 送信を試みたサイクルの通し番号（InfluxDB の seq）

// 経路ごとの成否カウンタ（どちらが壊れているか切り分けられるようにする）
// ★起動ごとに変わる識別子。InfluxDB 側で「どの起動の何サイクル目か」を復元するために使う。
//   これが無いと、再起動をまたいだときに seq が振り出しに戻り、欠番を数えられなくなる。
static uint32_t s_runId = 0;

static uint32_t s_gasOk = 0, s_gasNg = 0, s_gasConsecNg = 0;
static uint32_t s_cloudOk = 0, s_cloudNg = 0, s_cloudConsecNg = 0;
static uint32_t s_dlOk = 0, s_dlNg = 0;

// ★2026-09-24 FW24: SD の代わりに直近の診断ログを RAM に残す。再起動すると消える。
static const unsigned EVENT_LOG_MAX = 24;
static char s_eventLog[EVENT_LOG_MAX][97];
static unsigned s_eventLogNext = 0, s_eventLogCount = 0;
static void logEvent(const String& msg) {
  String line = String(millis() / 60000UL) + "分 ";
  if (s_rtcAvailable && !s_rtc.lostPower()) {
    DateTime now = s_rtc.now();
    if (now.isValid() && now.year() >= 2026 && now.year() <= 2035) {
      char tb[24];
      snprintf(tb, sizeof(tb), "%02u-%02u %02u:%02u ", now.month(), now.day(), now.hour(), now.minute());
      line += tb;
    }
  }
  line += msg;
  // UTF-8 の途中で切ると GAS の JSON が壊れるので、文字の先頭まで戻す（最大96バイト）。
  unsigned len = min((unsigned)line.length(), 96U);
  if (len < line.length()) while (len && ((uint8_t)line[len] & 0xC0) == 0x80) len--;
  memcpy(s_eventLog[s_eventLogNext], line.c_str(), len);
  s_eventLog[s_eventLogNext][len] = '\0';
  s_eventLogNext = (s_eventLogNext + 1) % EVENT_LOG_MAX;
  if (s_eventLogCount < EVENT_LOG_MAX) s_eventLogCount++;
}


// ══════════════════════════════════════════════
// WDT
// ══════════════════════════════════════════════
static void wdtInit(uint32_t timeoutMs) {
  NRF_WDT->CONFIG = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);
  NRF_WDT->CRV    = (uint32_t)((uint64_t)timeoutMs * 32768ULL / 1000ULL);
  NRF_WDT->RREN   = WDT_RREN_RR0_Msk;
  NRF_WDT->TASKS_START = 1;
}
static inline void wdtFeed() { NRF_WDT->RR[0] = WDT_RR_RR_Reload; }

// ★2026-09-23 FW14: 待機中のLoRaリング溢れを防ぎ、ハードWDTも給餌する。
static void waitWithLora(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    loraPoll();
    wdtFeed();
    yield();
  }
}

// 一定時間クラウド送信が成功しなければ強制再起動する。
static void appWatchdogCheck() {
  if (millis() - s_lastCloudSuccessMs >= computeAppWdtMs(s_sendIntervalMs)) {
    Serial.println(F("\n[APP-WDT] 規定時間クラウド送信が成功していません。再起動します"));
    Serial.flush();
    delay(100);
    NVIC_SystemReset();
  }
}

// ══════════════════════════════════════════════
// AT コマンド
// ══════════════════════════════════════════════
// ★2026-09-23 FW15: HTTP 全区間と入れ子の sendAT を保護し、SIM の64B受信リング溢れを防ぐ。
static bool s_atBusy = false;
struct AtBusyGuard {
  bool previous;
  AtBusyGuard() : previous(s_atBusy) { s_atBusy = true; }
  ~AtBusyGuard() { s_atBusy = previous; }
};

static String sendAT(const String& cmd, int waitMs = 5000, const char* waitToken = nullptr) {
  AtBusyGuard busy; // ★2026-09-23 FW15: return 時も元の通信状態へ戻す。
  Serial.print(F(">> ")); Serial.println(cmd);
  Serial1.print(cmd + "\r\n");
  uint32_t t0 = millis();
  String res = "";
  while (millis() - t0 < (uint32_t)waitMs) {
    // ★2026-09-23 FW14: 通信待ちでもLoRa受信とWDT給餌を継続。
    loraPoll(); wdtFeed();
    while (Serial1.available()) res += (char)Serial1.read();
    if (waitToken != nullptr && res.indexOf(waitToken) >= 0) {
      delay(20);
      while (Serial1.available()) res += (char)Serial1.read();
      break;
    }
    if (waitToken == nullptr &&
        (res.indexOf("\r\nOK\r\n") >= 0 || res.indexOf("\r\nERROR\r\n") >= 0)) {
      delay(20);
      while (Serial1.available()) res += (char)Serial1.read();
      break;
    }
    wdtFeed();
    yield();
  }
  return res;
}

// ★2026-09-23 FW15: 電池切れの RTC を網時刻（JSTとして扱う）で補正し、24時間ごとに再確認。
static uint32_t s_lastRtcSyncMs = 0;
static bool syncRtcFromNetworkTime() {
  s_lastRtcSyncMs = millis();
  if (!s_rtcAvailable) { Serial.println(F("[RTC] 補正不可: DS3231なし")); return false; }
  String res = sendAT("AT+CCLK?", 3000);
  int idx = res.indexOf("+CCLK: ");
  if (idx >= 0) {
    idx = res.indexOf('"', idx) + 1;
    if (idx == 0) { Serial.println(F("[RTC] 網時刻の形式不正")); return false; }
    String stamp = res.substring(idx, idx + 17);
    bool valid = stamp.length() == 17;
    for (int i = 0; valid && i < 17; i++) {
      if (i == 2 || i == 5) valid = stamp[i] == '/';
      else if (i == 8) valid = stamp[i] == ',';
      else if (i == 11 || i == 14) valid = stamp[i] == ':';
      else valid = stamp[i] >= '0' && stamp[i] <= '9';
    }
    int yy = stamp.substring(0, 2).toInt(), mon = stamp.substring(3, 5).toInt();
    int day = stamp.substring(6, 8).toInt(), hh = stamp.substring(9, 11).toInt();
    int mi = stamp.substring(12, 14).toInt(), ss = stamp.substring(15, 17).toInt();
    if (valid && yy >= 26 && yy <= 35 && mon >= 1 && mon <= 12 &&
        day >= 1 && day <= 31 && hh < 24 && mi < 60 && ss < 60) {
      DateTime now(2000 + yy, mon, day, hh, mi, ss);
      if (now.isValid()) {
        s_rtc.adjust(now);
        Serial.print(F("[RTC] 網時刻で補正成功（JST）: ")); Serial.println(stamp);
        return true;
      }
    }
  }
  Serial.println(F("[RTC] 網時刻の取得・補正失敗（RTCは変更せず）"));
  return false;
}

static bool probeAT() {
  Serial.print(F("AT 疎通確認"));
  for (int t = 0; t < 20; t++) {
    Serial.print('.');
    Serial1.print("AT\r\n");
    String r = "";
    uint32_t s = millis();
    while (millis() - s < 500) {
      while (Serial1.available()) r += (char)Serial1.read();
      wdtFeed(); yield();
    }
    if (r.indexOf("OK") >= 0) {
      Serial.println(F(" OK"));
      while (Serial1.available()) Serial1.read();
      return true;
    }
    while (Serial1.available()) Serial1.read();
    delay(500);
  }
  Serial.println(F(" 失敗"));
  return false;
}

static bool lteConnect() {
  sendAT("ATE0", 2000);
  sendAT("AT+CPIN?", 3000);
  sendAT("AT+CNMP=38", 2000);
  sendAT("AT+CMNB=1", 2000);
  sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\"", 2000);

  Serial.println(F("ネットワーク登録待ち（最大60秒）"));
  bool registered = false;
  for (int i = 0; i < 12; i++) {
    String r = sendAT("AT+CREG?", 3000);
    if (r.indexOf("0,1") >= 0 || r.indexOf("0,5") >= 0) { registered = true; break; }
    if (sendAT("AT+CGATT?", 2000).indexOf("+CGATT: 1") >= 0) { registered = true; break; }
    waitWithLora(5000);   // ★FW22: 待ち時間も LoRa 受信と WDT 給餌を続ける
  }
  if (!registered) { Serial.println(F("✗ ネットワーク登録失敗")); return false; }

  sendAT("AT+CNACT=0,1", 15000);   // 既にアクティブなら ERROR が返るが正常
  delay(3000);
  if (sendAT("AT+CNACT?", 3000).indexOf("0,1") < 0) {
    Serial.println(F("✗ IP 取得失敗"));
    return false;
  }
  sendAT("AT+COPS?", 3000);
  Serial.println(F("✓ LTE-M 接続完了"));
  return true;
}

static int readCsq() {
  String r = sendAT("AT+CSQ", 3000);
  int i = r.indexOf("+CSQ: ");
  if (i < 0) return 99;
  return r.substring(i + 6, r.indexOf(",", i)).toInt();
}

// ══════════════════════════════════════════════
// CAOPEN（生TCP + SSL）
// ══════════════════════════════════════════════
static void caClose() { sendAT("AT+CACLOSE=" + String(CA_CID), 3000); }

// ★SNI は必ず設定する。script.google.com / drive.usercontent.google.com は
//   共有エッジのため、SNI が無いと正しい証明書・サービスに振り分けられない。
//   （AWS の execute-api は SNI 無しでも通っていたが、統一して常に設定する）
static bool caOpen(const char* host, int port) {
  // ★2026-09-23 FW14: SSL設定間の待機でもLoRa受信を止めない。
  caClose();
  waitWithLora(300);
  sendAT("AT+CSSLCFG=\"sslversion\"," + String(CA_CID) + ",3", 2000);
  waitWithLora(150);
  sendAT("AT+CSSLCFG=\"ignorertctime\"," + String(CA_CID) + ",1", 2000);
  waitWithLora(150);
  sendAT("AT+CSSLCFG=\"sni\"," + String(CA_CID) + ",\"" + String(host) + "\"", 2000);
  waitWithLora(150);
  sendAT("AT+CASSLCFG=" + String(CA_CID) + ",\"ssl\",1", 2000);
  waitWithLora(150);

  String res = sendAT("AT+CAOPEN=" + String(CA_CID) + ",0,\"TCP\",\"" + String(host) +
                      "\"," + String(port), 30000, "+CAOPEN:");
  int ri = res.indexOf("+CAOPEN: ");
  if (ri >= 0) {
    int comma = res.indexOf(',', ri);
    if (comma > 0 && res.substring(comma + 1, comma + 4).toInt() == 0) return true;
  }
  Serial.println(F("  ✗ CAOPEN 失敗"));
  return false;
}

// ★送信確認のトークンを "SEND OK" 固定にしてはいけない。
//   【実機で確定】SIM7080G は AT+CASEND の本文送信後、"SEND OK" ではなく
//   "\r\nOK\r\n" だけを返す（2026-09-22、Gateway ver1.21 基板で確認）。
//   送信の成否は最終的な HTTP ステータスで判断する。
// ★モジュールが自発的に知らせてくる合図（URC）の到着時刻を記録する。
//   +CADATAIND: 0   … 受信バッファに相手からのデータが届いた
//   +CASTATE: 0,0   … 相手（またはネットワーク）が接続を切った
//   仮説「相手が返事を送ってすぐ切ると、モジュールが受信済みの返事ごと捨てる」を
//   確かめるには、この2つが「送信完了から何ms後に」「どちらが先に」来たかが必要。
static uint32_t s_reqSentMs   = 0;   // リクエストを送り終えた時刻
static uint32_t s_urcDataMs   = 0;   // +CADATAIND を最初に見た時刻（0=未着）
static uint32_t s_urcClosedMs = 0;   // +CASTATE: 0,0 を最初に見た時刻（0=未着）
static uint32_t s_firstByteMs = 0;   // AT+CARECV で最初に1バイト以上取れた時刻

static void noteUrc(const String& buf) {
  if (s_urcDataMs == 0 && buf.indexOf("+CADATAIND") >= 0) s_urcDataMs = millis();
  if (s_urcClosedMs == 0 && buf.indexOf("+CASTATE: 0,0") >= 0) s_urcClosedMs = millis();
}

// AT+CASEND の1回あたりの上限。SIMCom の AT Command Manual では <reqlength> の
// 最大は 1460 バイト（TCPのセグメントサイズ由来）。実装上の余裕を見て 1024 で刻む。
//
// ★この上限は「1回のコマンドで渡せる量」であって、送信できる総量ではない。
//   CAOPEN は生TCPソケットなので、CASEND を必要な回数繰り返せば
//   いくらでも送れる（TCPはバイトストリームなので受信側で連結される）。
//   AT+SHREQ の「URL 512バイト」のような、回避できない上限とは性質が違う。
static const uint16_t CASEND_CHUNK_BYTES = 1024;

// ★2026-09-23: 相手に先に切らせない（keep-alive）かどうか。
//   【背景】SIM7080G は、相手が接続を切ると、まだ AT+CARECV で取り出していない
//   受信データを捨てる（と考えられる）。InfluxDB は 204 を返してから約5msで切るため、
//   こちらが取りに行く前に返事が消え、「サーバーには書き込まれたのに失敗と判定する」
//   偽陰性になっていた（FW 12 で、切断後に0.3秒おきに問い合わせ直しても0バイトを確認）。
//   Connection: close を送らなければサーバーは接続を保つので、読み終えてから
//   こちら（AT+CACLOSE）が切る。
//   1=keep-alive（切らせない） / 0=従来どおり Connection: close
#ifndef HTTP_KEEP_ALIVE
#define HTTP_KEEP_ALIVE 1
#endif

// 1回分（CASEND_CHUNK_BYTES 以内）を送る。
static String visible(const String& in);   // 後方で定義（ログ表示用）

static bool caSendChunk(const String& data) {
  Serial1.print("AT+CASEND=" + String(CA_CID) + "," + String(data.length()) + "\r\n");

  bool gotPrompt = false;
  uint32_t s = millis();
  while (millis() - s < 5000) {
    // ★2026-09-23 FW14: 通信待ちでもLoRa受信とWDT給餌を継続。
    loraPoll(); wdtFeed();
    while (Serial1.available()) {
      if ((char)Serial1.read() == '>') { gotPrompt = true; break; }
    }
    if (gotPrompt) break;
    wdtFeed(); yield();
  }
  if (!gotPrompt) { Serial.println(F("  ✗ '>' プロンプト待ちタイムアウト")); return false; }

  delay(50);
  Serial1.print(data);

  String res = "";
  s = millis();
  while (millis() - s < 15000) {
    // ★2026-09-23 FW14: 通信待ちでもLoRa受信とWDT給餌を継続。
    loraPoll(); wdtFeed();
    while (Serial1.available()) res += (char)Serial1.read();
    noteUrc(res);
    if (res.indexOf("SEND OK") >= 0 || res.indexOf("\r\nOK\r\n") >= 0) return true;
    if (res.indexOf("ERROR") >= 0) {
      Serial.print(F("  ✗ CASEND が ERROR 生応答=[")); Serial.print(visible(res)); Serial.println(F("]"));
      return false;
    }
    wdtFeed(); yield();
  }
  return true;   // 確認トークンが来なくても受信へ進む（上記コメント参照）
}

// 任意長のデータを CASEND_CHUNK_BYTES ずつに分割して送る。
static bool caSend(const String& data) {
  const uint32_t total = data.length();
  if (total <= CASEND_CHUNK_BYTES) return caSendChunk(data);

  Serial.print(F("  [CASEND] ")); Serial.print(total);
  Serial.print(F("バイトを")); Serial.print((total + CASEND_CHUNK_BYTES - 1) / CASEND_CHUNK_BYTES);
  Serial.println(F("回に分割して送信"));

  for (uint32_t off = 0; off < total; off += CASEND_CHUNK_BYTES) {
    uint32_t len = total - off;
    if (len > CASEND_CHUNK_BYTES) len = CASEND_CHUNK_BYTES;
    if (!caSendChunk(data.substring(off, off + len))) {
      Serial.print(F("  ✗ CASEND 失敗（")); Serial.print(off / CASEND_CHUNK_BYTES + 1);
      Serial.println(F("個目のチャンク）"));
      return false;
    }
    wdtFeed();
  }
  return true;
}

// AT+CARECV を1回発行して受信データを取り出す。
// ★受信データ中に "OK" や改行が含まれ得るため、終端を文字列検索で探してはいけない。
//   "+CARECV: <n>," の <n> を読み、そのバイト数だけを本文として切り出す。
// 直近の caRecvOnce が -1 を返したときの診断情報（httpRequest がログに出す）。
static String s_recvDiag = "";

// 改行などを見える形にしてログに出すための変換
static String visible(const String& in) {
  String o = "";
  for (unsigned i = 0; i < in.length() && o.length() < 240; i++) {
    char c = in[i];
    if (c == '\r') o += "\\r";
    else if (c == '\n') o += "\\n";
    else if (c < 0x20 || c > 0x7E) o += '.';
    else o += c;
  }
  return o;
}

static int caRecvOnce(String& out) {
  out = "";

  // ★2026-09-23 修正（偽陰性の本当の原因）:
  //   【何が起きていたか】まだ返事が届いていない時点で AT+CARECV を送ると、モジュールは
  //   "+CARECV: 0" ではなく **"OK" だけ**を返すことがある。旧実装は "+CARECV:" の行が
  //   来るまで同じ呼び出しの中で待ち続けたため、直後に +CADATAIND（返事が届いた合図）が
  //   来ても次の AT+CARECV を送らず、10秒の時間切れで失敗と判定していた。
  //   返事はモジュール内に届いているので、サーバーには書き込まれている＝偽陰性になる。
  //   InfluxDB は返事が速い（約0.6秒）ため最初の問い合わせが空振りしやすく、失敗が集中した。
  //   （「相手が切ると返事が捨てられる」という仮説は、GAS で切断合図の82ms後に
  //     返事を読めていたことから否定された）
  //
  //   【対策】
  //   ・送る前に、溜まっている古い応答（前回の末尾の OK や合図）を読み捨てる
  //     （捨てる前に noteUrc で合図は記録する）。これをしないと古い OK を
  //     今回の「0バイト」と取り違える
  //   ・"OK" だけの応答は「今は0バイト」としてすぐ戻り、呼び出し側に問い合わせ直させる
  //   ・3秒なにも返らなければ、失敗ではなく0バイトとして戻る（全体の上限は呼び出し側が持つ）
  {
    String stale = "";
    while (Serial1.available()) stale += (char)Serial1.read();
    if (stale.length()) noteUrc(stale);
  }

  Serial1.print("AT+CARECV=" + String(CA_CID) + ",1024\r\n");
  String buf = "";
  int declared = -1, bodyStart = -1;
  uint32_t t0 = millis();
  while (millis() - t0 < 3000) {
    // ★2026-09-23 FW14: 通信待ちでもLoRa受信とWDT給餌を継続。
    loraPoll(); wdtFeed();
    while (Serial1.available()) buf += (char)Serial1.read();
    noteUrc(buf);
    if (declared < 0) {
      // ★2026-09-23 FW14: CARECVの本文は不透明なバイト列。本文中のERRORは判定しない。
      int i = buf.indexOf("+CARECV: ");
      if (i >= 0) {
        int j = i + 9, k = j;
        while (k < (int)buf.length() && isDigit(buf[k])) k++;
        if (k < (int)buf.length() && k > j) {
          declared = buf.substring(j, k).toInt();
          bodyStart = (buf[k] == ',') ? (k + 1) : k;
        }
      } else if (buf.indexOf("\r\nERROR") >= 0) {
        s_recvDiag = "モジュールがERRORを返した 生応答=[" + visible(buf) + "]";
        return -1;
      } else if (buf.indexOf("\r\nOK\r\n") >= 0) {
        s_recvDiag = "OKのみ 生応答=[" + visible(buf) + "]";   // ★FW18: 0バイト調査用
        return 0;   // "OK" だけ＝今は受信データなし
      }
    }
    if (declared >= 0 && bodyStart >= 0 && (int)buf.length() - bodyStart >= declared) {
      out = buf.substring(bodyStart, bodyStart + declared);
      // ★2026-09-23 FW14: データ部より後ろの末尾OKを最大300ms回収し、次回の空応答誤認を防ぐ。
      String tail = buf.substring(bodyStart + declared);
      uint32_t tailStart = millis();
      noteUrc(tail);
      while (tail.indexOf("\r\nOK\r\n") < 0 && millis() - tailStart < 300UL) {
        loraPoll(); wdtFeed();
        while (Serial1.available()) tail += (char)Serial1.read();
        noteUrc(tail);
        yield();
      }
      return declared;
    }
    wdtFeed(); yield();
  }
  // 3秒なにも返らない。失敗にはせず、呼び出し側で問い合わせ直す。
  s_recvDiag = "3秒間応答なし 生応答=[" + visible(buf) + "]";
  return 0;
}

// ══════════════════════════════════════════════
// HTTP 層（自前で組み立て・自前で解釈する）
// ══════════════════════════════════════════════
struct HttpResult {
  int    status = 0;
  String body = "";
  int    contentLength = -1;
  String location = "";     // 3xx の Location ヘッダー（リダイレクト先の判定に使う）
  bool   chunked = false;
  // ★応答を最後まで受け取れたか。
  //   ステータス行だけ届いて途中で切れても status には 200 が入るため、
  //   status を見るだけでは「切れているのに成功の顔をした応答」を弾けない
  //   （実機で status=200・本文0バイト・Content-Length無し として現れた）。
  bool   headersComplete = false;
  bool   complete = false;
  int    rawBytes = 0;      // 実際に受け取った総バイト数（切り分け用）
  uint32_t elapsedMs = 0;
};

static String decodeChunked(const String& raw) {
  String out = "";
  int pos = 0;
  while (pos < (int)raw.length()) {
    int nl = raw.indexOf("\r\n", pos);
    if (nl < 0) break;
    String sizeLine = raw.substring(pos, nl);
    sizeLine.trim();
    int semi = sizeLine.indexOf(';');
    if (semi >= 0) sizeLine = sizeLine.substring(0, semi);
    long sz = strtol(sizeLine.c_str(), nullptr, 16);
    if (sz <= 0) break;
    int dataStart = nl + 2;
    if (dataStart + sz > (int)raw.length()) { out += raw.substring(dataStart); break; }
    out += raw.substring(dataStart, dataStart + sz);
    pos = dataStart + sz + 2;
  }
  return out;
}

static void parseHttpResponse(const String& raw, HttpResult& r) {
  int hs = raw.indexOf("HTTP/1.");
  if (hs < 0) return;
  r.status = raw.substring(hs + 9, hs + 12).toInt();
  r.rawBytes = raw.length();
  int headerEnd = raw.indexOf("\r\n\r\n", hs);
  if (headerEnd < 0) return;   // ヘッダーが途中で切れている → headersComplete は false のまま
  r.headersComplete = true;
  String headers = raw.substring(hs, headerEnd);
  String lower = headers; lower.toLowerCase();

  int ci = lower.indexOf("content-length:");
  if (ci >= 0) {
    int le = headers.indexOf("\r\n", ci);
    r.contentLength = headers.substring(ci + 15, le < 0 ? headers.length() : le).toInt();
  }
  if (lower.indexOf("transfer-encoding:") >= 0 && lower.indexOf("chunked") >= 0) r.chunked = true;

  // ★Location を拾う理由: GAS Web App は成功時も 302 を返すため、ステータスだけでは
  //   「正常に実行されて結果ページへ誘導されている」のか「未ログインで認証画面へ
  //   飛ばされている」のか区別できない。行き先ホストを見れば確実に分かる。
  int li = lower.indexOf("location:");
  if (li >= 0) {
    int le = headers.indexOf("\r\n", li);
    r.location = headers.substring(li + 9, le < 0 ? headers.length() : le);
    r.location.trim();
  }

  String rawBody = raw.substring(headerEnd + 4);
  r.body = r.chunked ? decodeChunked(rawBody) : rawBody;
}

// HTTP リクエストを1往復する。wantBody=false なら本文を待たずに閉じる。
static HttpResult httpRequest(const String& host, const String& path, const String& method,
                              const String& body, const String& extraHeaders, bool wantBody) {
  AtBusyGuard busy; // ★2026-09-23 FW15: caOpen 開始から caClose 完了まで送信禁止。
  HttpResult r;
  uint32_t t0 = millis();
  // ★2026-09-24 FW24: 顧客への各 POST の結果をここで記録（iPEC の一部失敗も残す）。
  auto finish = [&]() -> HttpResult {
    r.elapsedMs = millis() - t0;
    if (method == "POST" && host != GAS_HOST &&
        (!r.headersComplete || r.status < 200 || r.status >= 300)) {
      logEvent("顧客送信失敗 status=" + String(r.status) + " " + r.body.substring(0, 60));
    }
    return r;
  };

  if (!caOpen(host.c_str(), 443)) { caClose(); return finish(); }
  waitWithLora(300); // ★2026-09-23 FW14: 待機中もLoRa受信を継続。

  String req = method + " " + path + " HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "User-Agent: MonitaGateway/" + String(GATEWAY_FW_VERSION) + "\r\n";
  req += extraHeaders;
  if (method == "POST") req += "Content-Length: " + String(body.length()) + "\r\n";
#if HTTP_KEEP_ALIVE
  req += "\r\n";   // Connection ヘッダー無し＝HTTP/1.1 既定の keep-alive
#else
  req += "Connection: close\r\n\r\n";
#endif
  req += body;

  s_urcDataMs = s_urcClosedMs = s_firstByteMs = 0;
  s_recvDiag = "";   // ★FW18: 前回の通信の診断文を持ち越さない
  if (!caSend(req)) { caClose(); return finish(); }
  s_reqSentMs = millis();

  // ★2026-09-22 修正: 応答が来る前に打ち切ってはいけない。
  //
  //   【何が起きたか】本文を読まない経路で「無音が5回続いたら終了」（約1.5秒）に
  //   していたため、GAS の doPost のように**実行に数秒かかる応答**を取りこぼし、
  //   実際にはシートへ書き込まれているのに status=0（失敗）と判定していた。
  //   認証エラー(401)はエッジが即返すので拾えてしまい、成功時だけ落ちるという
  //   気づきにくい壊れ方をしていた。
  //
  //   【対策】無音カウントは「一度でもデータが届いた後」だけ数える。
  //   まだ何も届いていない間は、全体のタイムアウトまで粘る。
  String raw = "", chunk;
  int emptyStreak = 0;
  // ★2026-09-23 FW14: 一時的なCARECV ERRORは3回連続まで再問い合わせする。
  int errorStreak = 0;
  uint32_t s = millis();
  // ★2026-09-23 FW14: GASの遅い応答を40秒まで待つ（待機中もWDT給餌）。
  uint32_t limit = wantBody ? 30000UL : 40000UL;
  while (millis() - s < limit) {
    int n = caRecvOnce(chunk);
    if (n < 0) {
      if (++errorStreak < 3) {
        Serial.print(F("  [HTTP] AT+CARECV ERROR 再試行（連続"));
        Serial.print(errorStreak); Serial.println(F("回、500ms待機）"));
        waitWithLora(500);
        continue;
      }
      Serial.print(F("  [HTTP] AT+CARECV が中断（受信済み "));
      Serial.print(raw.length()); Serial.print(F("バイト、経過 "));
      Serial.print(millis() - s); Serial.println(F("ms）"));
      Serial.print(F("  [HTTP] 理由: ")); Serial.println(s_recvDiag);
      // ★ソケットの状態を問い合わせる。
      //   +CASTATE: 0,0 … 相手（またはネットワーク）が接続を切った
      //   +CASTATE: 0,1 … まだ繋がっている（＝返事が来ないだけ／モジュール側の問題）
      //   何も出ない    … ソケット自体が既に破棄されている
      String st = sendAT("AT+CASTATE?", 3000);
      Serial.print(F("  [HTTP] ソケット状態: ")); Serial.println(visible(st));
      break;
    }
    errorStreak = 0;
    if (n == 0) {
      // 相手が切ってから1.5秒たっても0バイトなら、もう何も来ない。
      //   （切断の後でも受信済みの返事は読めるので、切断合図だけで打ち切ってはいけない）
      if (s_urcClosedMs != 0 && millis() - s_urcClosedMs > 1500) break;
      if (raw.length() > 0 && ++emptyStreak >= 10) break;   // 受信後の無音＝終端
      waitWithLora(300); // ★2026-09-23 FW14: 待機中もLoRa受信を継続。
      continue;
    }
    emptyStreak = 0;
    if (s_firstByteMs == 0) s_firstByteMs = millis();
    raw += chunk;
    // ★ステータス行だけで打ち切ると Location を取りこぼすため、ヘッダー末尾まで待つ。
    if (!wantBody && raw.indexOf("\r\n\r\n") >= 0) break;
    int he = raw.indexOf("\r\n\r\n");
    if (he >= 0) {
      HttpResult probe;
      parseHttpResponse(raw, probe);
      int bodyLen = raw.length() - (he + 4);
      if (probe.contentLength >= 0 && bodyLen >= probe.contentLength) break;
      if (probe.chunked && raw.indexOf("\r\n0\r\n") >= 0) break;
      if (probe.status >= 300 && probe.status < 400) break;
      // 204（本文なし）はヘッダーが揃えば完了。keep-alive では相手が切らないので、
      // これが無いと無音待ちの分だけ遅くなる。
      if (probe.status == 204 || probe.status == 304) break;
    }
    wdtFeed(); yield();
  }
  // 送信完了からの経過で、合図と受信のタイミングを1行に出す（成功・失敗とも）。
  //   成功時と失敗時を並べて比べられるようにするため、毎回出す。
  {
    auto rel = [](uint32_t t) -> String {
      return t ? ("+" + String(t - s_reqSentMs) + "ms") : String("なし");
    };
    Serial.print(F("  [HTTP] 送信後 → 着信合図:")); Serial.print(rel(s_urcDataMs));
    Serial.print(F(" 切断合図:"));  Serial.print(rel(s_urcClosedMs));
    Serial.print(F(" 最初の受信:")); Serial.println(rel(s_firstByteMs));
  }
  // ★2026-09-23 FW18: 着信合図があるのに1バイトも読めなかったとき（FW17 の InfluxDB で発生）、
  //   最後の AT+CARECV の生応答とソケット状態を残す。閉じる前に問い合わせる。
  if (raw.length() == 0 && s_urcDataMs != 0) {
    Serial.print(F("  [HTTP] 着信合図ありで0バイト。最後の CARECV: ")); Serial.println(s_recvDiag);
    String st = sendAT("AT+CASTATE?", 3000);
    Serial.print(F("  [HTTP] ソケット状態: ")); Serial.println(visible(st));
  }
  caClose();
  parseHttpResponse(raw, r);
  // ★2026-09-23 FW17: 想定外の応答（ヘッダー未完・2xx/3xx 以外）のときは受信内容の先頭をそのまま出す。
  //   FW16 で InfluxDB が status=2（204 のはずが数字が欠けた）になった。バイト落ちなのか
  //   別の文字が混ざったのかを、次に起きたときにログだけで切り分けられるようにする。
  if (!r.headersComplete || r.status < 200 || r.status >= 400) {
    Serial.print(F("  [HTTP] 想定外の応答 受信="));
    Serial.print(raw.length()); Serial.print(F("バイト 先頭=["));
    Serial.print(visible(raw.substring(0, 160))); Serial.println(F("]"));
  }

  // 応答が完結しているかを判定する。ヘッダーが揃っていないものは、
  // ステータスが 200 でも使ってはいけない。
  if (!r.headersComplete) {
    r.complete = false;
  } else if (!wantBody) {
    r.complete = true;                       // 本文は元々読まない経路
  } else if (r.contentLength >= 0) {
    r.complete = ((int)r.body.length() >= r.contentLength);
  } else if (r.chunked) {
    r.complete = (raw.indexOf("\r\n0\r\n") >= 0);
  } else {
    r.complete = true;                       // 長さの手がかりが無い場合は判断しない
  }

  return finish();
}

// ══════════════════════════════════════════════
// LoRa
// ══════════════════════════════════════════════
static bool loraSetMode(bool high) {
  digitalWrite(LORA_M0M1_PIN, high ? HIGH : LOW);
  delay(LORA_MODE_SWITCH_DELAY_MS);
  return true;
}
static inline bool loraModeNormal() { return loraSetMode(false); }
static inline bool loraModeConfig() { return loraSetMode(true); }

static bool loraReadConfig(uint8_t* out6) {
  uint32_t drainStart = millis();
  while (loraSerial.available()) {
    loraSerial.read();
    if (millis() - drainStart > 300UL) break;
  }
  loraSerial.write((uint8_t)0xC1);
  loraSerial.write((uint8_t)LORA_CFG_REG_START);
  loraSerial.write((uint8_t)LORA_CFG_REG_LEN);

  const int respLen = 3 + LORA_CFG_REG_LEN;
  uint8_t resp[3 + LORA_CFG_REG_LEN];
  int idx = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 500UL && idx < respLen) {
    if (loraSerial.available()) resp[idx++] = (uint8_t)loraSerial.read();
  }
  if (idx < respLen || resp[0] != 0xC1) return false;
  memcpy(out6, &resp[3], LORA_CFG_REG_LEN);
  return true;
}

static void loraWriteConfig() {
  loraSerial.write((uint8_t)0xC0);
  loraSerial.write((uint8_t)LORA_CFG_REG_START);
  loraSerial.write((uint8_t)LORA_CFG_REG_LEN);
  loraSerial.write(LORA_CFG_ADDH); loraSerial.write(LORA_CFG_ADDL);
  loraSerial.write(LORA_CFG_REG0); loraSerial.write(LORA_CFG_REG1);
  loraSerial.write(LORA_CFG_REG2); loraSerial.write(LORA_CFG_REG3);
  delay(200);
  uint32_t t0 = millis();
  while (millis() - t0 < 300UL) { while (loraSerial.available()) loraSerial.read(); }
}

// 起動毎に設定値を確認し、想定と違えば書き込む。
static bool loraCheckAndConfigure() {
  if (!loraModeConfig()) return false;
  uint8_t cur[LORA_CFG_REG_LEN] = {0};
  bool readOk = loraReadConfig(cur);
  bool matches = readOk &&
      cur[0] == LORA_CFG_ADDH && cur[1] == LORA_CFG_ADDL &&
      cur[2] == LORA_CFG_REG0 && cur[3] == LORA_CFG_REG1 &&
      cur[4] == LORA_CFG_REG2 && cur[5] == LORA_CFG_REG3;

  Serial.print(F("[LORA] config read "));
  Serial.println(matches ? F("一致") : F("不一致 → 書き込み"));
  if (readOk) {
    Serial.print(F("[LORA] 実測値: "));
    for (int i = 0; i < LORA_CFG_REG_LEN; i++) {
      if (cur[i] < 0x10) Serial.print('0');
      Serial.print(cur[i], HEX); Serial.print(' ');
    }
    Serial.println();
  }
  if (!matches) {
    loraWriteConfig();
    uint8_t verify[LORA_CFG_REG_LEN] = {0};
    matches = loraReadConfig(verify) &&
        verify[0] == LORA_CFG_ADDH && verify[1] == LORA_CFG_ADDL &&
        verify[2] == LORA_CFG_REG0 && verify[3] == LORA_CFG_REG1 &&
        verify[4] == LORA_CFG_REG2 && verify[5] == LORA_CFG_REG3;
    Serial.println(matches ? F("[LORA] 書込確認 OK") : F("[LORA] 書込確認 NG（配線・電源を確認）"));
  }
  loraModeNormal();
  return matches;
}

// ── 受信フレーム組み立て（状態機械）──────────────────
// フレーム形式: [0]SYNC=0xAA [1]LEN [2..LEN+1]MSDペイロード [LEN+2]チェックサム [+1]RSSI
// MSDペイロード（子機が送る19バイト固定）:
//   [0]PktType [1]DeviceID [2]FWVersion [3-10]CH1-4(int16 LE)
//   [11-12]BATT(mV) [13]Hour [14]Min [15-18]CH1-4 Range
enum LoraRxState { LORA_WAIT_SYNC, LORA_WAIT_LEN, LORA_WAIT_BODY, LORA_WAIT_CKSUM, LORA_WAIT_RSSI };
static LoraRxState s_loraState = LORA_WAIT_SYNC;
static uint8_t  s_loraLen = 0, s_loraBody[MAX_PAYLOAD], s_loraBodyIdx = 0;
static uint8_t  s_loraSum = 0, s_loraRssiRaw = 0;
// ★2026-09-23 FW14: チェックサム正常でも形式が異なるフレームを別集計。
static uint32_t s_loraBadFormat = 0;
static uint32_t s_loraLastRxMs = 0, s_loraCksumNg = 0, s_loraFramesOk = 0, s_loraRejected = 0;

#define LORA_RX_STALL_MS 30000UL

static void loraKickTx() {
  const uint8_t dummy[3] = {0x00, 0x00, 0x00};
  loraSerial.write(dummy, sizeof(dummy));
  loraSerial.flush();
  delay(200);
  while (loraSerial.available()) loraSerial.read();
}

// UARTE1 のエラー要因を回収し、受信が止まっていれば再起動する。
static void loraRxWatchdog() {
  uint32_t errsrc = NRF_UARTE1->ERRORSRC;
  if (errsrc) NRF_UARTE1->ERRORSRC = errsrc;
  if (NRF_UARTE1->EVENTS_ERROR) NRF_UARTE1->EVENTS_ERROR = 0;

  if (millis() - s_loraLastRxMs >= LORA_RX_STALL_MS) {
    s_loraLastRxMs = millis();
    NRF_UARTE1->TASKS_STARTRX = 1;
    Serial.println(F("[LORA] 受信ストール検出 → RX再起動"));
    loraModeNormal();
    loraKickTx();
  }
}

static bool loraFeedByte(uint8_t b) {
  switch (s_loraState) {
    case LORA_WAIT_SYNC:
      if (b == 0xAA) { s_loraSum = b; s_loraState = LORA_WAIT_LEN; }
      return false;
    case LORA_WAIT_LEN:
      s_loraLen = b; s_loraSum = (uint8_t)(s_loraSum + b); s_loraBodyIdx = 0;
      // ★2026-09-23 FW14: バッファに収まらない長さも形式NGとして集計する。
      if (s_loraLen == 0 || s_loraLen > MAX_PAYLOAD) {
        s_loraBadFormat++;
        Serial.print(F("[LORA] 形式NG LEN=")); Serial.println(s_loraLen);
        s_loraState = LORA_WAIT_SYNC;
        return false;
      }
      s_loraState = LORA_WAIT_BODY;
      return false;
    case LORA_WAIT_BODY:
      s_loraBody[s_loraBodyIdx++] = b;
      s_loraSum = (uint8_t)(s_loraSum + b);
      if (s_loraBodyIdx >= s_loraLen) s_loraState = LORA_WAIT_CKSUM;
      return false;
    case LORA_WAIT_CKSUM:
      if (b != s_loraSum) { s_loraCksumNg++; s_loraState = LORA_WAIT_SYNC; return false; }
      s_loraState = LORA_WAIT_RSSI;
      return false;
    case LORA_WAIT_RSSI:
      s_loraRssiRaw = b; s_loraState = LORA_WAIT_SYNC;
      return true;
    default:
      s_loraState = LORA_WAIT_SYNC;
      return false;
  }
}

// 自群の子機のフレームか判定する。
// ★子機IDを列挙せず群で判定するので、群内で機器を増やしても Gateway 側の修正は要らない。
static inline bool isAllowedLoRaPacket(uint8_t deviceId) {
  if ((deviceId & 0x1F) == 0) return false;          // 下位5bitが0は無効値
  return (deviceId >> 5) == GATEWAY_GROUP_ID;
}

static uint32_t rtcEpochNow() {
  if (!s_rtcAvailable) return 0;
  DateTime now = s_rtc.now();
  if (now.year() < 2026 || now.year() > 2035) return 0;   // I2Cノイズ由来の異常値を弾く
  return (uint32_t)now.unixtime();
}

static void removeRecordAt(int i) {
  for (int k = i; k + 1 < s_recordCount; k++) s_records[k] = s_records[k + 1];
  s_recordCount--;
}

// ★FW18: 子機ごとに最大 RECORDS_PER_DEVICE 件ためる。冗長フレームは1件にまとめる。
static void updateRecord(uint8_t deviceId, uint8_t fw, const int16_t* ch, uint8_t nCh,
                         uint16_t batt, uint8_t hour, uint8_t minute, bool timeValid, int rssi) {
  const uint32_t now = millis();
  if (nCh > LORA_MAX_CH) nCh = LORA_MAX_CH;
  // ★FW21: FNV-1a。RSSI は同じフレームでも変わるため含めない。
  uint32_t hash = 2166136261UL;
  auto addByte = [&hash](uint8_t v) { hash = (hash ^ v) * 16777619UL; };
  addByte(fw); addByte(nCh);
  for (int i = 0; i < nCh; i++) { addByte((uint16_t)ch[i] & 0xFF); addByte((uint16_t)ch[i] >> 8); }
  addByte(batt & 0xFF); addByte(batt >> 8);
  addByte(hour); addByte(minute); addByte(timeValid ? 1 : 0);
  int last = -1, freeSlot = -1, oldestFrame = 0;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (s_lastFrame[i].used && s_lastFrame[i].deviceId == deviceId) last = i;
    if (!s_lastFrame[i].used && freeSlot < 0) freeSlot = i;
    if (now - s_lastFrame[i].rxMs > now - s_lastFrame[oldestFrame].rxMs) oldestFrame = i;
  }
  if (last >= 0 && now - s_lastFrame[last].rxMs < DUP_WINDOW_MS && s_lastFrame[last].hash == hash) return;
  if (last < 0) last = (freeSlot >= 0) ? freeSlot : oldestFrame;
  s_lastFrame[last] = {true, deviceId, now, hash};

  int count = 0, oldest = -1;
  for (int i = 0; i < s_recordCount; i++) {
    if (s_records[i].deviceId != deviceId) continue;
    count++;
    if (oldest < 0) oldest = i;
  }
  if (count >= RECORDS_PER_DEVICE || s_recordCount >= MAX_RECORDS) {
    int drop = (count >= RECORDS_PER_DEVICE) ? oldest : 0;
    s_recordsDropped++;
    Serial.print(F("[BUF] ⚠ ためられる件数を超えたため、子機0x"));
    Serial.print(s_records[drop].deviceId, HEX);
    Serial.print(F(" の古い1件を捨てました（累計 ")); Serial.print(s_recordsDropped);
    Serial.println(F(" 件。Gateway の送信間隔を短くしてください）"));
    logEvent("バッファあふれ 子機0x" + String(s_records[drop].deviceId, HEX) + " 累計=" + String(s_recordsDropped));
    removeRecordAt(drop);
  }
  int idx = s_recordCount++;
  s_records[idx].rxMs     = now;
  s_records[idx].rtcEpoch = rtcEpochNow();

  s_records[idx].used      = true;
  s_records[idx].deviceId  = deviceId;
  s_records[idx].fwVersion = fw;
  s_records[idx].nCh = nCh;
  for (int i = 0; i < nCh; i++) s_records[idx].ch[i] = ch[i];
  s_records[idx].battMv   = batt;
  s_records[idx].hour     = hour;
  s_records[idx].minute   = minute;
  s_records[idx].timeValid = timeValid;
  s_records[idx].rssiDbm  = rssi;
}


// ★2026-09-24 FW21: 結果ページは HTTPS の指定ホストだけ許可する（クエリ中の文字列一致は不可）。
static bool isGasResultLocation(const String& loc, String& host, String& path) {
  if (!loc.startsWith("https://")) return false;
  unsigned end = 8;
  while (end < loc.length() && loc[end] != '/' && loc[end] != '?' && loc[end] != '#') end++;
  String authority = loc.substring(8, end);
  if (authority != "script.googleusercontent.com" && authority != "script.googleusercontent.com:443") return false;
  host = "script.googleusercontent.com";
  path = loc.substring(end);   // クエリを含め、ホスト以降をそのまま保つ
  if (path.length() == 0) path = "/";
  else if (path[0] != '/') path = "/" + path;
  return true;
}

static bool fetchGasResultBody(const String& location, String& body) {
  String host, path;
  if (!isGasResultLocation(location, host, path)) return false;
  waitWithLora(300);
  HttpResult r = httpRequest(host, path, "GET", "", "", true);
  body = r.body; body.trim();
  Serial.print(F("  [GAS] 結果: status=")); Serial.print(r.status);
  Serial.print(F(" 本文=[")); Serial.print(body.substring(0, 80)); Serial.println(F("]"));
  return r.status == 200 && r.complete;
}

#if ENABLE_DOWNLINK
// ★2026-09-23 FW15: Flex 専用の予約と報告キュー。
#define MAX_PENDING_CHILDREN 31
#define DOWNLINK_MAX_ATTEMPTS 3
#define DOWNLINK_DEDUP_MS 10000UL
#define DL_STATUS_NO_ACK 99
struct PendingDownlink {
  bool     active;
  uint8_t  childId;
  uint16_t sleepMin;
  uint8_t  avg;
  uint8_t  median;
  uint8_t  attempts;
  uint32_t seq;         // 予約の通し番号。報告に含めてGAS側で新旧を判別させる
  bool     statusOnly;  // trueなら設定変更フラグを立てずに送る（ステータス確認）
  uint32_t lastSendMs;  // 最後にこの子機へ送信した時刻（連続送信の抑制用）

};
struct DownlinkReport {
  bool     used;
  bool     finalResult;  // true=downlink_result（最終）/ false=downlink_sent（中間報告）
  uint8_t  childId;
  uint8_t  status;
  uint16_t sleepMin;
  uint8_t  avg;
  uint8_t  median;
  uint8_t  attempts;
  uint32_t seq;
  uint16_t wdtMin;  // 子機の確認応答に載る、適用後に有効になるWDTタイムアウト（分）
  uint32_t order;    // 積んだ順の通し番号。processReportQueue() はこの昇順で送る
};
struct SentDownlink {
  bool     valid;
  uint8_t  childId;
  uint8_t  attempts;
  uint32_t seq;
};
static PendingDownlink s_pending[MAX_PENDING_CHILDREN];
#define MAX_REPORTS 32
static DownlinkReport s_reports[MAX_REPORTS];
static SentDownlink s_lastSent[MAX_PENDING_CHILDREN];
static uint32_t s_reportOrder = 0, s_downlinkSent = 0, s_downlinkAck = 0, s_downlinkNoAck = 0;
// ★2026-09-23 FW15: Flex の予約・送信・報告処理を v1.21 から移植。
static void dropSupersededReports(uint8_t childId, uint32_t seq) {
  for (int i = 0; i < MAX_REPORTS; i++) {
    if (s_reports[i].used && !s_reports[i].finalResult &&
        s_reports[i].childId == childId && s_reports[i].seq == seq) {
      s_reports[i].used = false;
    }
  }
}
// ★2026-09-23 FW15: Flex の予約・送信・報告処理を v1.21 から移植。
static DownlinkReport* allocReportSlot(bool finalResult) {
  for (int i = 0; i < MAX_REPORTS; i++) {
    if (!s_reports[i].used) return &s_reports[i];
  }
  if (finalResult) {
    DownlinkReport* oldest = nullptr;
    for (int i = 0; i < MAX_REPORTS; i++) {
      if (!s_reports[i].finalResult && (oldest == nullptr || s_reports[i].order < oldest->order)) {
        oldest = &s_reports[i];
      }
    }
    if (oldest != nullptr) {
      Serial.println(F("[REPORT] キュー満杯のため最も古い中間報告を最終結果で上書きします"));
      return oldest;
    }
  }
  Serial.println(F("[REPORT] キューが満杯のため報告を破棄しました"));
  return nullptr;
}
// ★2026-09-23 FW15: Flex の予約・送信・報告処理を v1.21 から移植。
static void queueReport(bool finalResult, uint8_t childId, uint8_t status,
                        uint16_t sleepMin, uint8_t avg, uint8_t median,
                        uint8_t attempts, uint32_t seq, uint16_t wdtMin = 0) {
  if (finalResult) {
    logEvent("子機0x" + String(childId, HEX) + (status == DL_STATUS_NO_ACK ? " 未達" : " ACK status=" + String(status)) +
             " seq=" + String(seq));
    dropSupersededReports(childId, seq);
  }
  DownlinkReport* r = allocReportSlot(finalResult);
  if (r == nullptr) return;

  memset(r, 0, sizeof(*r));
  r->used        = true;
  r->finalResult = finalResult;
  r->childId     = childId;
  r->status      = status;
  r->sleepMin    = sleepMin;
  r->avg         = avg;
  r->median      = median;
  r->attempts    = attempts;
  r->seq         = seq;
  r->wdtMin      = wdtMin;
  r->order       = s_reportOrder++;
}
// ★2026-09-23 FW15: Flex の予約・送信・報告処理を v1.21 から移植。
static SentDownlink* findLastSent(uint8_t childId) {
  for (int i = 0; i < MAX_PENDING_CHILDREN; i++) {
    if (s_lastSent[i].valid && s_lastSent[i].childId == childId) return &s_lastSent[i];
  }
  return nullptr;
}
// ★2026-09-23 FW15: Flex の予約・送信・報告処理を v1.21 から移植。
static void recordSent(uint8_t childId, uint8_t attempts, uint32_t seq) {
  SentDownlink* e = findLastSent(childId);
  if (e == nullptr) {
    for (int i = 0; i < MAX_PENDING_CHILDREN; i++) {
      if (!s_lastSent[i].valid) { e = &s_lastSent[i]; break; }
    }
  }
  if (e == nullptr) return;
  e->valid    = true;
  e->childId  = childId;
  e->attempts = attempts;
  e->seq      = seq;
}
// ★2026-09-23 FW15: Flex の予約・送信・報告処理を v1.21 から移植。
static PendingDownlink* findPending(uint8_t childId) {
  for (int i = 0; i < MAX_PENDING_CHILDREN; i++) {
    if (s_pending[i].active && s_pending[i].childId == childId) return &s_pending[i];
  }
  return nullptr;
}
// ★2026-09-23 FW15: Flex の予約・送信・報告処理を v1.21 から移植。
static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
// ★2026-09-23 FW15: Flex の予約・送信・報告処理を v1.21 から移植。
static bool parseDecStrict(const String& s, uint8_t maxDigits, unsigned long& out) {
  const unsigned int len = s.length();
  if (len == 0 || len > maxDigits) return false;
  unsigned long v = 0;
  for (unsigned int i = 0; i < len; i++) {
    const char c = s.charAt(i);
    if (c < '0' || c > '9') return false;
    v = v * 10 + (unsigned long)(c - '0');
  }
  out = v;
  return true;
}
// ★2026-09-23 FW15: Flex の予約・送信・報告処理を v1.21 から移植。
static bool parseHexStrict(const String& s, uint8_t maxDigits, unsigned long& out) {
  const unsigned int len = s.length();
  if (len == 0 || len > maxDigits) return false;
  unsigned long v = 0;
  for (unsigned int i = 0; i < len; i++) {
    const int n = hexNibble(s.charAt(i));
    if (n < 0) return false;
    v = (v << 4) | (unsigned long)n;
  }
  out = v;
  return true;
}
// ★2026-09-23 FW15: Flex の予約・送信・報告処理を v1.21 から移植。
static void applyDownlinkCache(const String& body) {
  // ★2026-09-23 FW15: 報告失敗で GAS の attempts が古くても試行回数と完了状態を巻き戻さない。
  PendingDownlink previous[MAX_PENDING_CHILDREN];
  memcpy(previous, s_pending, sizeof(previous));
  memset(s_pending, 0, sizeof(s_pending));

  int slot = 0;
  int from = body.indexOf('\n');           // 1行目（コマンド）は読み飛ばす
  if (from < 0) {
    Serial.println(F("[DOWNLINK] 保留中のダウンリンク予約はありません"));
    return;
  }
  from++;

  while (from < (int)body.length() && slot < MAX_PENDING_CHILDREN) {
    int nl = body.indexOf('\n', from);
    String line = (nl < 0) ? body.substring(from) : body.substring(from, nl);
    from = (nl < 0) ? body.length() : nl + 1;
    line.trim();
    if (line.length() == 0) continue;

    int pos[7];
    int found = 0, scan = 0;
    while (found < 7) {
      int c = line.indexOf(':', scan);
      if (c < 0) break;
      pos[found++] = c;
      scan = c + 1;
    }
    if (found != 6) {
      Serial.print(F("[DOWNLINK] 書式不正のため無視: ")); Serial.println(line);
      continue;
    }

    // ★2026-08-28: 値域検証は必ず「格納型へ縮小変換する前」に行うこと。
    //   以前は (uint8_t)strtoul(...) のように切り詰めてから検証していたため、GASが誤って
    //   "101" を返すと 0x101 が 0x01 へ化け、実在する別の子機あての予約として受理された。
    //   群検証も切り詰め後の値に対して行われるので、他群あての異常値が自群の実在IDに
    //   化けて誤配送される経路になっていた（avg/medianの257→1、sleepMinの65537→1も同じ）。
    //   したがって一旦 long / unsigned long のまま受けてから値域を見る。
    // ★7番目(mode)の終端は、8番目(trig)が有るかどうかで変わる。
    String modeField = line.substring(pos[5] + 1);
    modeField.trim();
    // 桁数の上限：childId 2桁(16進) / sleep 4桁(≦1440) / avg・median 3桁 / attempts 3桁 / seq 9桁。
    // seq を9桁で止めるのは、unsigned long(32bit) で桁あふれさせないため（GASの採番は1ずつ）。
    unsigned long childIdRaw = 0, sleepMinU = 0, avgU = 0, medianU = 0, attemptsU = 0, seqU = 0;
    const bool numsOk =
        parseHexStrict(line.substring(0, pos[0]), 2, childIdRaw) &&
        parseDecStrict(line.substring(pos[0] + 1, pos[1]), 4, sleepMinU) &&
        parseDecStrict(line.substring(pos[1] + 1, pos[2]), 3, avgU) &&
        parseDecStrict(line.substring(pos[2] + 1, pos[3]), 3, medianU) &&
        parseDecStrict(line.substring(pos[3] + 1, pos[4]), 3, attemptsU) &&
        parseDecStrict(line.substring(pos[4] + 1, pos[5]), 9, seqU) &&
        (modeField == "0" || modeField == "1");
    if (!numsOk) {
      Serial.print(F("[DOWNLINK] 数値欄が不正（数字以外・空欄・桁あふれ）のため無視: ")); Serial.println(line);
      continue;
    }
    long          sleepMinRaw = (long)sleepMinU;
    long          avgRaw      = (long)avgU;
    long          medianRaw   = (long)medianU;
    long          attemptsRaw = (long)attemptsU;
    long          seqRaw      = (long)seqU;
    bool          statusOnly  = (modeField == "1");
    // ★群の検証。GAS側でも群別に配信するが、誤配信時の多重防御として必ず検証する。
    if (childIdRaw > 0xFF || (childIdRaw & 0x1F) == 0 ||
        (childIdRaw >> 5) != GATEWAY_GROUP_ID) {
      Serial.print(F("[DOWNLINK] 自群(")); Serial.print(GATEWAY_GROUP_ID);
      Serial.print(F(")宛でないため無視: ")); Serial.println(line);
      continue;
    }

    // ★ステータス確認(statusOnly)はsleep/avg/medianを使わない（GAS側は0を送ってくる）ので
    //   値域チェックの対象外にする。通常の設定変更だけ範囲を検証する。
    //   attempts/seqはstatusOnlyでも使うため常に検証する。
    if (attemptsRaw < 0 || attemptsRaw > 255 || seqRaw < 0) {
      Serial.print(F("[DOWNLINK] 値が範囲外のため無視: ")); Serial.println(line);
      continue;
    }
    if (!statusOnly &&
                       (sleepMinRaw < 1 || sleepMinRaw > 1440 ||
                        avgRaw < 1 || avgRaw > 255 ||
                        medianRaw < 1 || medianRaw > 255)) {
      Serial.print(F("[DOWNLINK] 値が範囲外のため無視: ")); Serial.println(line);
      continue;
    }

    uint8_t childId = (uint8_t)childIdRaw;  // 上で0x01〜0xFFに収まることを確認済み

    s_pending[slot].active     = true;
    s_pending[slot].childId    = childId;
    s_pending[slot].sleepMin   = (uint16_t)sleepMinRaw;
    s_pending[slot].avg        = (uint8_t)avgRaw;
    s_pending[slot].median     = (uint8_t)medianRaw;
    s_pending[slot].attempts   = (uint8_t)attemptsRaw;  // ★GAS側が正（Gatewayが再起動しても引き継がれる）
    s_pending[slot].seq        = (uint32_t)seqRaw;
    s_pending[slot].statusOnly = statusOnly;
    s_pending[slot].lastSendMs = 0;  // キャッシュ更新時は抑制をリセット（新しい予約として扱う）
    for (const auto& old : previous) {
      if (old.childId == childId && old.seq == (uint32_t)seqRaw) {
        s_pending[slot].attempts = max(old.attempts, s_pending[slot].attempts);
        s_pending[slot].lastSendMs = old.lastSendMs;
        s_pending[slot].active = old.active;
        break;
      }
    }
    slot++;

    Serial.print(F("[DOWNLINK] 予約: 子機0x")); Serial.print(childId, HEX);
    if (statusOnly) Serial.print(F(" ステータス確認のみ"));
    Serial.print(F(" 間隔=")); Serial.print(sleepMinRaw);
    Serial.print(F("分 平均=")); Serial.print(avgRaw);
    Serial.print(F(" メジアン=")); Serial.print(medianRaw);
    Serial.print(F(" 試行済=")); Serial.print(attemptsRaw);
    Serial.print(F(" seq=")); Serial.println(seqRaw);
  }
  Serial.print(F("[DOWNLINK] 有効な予約 ")); Serial.print(slot); Serial.println(F(" 件を保持しました"));
}
// ★2026-09-23 FW15: Flex の予約・送信・報告処理を v1.21 から移植。
static void processReportQueue() {
  for (;;) {
    // 積んだ順（order 昇順）に1件ずつ取り出す。配列の並びは順序を表さない
    int i = -1;
    for (int k = 0; k < MAX_REPORTS; k++) {
      if (s_reports[k].used && (i < 0 || s_reports[k].order < s_reports[i].order)) i = k;
    }
    if (i < 0) return;

    // GAS側は child を大文字16進2桁で判定する（/^[0-9A-F]{2}$/）ため、ここで整形する
    char childHex[3];
    snprintf(childHex, sizeof(childHex), "%02X", s_reports[i].childId);

    String q;
    if (s_reports[i].finalResult) {
      q  = "action=downlink_result&child="; q += childHex;
      q += "&group=";    q += String(GATEWAY_GROUP_ID);
      q += "&status=";   q += String(s_reports[i].status);
      q += "&sleep=";    q += String(s_reports[i].sleepMin);
      q += "&avg=";      q += String(s_reports[i].avg);
      q += "&median=";   q += String(s_reports[i].median);
      q += "&attempts="; q += String(s_reports[i].attempts);
      q += "&seq=";      q += String(s_reports[i].seq);
      q += "&wdt=";      q += String(s_reports[i].wdtMin);
    } else {
      q  = "action=downlink_sent&child="; q += childHex;
      q += "&group=";    q += String(GATEWAY_GROUP_ID);
      q += "&attempts="; q += String(s_reports[i].attempts);
      q += "&seq=";      q += String(s_reports[i].seq);
    }

    Serial.print(F("[REPORT] GASへ報告: ")); Serial.println(q);
    // ★送信中（sendAT の待機ループ内の loraPoll）に ACK が届くと、dropSupersededReports() が
    //   このスロットを空け、新しい最終結果が**同じスロットに**入ることがある。
    //   送信後に無条件で used=false にすると、その最終結果を消してしまう。order で本人確認する。
    const uint32_t sentOrder = s_reports[i].order;
    // ★2026-09-23 FW15: FW14 の CAOPEN 経路で送信。ヘッダー未完・認証リダイレクトは失敗。
    HttpResult result = httpRequest(GAS_HOST, "/macros/s/" + String(GAS_SCRIPT_ID) + "?" + q,
                                    "GET", "", "", false);
    // ★2026-09-24 FW21: 302 だけではロック失敗も成功に見える。結果本文で報告の受理を確認する。
    String body;
    bool fetched = result.headersComplete && result.status == 302 && fetchGasResultBody(result.location, body);
    bool ok = fetched && (body.startsWith("ok") || body.startsWith("stale"));
    // ★2026-09-24 FW21: 送り直しても通らないエラー（群の設定違い・子機IDの形式違い）は報告を捨てる。
    //   失敗扱いのままだと毎サイクル同じ報告で止まり、後ろに並んだ報告も永久に送れなくなるため。
    if (fetched && (body.startsWith("error: group mismatch") || body.startsWith("error: bad child id"))) {
      Serial.print(F("[REPORT] ⚠ GAS が恒久的なエラーを返したため報告を破棄: ")); Serial.println(body);
      ok = true;
    }
    Serial.print(F("[REPORT] 報告送信 status=")); Serial.print(result.status);
    Serial.println(ok ? F(" 成功") : F(" 失敗"));
    if (ok) {
      s_lastCloudSuccessMs = millis();
      if (s_reports[i].used && s_reports[i].order == sentOrder) s_reports[i].used = false;
    } else {
      Serial.println(F("[REPORT] 報告に失敗。次サイクルで再送します"));
      return;  // 通信不調とみなし、残りは次回に回す
    }
  }
}
// ★2026-09-23 FW15: 累計に未送信の報告数を表示する。
static unsigned reportQueueCount() {
  unsigned n = 0;
  for (const auto& r : s_reports) if (r.used) n++;
  return n;
}
// ★2026-09-23 FW15: Flex の予約・送信・報告処理を v1.21 から移植。
static void loraSendFrame(const uint8_t *msd, uint8_t msdLen) {
  uint8_t sum = (uint8_t)(0xAAU + msdLen);
  for (uint8_t i = 0; i < msdLen; i++) sum = (uint8_t)(sum + msd[i]);



  uint8_t frame[3 + 32];  // [SYNC][LEN][payload...][checksum]
  uint8_t n = 0;
  frame[n++] = 0xAA;
  frame[n++] = msdLen;
  for (uint8_t i = 0; i < msdLen; i++) frame[n++] = msd[i];
  frame[n++] = sum;

  loraSerial.write(frame, n);
  loraSerial.flush();
}
static const uint16_t DOWNLINK_COMPANY_ID   = 0xC0DE;  // 要: 子機ファームと一致させること
static const uint8_t  DOWNLINK_PKT_TYPE     = 0x81;
static const uint8_t  DOWNLINK_ACK_PKT_TYPE = 0x05;

static const uint8_t DOWNLINK_ACK_LEN_OLD = 7, DOWNLINK_ACK_LEN_CUR = 9;
enum DownlinkFlag { DL_FLAG_TIME = 1u << 0, DL_FLAG_SLEEP_MIN = 1u << 1, DL_FLAG_AVG_MEDIAN = 1u << 2 };
// ★2026-09-23 FW15: 400ms → 1200ms に変更。
//   上の 400ms は検証用スケッチ 25_lora_downlink_child（LORA_TX_REPEAT=1、1回だけ送信）で
//   測った値だった。v3.20・firmware_child・03_template/child は **2回送信**（LORA_TX_REPEAT=2）で、
//   受信窓が開くまでの流れが次のように長くなる（子機の1回目の送信開始を0ms）:
//       0ms  1回目送信 → 300ms 送信完了待ち → 100ms 間隔
//     400ms  2回目送信 → 300ms 送信完了待ち
//     700ms  E220 の受信を復活させるためのダミー送信 → 120ms 待ち
//    約850ms  受信窓が開く（〜約2850ms まで2秒間）
//   Gateway は1回目のフレームで反応し、2回目は冗長送信として無視するので、400ms だと
//   約520ms に送ってしまい、子機は2回目の送信直後で受信が止まっている＝毎回取りこぼす。
//   1200ms なら送出は約1310ms。1回送信の子機（窓 450〜2450ms）でも2回送信の子機
//   （窓 約850〜2850ms）でも窓の中に入る。
//   ※机上の計算。実機で子機ログの「RXウィンドウ内に受信なし」が続く場合はここを疑うこと
//     （生バイト受信数が0なら時間が合っていない、1以上なら届いているが形式・宛先で弾かれている）。
#define DOWNLINK_RESPONSE_DELAY_MS 1200UL
// ★2026-09-23 FW15: Flex の予約・送信・報告処理を v1.21 から移植。
static void sendDownlinkCommand(uint8_t targetDeviceId, uint16_t sleepMinutes,
                                uint8_t samplesPerAvg, uint8_t measureCount,
                                bool statusOnly) {
  // ★2026-09-23 FW15: E220 は既に M0M1=LOW。モード切替待ちを挟まず透過送信。

  uint8_t flags = statusOnly ? 0 : (DL_FLAG_SLEEP_MIN | DL_FLAG_AVG_MEDIAN);

  // 時刻欄はDL_FLAG_TIMEを立てていないため子機側で無視されるが、GatewayにはDS3231が
  // あるので実時刻を載せておく（将来この機能を有効にする際にそのまま使える）。
  DateTime now = s_rtcAvailable ? s_rtc.now() : DateTime(2000, 1, 1);

  uint8_t payload[15];
  payload[0]  = (uint8_t)(DOWNLINK_COMPANY_ID >> 8);
  payload[1]  = (uint8_t)(DOWNLINK_COMPANY_ID & 0xFF);
  payload[2]  = DOWNLINK_PKT_TYPE;
  payload[3]  = targetDeviceId;
  payload[4]  = flags;
  payload[5]  = (uint8_t)(now.year() % 100);
  payload[6]  = now.month();
  payload[7]  = now.day();
  payload[8]  = now.hour();
  payload[9]  = now.minute();
  payload[10] = now.second();
  payload[11] = (uint8_t)(sleepMinutes >> 8);
  payload[12] = (uint8_t)(sleepMinutes & 0xFF);
  payload[13] = samplesPerAvg;
  payload[14] = measureCount;

  Serial.print(F("[DOWNLINK] 送信: 宛先=0x")); Serial.print(targetDeviceId, HEX);
  Serial.print(F(" sleepMin=")); Serial.print(sleepMinutes);
  Serial.print(F(" avg=")); Serial.print(samplesPerAvg);
  Serial.print(F(" median=")); Serial.println(measureCount);

  loraSendFrame(payload, sizeof(payload));
  s_downlinkSent++;
  delay(300);  // 送信完了待ち（AUX未接続のため固定ディレイ）

  // ★送信直後にUARTE1のDMA受信を明示的に再武装する。
  // 実機で「ダウンリンクを1回送信した直後からGatewayの受信が完全に止まる」事象を確認した。
  // 子機はこの直後（2秒の受信窓の中）に確認応答を返してくるため、ここで取りこぼすと
  // 永久に確認が取れず再試行を繰り返すことになる。ストール検出を待たずに復帰させる。
  NRF_UARTE1->TASKS_STARTRX = 1;
}
// ★2026-09-23 FW15: Flex の予約・送信・報告処理を v1.21 から移植。
static void onUplinkReceived(uint8_t childId) {
  PendingDownlink* p = findPending(childId);
  if (p == nullptr) return;

  // ★AT通信中はダウンリンクを送らない。
  //   sendAT()の待機ループからloraPoll()が呼ばれるため、ここでLoRa送信（UARTへの書き込みと
  //   delay）を行うと、SIM7080Gへ送信中のATコマンド文字列がバイト単位で化ける事象を実機で
  //   確認している（loraRxWatchdog()のs_atBusyと同じ理由）。
  //   予約はactiveのまま残るので、子機の次の起床で改めて送信される（1周期遅れるだけ）。
  if (s_atBusy) {
    Serial.print(F("[DOWNLINK] 子機0x")); Serial.print(childId, HEX);
    Serial.println(F(" のアップリンクを検知しましたが、AT通信中のため次サイクルに見送ります"));
    return;
  }

  // ★子機の冗長送信（LORA_TX_REPEAT）を1回の起床として扱う。
  //   これが無いと同じ起床で試行回数を複数消費してしまう。
  if (p->lastSendMs != 0 && (millis() - p->lastSendMs) < DOWNLINK_DEDUP_MS) {
    Serial.print(F("[DOWNLINK] 子機0x")); Serial.print(childId, HEX);
    Serial.println(F(" の冗長送信とみなし、この分の応答は省略します"));
    return;
  }

  if (p->attempts >= DOWNLINK_MAX_ATTEMPTS) {
    // 規定回数送っても確認が返らなかった → 未達として打ち切り、GASへ報告する
    Serial.print(F("[DOWNLINK] 子機0x")); Serial.print(childId, HEX);
    Serial.print(F(" へ")); Serial.print(p->attempts);
    Serial.println(F("回送信しましたが確認が返りません。未達として打ち切ります"));
    queueReport(true, childId, DL_STATUS_NO_ACK, 0, 0, 0, p->attempts, p->seq);
    s_downlinkNoAck++;
    p->active = false;
    return;
  }

  p->attempts++;
  Serial.print(F("[DOWNLINK] 子機0x")); Serial.print(childId, HEX);
  Serial.print(F(" のアップリンクを検知（")); Serial.print(p->attempts);
  Serial.print(F("回目/")); Serial.print(DOWNLINK_MAX_ATTEMPTS);
  Serial.print(F("）→ ")); Serial.print(DOWNLINK_RESPONSE_DELAY_MS);
  Serial.println(F("ms待ってから送信します（子機が受信窓を開くのを待つ）"));

  // ★即座に送ると子機の受信窓が開く前に到着してしまう（DOWNLINK_RESPONSE_DELAY_MS参照）。
  //   ここでloraPoll()付きの待機を使ってはいけない。この関数自体がloraPoll()から
  //   呼ばれているため、待機中に別の子機のアップリンクが来ると再帰してしまう。
  //   受信バイトはUARTE割り込みでリングバッファに積まれるので、ここでポーリングを止めても
  //   取りこぼしにはならず、処理が数百ms遅れるだけで済む。
  delay(DOWNLINK_RESPONSE_DELAY_MS); // ★2026-09-23 FW15: loraPoll 再帰を防ぐ通常待機。
  wdtFeed();
  sendDownlinkCommand(childId, p->sleepMin, p->avg, p->median, p->statusOnly);
  p->lastSendMs = millis();
  recordSent(childId, p->attempts, p->seq);  // 確認応答をこの予約に紐付けるための控え
  queueReport(false, childId, 0, p->sleepMin, p->avg, p->median, p->attempts, p->seq);
}
// ★2026-09-23 FW15: Flex の予約・送信・報告処理を v1.21 から移植。
static void onDownlinkAckReceived(const uint8_t* ack, uint8_t len) {
  // ★以前は len >= 7 だった。7バイト以上のフレームが 0x05 で来ると何でもACKとして
  //   解釈してしまう（改修範囲メモ §5.2）。実害が出る前に厳密一致へ変える。
  if (len != DOWNLINK_ACK_LEN_OLD && len != DOWNLINK_ACK_LEN_CUR) {
    Serial.print(F("[DOWNLINK] ACKの長さが不正なため破棄: ")); Serial.println(len);
    return;
  }
  uint8_t  childId       = ack[1];
  uint8_t  status        = ack[2];
  uint16_t applied       = ((uint16_t)ack[3] << 8) | ack[4];
  uint8_t  appliedAvg    = ack[5];
  uint8_t  appliedMedian = ack[6];
  // 旧フレーム(7バイト)との互換のため、WDT欄が無い場合は0扱いにする
  uint16_t appliedWdtMin = (len >= 9) ? (((uint16_t)ack[7] << 8) | ack[8]) : 0;

  // ★seqと試行回数は「実際に送信した時の控え」から取る。
  //   現在のs_pending[]から取ると、送信〜応答の間にキャッシュ更新で新しい予約へ
  //   入れ替わっていた場合に、古い応答を新しい予約の完了として誤報告してしまう。
  SentDownlink* sent = findLastSent(childId);
  if (sent == nullptr) {
    Serial.print(F("[DOWNLINK] 送信控えが無い子機0x")); Serial.print(childId, HEX);
    Serial.println(F(" からの応答のため無視します"));
    return;
  }
  // ★2026-09-23 FW15: 定義外の status で予約を完了させない。
  // ★2026-09-23 FW16: status 3（子機の flash 保存失敗。設定は変更されていない）を受け付ける。
  //   子機 03_template FW12〜が返す。最終結果として GAS に報告し、GAS 側で「失敗」にする。
  if (status > 3) { Serial.println(F("[DOWNLINK] ACK status不正のため破棄")); return; }
  s_downlinkAck++;
  uint8_t  attempts = sent->attempts;
  uint32_t seq      = sent->seq;

  Serial.print(F("[DOWNLINK] ★子機0x")); Serial.print(childId, HEX);
  Serial.print(F(" から確認応答: status=")); Serial.print(status);
  Serial.print(F(" 適用値 間隔=")); Serial.print(applied);
  Serial.print(F("分 平均=")); Serial.print(appliedAvg);
  Serial.print(F(" メジアン=")); Serial.print(appliedMedian);
  Serial.print(F(" WDT=")); Serial.print(appliedWdtMin); Serial.println(F("分"));

  queueReport(true, childId, status, applied, appliedAvg, appliedMedian, attempts, seq, appliedWdtMin);
  sent->valid = false;  // この控えは消費した

  // ★再試行を止めてよいのは「今キャッシュにある予約」＝「今受け取った応答の予約」の時だけ。
  //   応答待ちの間に新しい予約へ入れ替わっていた場合、その新しい予約はまだ未送信なので
  //   activeのまま残し、次のアップリンクで送信されるようにする。
  PendingDownlink* p = findPending(childId);
  if (p != nullptr && p->seq == seq) p->active = false;
}
#endif

// ★2026-09-23 FW15: 正常アップリンク後に応答し、ACK は子機IDで群を検証する。
static void loraPoll() {
  while (loraSerial.available()) {
    uint8_t b = (uint8_t)loraSerial.read();
    s_loraLastRxMs = millis();
    if (!loraFeedByte(b)) continue;

    // ★2026-09-23 FW15: Flex ACK の7/9バイトだけを独立処理する。
#if ENABLE_DOWNLINK
    if (s_loraBody[0] == DOWNLINK_ACK_PKT_TYPE &&
        (s_loraLen == DOWNLINK_ACK_LEN_OLD || s_loraLen == DOWNLINK_ACK_LEN_CUR)) {
      if (!isAllowedLoRaPacket(s_loraBody[1])) { s_loraRejected++; continue; }
      onDownlinkAckReceived(s_loraBody, s_loraLen);
      continue;
    }
#endif
    // ★2026-09-23 FW14: 長さとPktTypeを確認し、短いフレームの前回値混入を防ぐ。
    // ★2026-09-23 FW15(0x06): 0x04（19バイト固定・4ch）に加えて 0x06（可変長）を受け付ける。
    //   0x06: [0]0x06 [1]ID [2]FW [3]n [4..3+2n]CH(int16 LE, 0x8000=欠測) [+2]BATT [+1]時 [+1]分(0xFF=無効)
    const uint8_t pkt = s_loraBody[0];
    uint8_t nCh = 0, hour = 0, minute = 0;
    int16_t ch[LORA_MAX_CH];
    uint16_t batt = 0;
    bool timeValid = true;
    if (pkt == LORA_PKT_TYPE_FIXED && s_loraLen == 19) {
      nCh = 4;
      for (int i = 0; i < 4; i++) {
        ch[i] = (int16_t)((uint16_t)s_loraBody[3 + i * 2] | ((uint16_t)s_loraBody[3 + i * 2 + 1] << 8));
      }
      batt   = (uint16_t)s_loraBody[11] | ((uint16_t)s_loraBody[12] << 8);
      hour   = s_loraBody[13];
      minute = s_loraBody[14];
      // 0x04 は時刻の無効値を持たない。範囲外は無効、0:0 は TREAT_ZERO_TIME_AS_INVALID で判断する。
      if (hour >= 24 || minute >= 60) timeValid = false;
#if TREAT_ZERO_TIME_AS_INVALID
      if (hour == 0 && minute == 0) timeValid = false;   // 0x04 の 0:0 は RTC 読み取り失敗の可能性が高い
#endif
    } else if (pkt == LORA_PKT_TYPE_VAR && s_loraLen >= 10 &&
               s_loraBody[3] >= 1 && s_loraBody[3] <= LORA_MAX_CH &&
               s_loraLen == (uint8_t)(8 + 2 * s_loraBody[3])) {
      nCh = s_loraBody[3];
      for (int i = 0; i < nCh; i++) {
        ch[i] = (int16_t)((uint16_t)s_loraBody[4 + i * 2] | ((uint16_t)s_loraBody[4 + i * 2 + 1] << 8));
      }
      batt   = (uint16_t)s_loraBody[4 + 2 * nCh] | ((uint16_t)s_loraBody[5 + 2 * nCh] << 8);
      hour   = s_loraBody[6 + 2 * nCh];
      minute = s_loraBody[7 + 2 * nCh];
      if (hour == LORA_TIME_INVALID || minute == LORA_TIME_INVALID || hour >= 24 || minute >= 60) timeValid = false;
    } else {
      s_loraBadFormat++;
      Serial.print(F("[LORA] 形式NG LEN=")); Serial.print(s_loraLen);
      Serial.print(F(" PktType=0x")); Serial.println(pkt, HEX);
      continue;
    }
    uint8_t deviceId = s_loraBody[1];
    if (!isAllowedLoRaPacket(deviceId)) { s_loraRejected++; continue; }

    s_loraFramesOk++;
    int rssiDbm = (int)s_loraRssiRaw - 256;

    Serial.print(F("[LORA RX] #")); Serial.print(s_loraFramesOk);
    Serial.print(F(" 0x")); Serial.print(pkt, HEX);
    Serial.print(F(" ID=0x")); Serial.print(deviceId, HEX);
    Serial.print(F(" n=")); Serial.print(nCh);
    Serial.print(F(" CH="));
    for (int i = 0; i < nCh; i++) {
      if (ch[i] == LORA_CH_MISSING && pkt == LORA_PKT_TYPE_VAR) Serial.print(F("欠測")); else Serial.print(ch[i]);
      Serial.print(' ');
    }
    Serial.print(F("BATT=")); Serial.print(batt);
    Serial.print(F(" TIME="));
    if (timeValid) { Serial.print(hour); Serial.print(':'); Serial.print(minute); } else Serial.print(F("無効"));
    Serial.print(F(" RSSI=")); Serial.print(rssiDbm); Serial.println(F("dBm"));

    // 0x04 の欠測値は存在しない（0 がそのまま入る）。0x06 以外では欠測表現に変換しない。
    if (pkt != LORA_PKT_TYPE_VAR) {
      for (int i = 0; i < nCh; i++) if (ch[i] == LORA_CH_MISSING) ch[i] = (int16_t)(LORA_CH_MISSING + 1);
    }
    updateRecord(deviceId, s_loraBody[2], ch, nCh, batt, hour, minute, timeValid, rssiDbm);
#if ENABLE_DOWNLINK
    onUplinkReceived(deviceId);
#endif
  }
}

// ══════════════════════════════════════════════
// 計測日時の組み立て
// ══════════════════════════════════════════════
// ★子機が送ってくるのは Hour/Min だけ（LoRaペイロードが19バイト固定のため年月日が入らない）。
//   Gateway の DS3231 が持つ年月日と組み合わせて完全な日時にする。
//   日付をまたぐ瞬間（23:59計測 → 00:0x受信）に1日ずれるため、受信時刻より
//   大きく未来になる場合は1日戻す。
// ★子機のRTC読み取り失敗を 0:0 として扱うか。
//   子機は ds3231GetTime() に失敗すると Hour/Min に 0 を入れて送ってくる
//   （msd[13] = rtcOk ? rtcT.hour : 0）。つまり 0:0 は「本当に0時0分」と
//   「RTC読み取り失敗」の区別がつかない。
//
//   1 にすると 0:0 を失敗とみなして計測日時を空欄にする。
//   0 にすると額面どおり 0時0分として扱う。
//
//   【なぜ既定を 1 にするか】実機で子機のDS3231が断続的に失敗しており
//   （2026-09-22、6フレーム中4フレームが 0:0）、そのまま組み立てると
//   「2026-09-22 00:00:00」という**もっともらしい間違った計測時刻**が
//   シートに入る。空欄のほうが「取れていない」ことが分かる。
//   ★本筋の対策は子機側のDS3231を直すこと。これは暫定の防御。


static String buildMeasuredAt(const FlexRecord& rec) {
  if (rec.rtcEpoch == 0) return "";
  if (!rec.timeValid) {   // ★2026-09-23 FW15(0x06): 子機が「時刻無効」を送ってきた（0x06）／範囲外
    Serial.print(F("  [RTC] 子機ID=0x")); Serial.print(rec.deviceId, HEX);
    Serial.println(F(" の計測時刻が無効。計測日時は空欄にします"));
    return "";
  }
  DateTime recv(rec.rtcEpoch);
  DateTime meas(recv.year(), recv.month(), recv.day(), rec.hour, rec.minute, 0);
  if (meas.unixtime() > recv.unixtime() + 12UL * 3600UL) {
    meas = DateTime(meas.unixtime() - 24UL * 3600UL);   // 日付をまたいだ
  }
  char buf[24];   // "YYYY-MM-DD HH:MM:00" は19文字。コンパイラの桁あふれ警告を避けるため余裕を持たせる
  snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:00",
           meas.year(), meas.month(), meas.day(), meas.hour(), meas.minute());
  return String(buf);
}

// ══════════════════════════════════════════════
// クラウド送信
// ══════════════════════════════════════════════
// GAS へ全台をまとめて1回の POST で送る。
// ★GET 時代は URL 512バイト制限のため台数を分割し、計測日時・BATT・FW・RSSI を
//   捨てていた。POST になったのでその制約が無い。
// ★2026-09-24 FW20: 302 の行き先（結果ページ）を読んで、doPost の戻り値を確かめる。
//   302 は「GAS に届いた」だけで、シートに書けたかは分からない。doPost は JSON が壊れていたり
//   ロック待ちで失敗したりしても例外にせず `error: …` を返すので、本文を読まないと成功と誤認する
//   （2026-09-23 22:26 に実際に1サイクル分がシートから消えた）。
//   本文は `ok:<書いた件数>`（重複を捨てた分があれば ` dup:<件数>` が続く）。合計が送った件数と一致したら成功。
static bool checkGasResult(const String& location, int sent) {
  String body;
  if (!fetchGasResultBody(location, body) || !body.startsWith("ok:")) {
    logEvent("GAS送信失敗 結果=" + body.substring(0, 60));
    Serial.println(F("  [GAS] ✗ 書き込みを確認できないため失敗として扱います"));
    return false;
  }
  long written = body.substring(3).toInt();
  long dup = 0;
  int di = body.indexOf("dup:");
  if (di >= 0) dup = body.substring(di + 4).toInt();
  if (written + dup != sent) {
    logEvent("GAS送信失敗 件数不一致 " + body.substring(0, 60));
    Serial.print(F("  [GAS] ✗ 件数が合わない（送信=")); Serial.print(sent);
    Serial.print(F(" 書込=")); Serial.print(written); Serial.print(F(" 重複=")); Serial.print(dup);
    Serial.println(F("）→ 失敗として扱い、全件を再送します（重複は GAS 側で捨てる）"));
    return false;
  }
  return true;
}

static bool postToGas(const FlexRecord* recs, int n) {
  String body = "{\"gw\":\"" + String(GATEWAY_NAME) + "\",\"csq\":" + String(s_lastCsq) + ",\"d\":[";
  for (int i = 0; i < n; i++) {
    if (i > 0) body += ",";
    body += "{\"id\":" + String(recs[i].deviceId);
    String t = buildMeasuredAt(recs[i]);
    if (t.length() > 0) body += ",\"t\":\"" + t + "\"";
    // ★FW18: Gateway の受信時刻。まとめて送ると GAS の受信日時が全件同じになるため
    if (recs[i].rtcEpoch != 0) {
      DateTime rx(recs[i].rtcEpoch);
      char rb[32];   // 19文字＋コンパイラの桁あふれ警告よけ
      snprintf(rb, sizeof(rb), "%04u-%02u-%02u %02u:%02u:%02u",
               rx.year(), rx.month(), rx.day(), rx.hour(), rx.minute(), rx.second());
      body += ",\"rx\":\"" + String(rb) + "\"";
    }
    body += ",\"ch\":[";
    // ★2026-09-23 FW15(0x06): 子機が送ってきたチャネル数だけ送る。欠測は null（GAS は null を空欄にする）。
    for (int c = 0; c < recs[i].nCh; c++) {
      if (c) body += ",";
      body += (recs[i].ch[c] == LORA_CH_MISSING) ? String("null") : String(recs[i].ch[c]);
    }
    body += "],\"batt\":" + String(recs[i].battMv);
    body += ",\"rssi\":" + String(recs[i].rssiDbm);
    body += ",\"fw\":" + String(recs[i].fwVersion) + "}";
  }
  body += "]}";

  Serial.print(F("  [GAS] ")); Serial.println(body);
  HttpResult r = httpRequest(GAS_HOST, "/macros/s/" + String(GAS_SCRIPT_ID), "POST",
                             body, "Content-Type: application/json\r\n", false);
  Serial.print(F("  [GAS] status=")); Serial.print(r.status);
  Serial.print(F(" ")); Serial.print(r.elapsedMs); Serial.println(F("ms"));
  // ★2026-09-23 FW14: ステータス行だけ届いた応答を成功と誤認しない。
  if (!r.headersComplete) { logEvent("GAS送信失敗 ヘッダー未完 status=" + String(r.status)); Serial.println(F("  [GAS] ヘッダー未完")); return false; }

  // ★GAS は成功時も 302 を返す（doPost は実行済みで、302 は結果ページへの誘導）。
  //   ただし**公開設定が誤っていると、同じ 302 で認証画面へ飛ばされる**。
  //   ステータスだけを見ていると後者を成功と数えてしまい、「ログ上は成功なのに
  //   シートに1行も入らない」という気づきにくい壊れ方をする。行き先で判別する。
  if (r.status >= 300 && r.status < 400) {
    String host, path;
    bool toContent = isGasResultLocation(r.location, host, path);
    Serial.print(F("  [GAS] → ")); Serial.println(r.location.length() ? r.location : String("(Location無し)"));
    if (!toContent) {
      logEvent("GAS送信失敗 不正なリダイレクト status=" + String(r.status));
      Serial.println(F("  [GAS] ✗ 認証画面へのリダイレクト。デプロイの「アクセスできるユーザー」を全員にすること"));
      return false;
    }
    return checkGasResult(r.location, n);
  }
  logEvent("GAS送信失敗 status=" + String(r.status) + " " + r.body.substring(0, 60));
  if (r.status == 200) Serial.println(F("[GAS] ✗ 302 ではなく 200 が返った（エラーページの可能性）"));
  return false;
}

// ★2026-09-23 FW15: 顧客送信実装を選択する。
#include "customer_sink.h"


// ══════════════════════════════════════════════
// ダウンリンク予約の取得（GAS → Drive の2段）
// ══════════════════════════════════════════════
#if ENABLE_DOWNLINK
// ★なぜ2段なのか
//   GAS Web App の応答は Content-Length が無く（常に chunked）、Gateway 側で
//   本文長を確定できない。そこで GAS には「集計して Drive に書け」と依頼するだけにし、
//   本文は Content-Length 付きで配信される Drive から読む。
//
// ★nonce の役割
//   1段目は本文を読まないため、GAS が失敗していても Gateway には成功に見える。
//   そのまま2段目を読むと「古いファイルを新鮮だと思い込む」事故になる。
//   毎回違う値を書かせ、読んだ内容の先頭行と突き合わせて鮮度を検証する。
static String makeNonce() {
  char buf[9];
  // ★2026-09-23 FW14: 連番と起動IDを混ぜ、millis下位16bitだけの繰り返しを避ける。
  static uint32_t seq = 0;
  uint32_t nonce = s_runId ^ (++seq * 2654435761UL) ^ millis();
  snprintf(buf, sizeof(buf), "%08lX", (unsigned long)nonce);
  return String(buf);
}

// ══════════════════════════════════════════════
// Gateway 設定（送信間隔）の保存・適用  ★2026-09-23 FW17
// ══════════════════════════════════════════════
// GAS の Gateway 設定欄で送信間隔を変えると、check_cmd の1行目に "interval:N"（分）が載る。
// ★CLAUDE.md §7: 送信間隔を変えるときはアプリ層WDTも必ず合わせる。本ファームのアプリ層WDTは
//   computeAppWdtMs(s_sendIntervalMs) で毎回計算するので、s_sendIntervalMs を変えれば追従する。
//   ハードWDT（WDT_TIMEOUT_MS）は loop() から常時給餌しているので送信間隔とは無関係。
// ★GAS への完了確認は、毎回の check_cmd に現在の間隔（gw_interval）を載せることで行う。
//   GAS は予約値と一致した時点で予約を消す。報告が失敗しても次の取得で必ず確定する。
static const char*   GW_SETTINGS_FILE     = "/gw_settings.bin";
static const char*   GW_SETTINGS_TMP_FILE = "/gw_settings.tmp";
static const uint8_t GW_SETTINGS_VERSION  = 1;
static const uint16_t GW_INTERVAL_MIN_MIN = 1;
static const uint16_t GW_INTERVAL_MAX_MIN = 1440;
struct GwSettings { uint8_t version; uint8_t reserved; uint16_t intervalMin; };

static void loadGatewaySettings() {
  if (!InternalFS.begin()) { Serial.println(F("[SETTINGS] flashの初期化に失敗。既定の送信間隔を使います")); return; }
  File f(InternalFS);
  if (!f.open(GW_SETTINGS_FILE, FILE_O_READ)) return;   // 保存なし＝既定値
  GwSettings tmp = {0, 0, 0};
  bool ok = ((size_t)f.size() == sizeof(tmp)) && (f.read((uint8_t*)&tmp, sizeof(tmp)) == (int)sizeof(tmp));
  f.close();
  if (ok && tmp.version == GW_SETTINGS_VERSION &&
      tmp.intervalMin >= GW_INTERVAL_MIN_MIN && tmp.intervalMin <= GW_INTERVAL_MAX_MIN) {
    s_sendIntervalMs = (uint32_t)tmp.intervalMin * 60000UL;
    s_sendPaused = (tmp.reserved & 1) != 0;   // ★FW24: 旧ファイルは reserved=0 のため送信中
  } else {
    Serial.println(F("[SETTINGS] 保存内容が不正なため破棄し、既定の送信間隔を使います"));
  }
}

// 一時ファイルへ書く → 読み戻して一致を確認 → 差し替え（子機 FW12 と同じ手順）。
// ★2026-09-24 FW24: 現在の間隔と停止フラグをまとめて保存。version=1 の互換性を保つ。
static bool saveGatewaySettings() {
  if (!InternalFS.begin()) { Serial.println(F("[SETTINGS] ✗ flashの初期化に失敗")); return false; }
  GwSettings st = {GW_SETTINGS_VERSION, (uint8_t)(s_sendPaused ? 1 : 0), (uint16_t)(s_sendIntervalMs / 60000UL)};
  InternalFS.remove(GW_SETTINGS_TMP_FILE);
  {
    File f(InternalFS);
    if (!f.open(GW_SETTINGS_TMP_FILE, FILE_O_WRITE)) { Serial.println(F("[SETTINGS] ✗ 一時ファイルを開けない")); return false; }
    size_t n = f.write((const uint8_t*)&st, sizeof(st));
    f.close();
    if (n != sizeof(st)) { Serial.println(F("[SETTINGS] ✗ 書き込み長が不一致")); return false; }
  }
  {
    File f(InternalFS);
    GwSettings chk;
    if (!f.open(GW_SETTINGS_TMP_FILE, FILE_O_READ)) { Serial.println(F("[SETTINGS] ✗ 読み戻しで開けない")); return false; }
    int n = f.read((uint8_t*)&chk, sizeof(chk));
    f.close();
    if (n != (int)sizeof(chk) || memcmp(&chk, &st, sizeof(chk)) != 0) { Serial.println(F("[SETTINGS] ✗ 読み戻した内容が一致しない")); return false; }
  }
  // ★2026-09-24 FW21: 削除→rename の間に電源断すると設定が消えるため、置換 rename にした。
  if (!InternalFS.rename(GW_SETTINGS_TMP_FILE, GW_SETTINGS_FILE)) { Serial.println(F("[SETTINGS] ✗ 差し替えに失敗")); return false; }
  return true;
}

// 現在の送信間隔を GAS へ知らせる（適用直後に1回。失敗しても次の check_cmd で伝わる）。
static void reportGatewayState() {
  String q = "?action=gw_state&device_id=" + String(GATEWAY_NAME) +
             "&group=" + String(GATEWAY_GROUP_ID) +
             "&gw_interval=" + String(s_sendIntervalMs / 60000UL);
  HttpResult r = httpRequest(GAS_HOST, "/macros/s/" + String(GAS_SCRIPT_ID) + q, "GET", "", "", false);
  bool ok = r.headersComplete && (r.status == 200 ||
            (r.status == 302 && r.location.indexOf("googleusercontent.com") >= 0));
  Serial.print(F("[GW-CMD] 現在値をGASへ報告 status=")); Serial.print(r.status);
  Serial.println(ok ? F(" 成功") : F(" 失敗（次の予約確認で伝わります）"));
  if (ok) s_lastCloudSuccessMs = millis();
}

// "interval:N" を適用する。範囲外・保存失敗なら変更しない（GAS 側には旧値が報告され続ける）。
static void applyGatewayIntervalCommand(const String& arg) {
  long n = arg.toInt();
  bool digits = arg.length() > 0;
  for (unsigned i = 0; i < arg.length(); i++) if (!isDigit(arg[i])) digits = false;
  if (!digits || n < GW_INTERVAL_MIN_MIN || n > GW_INTERVAL_MAX_MIN) {
    Serial.print(F("[GW-CMD] 送信間隔の値が不正のため無視: ")); Serial.println(arg);
    return;
  }
  uint32_t newMs = (uint32_t)n * 60000UL;
  if (newMs == s_sendIntervalMs) {
    logEvent("Gatewayコマンド interval:" + String(n) + " 適用済み");
    Serial.print(F("[GW-CMD] 送信間隔は既に ")); Serial.print(n); Serial.println(F("分です"));
    reportGatewayState();
    return;
  }
  uint32_t oldMs = s_sendIntervalMs;
  s_sendIntervalMs = newMs;
  if (!saveGatewaySettings()) {
    s_sendIntervalMs = oldMs;
    Serial.println(F("[GW-CMD] ✗ flashへの保存に失敗したため、送信間隔は変更しません"));
    return;
  }
  uint32_t oldMin = oldMs / 60000UL;
  logEvent("Gatewayコマンド interval:" + String(n));
  s_lastCloudSuccessMs = millis();   // 新しい間隔のアプリ層WDTを、ここから数え直す
  Serial.print(F("[GW-CMD] ★送信間隔を変更: ")); Serial.print(oldMin); Serial.print(F("分 → "));
  Serial.print(n); Serial.print(F("分（アプリWDT=")); Serial.print(computeAppWdtMs(s_sendIntervalMs) / 60000UL);
  Serial.println(F("分、flashへ保存済み）"));
  reportGatewayState();
}

// ★2026-09-24 FW24: 結果本文の ok を確認する。302 だけでは ack 成功とみなさない。
static bool gatewayResultOk(const HttpResult& r) {
  String body;
  return r.headersComplete && r.status == 302 && fetchGasResultBody(r.location, body) && body.startsWith("ok");
}

static void postStatusReport() {
  char runHex[9];
  snprintf(runHex, sizeof(runHex), "%08lX", (unsigned long)s_runId);
  s_lastCsq = readCsq();
  String rtc = "";
  if (s_rtcAvailable && !s_rtc.lostPower()) {
    DateTime now = s_rtc.now();
    if (now.isValid() && now.year() >= 2026 && now.year() <= 2035) {
      char tb[32];
      snprintf(tb, sizeof(tb), "%04u-%02u-%02u_%02u:%02u:%02u",
               now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
      rtc = tb;
    }
  }
  String q = "?action=status_report&device_id=" + String(GATEWAY_NAME) +
             "&fw=" + String(GATEWAY_FW_VERSION) + "&group=" + String(GATEWAY_GROUP_ID) +
             "&csq=" + String(s_lastCsq) + "&uptime_min=" + String(millis() / 60000UL) +
             "&free_heap=" + String(dbgHeapTotal() - dbgHeapUsed()) +
             "&interval=" + String(s_sendIntervalMs / 60000UL) + "&paused=" + String(s_sendPaused ? 1 : 0) +
             "&rtc=" + rtc + "&buf=" + String(s_recordCount) +
             "&retry_gas=" + String(s_gasRetry.n) + "&retry_cust=" + String(s_custRetry.n) +
             "&lora_ok=" + String(s_loraFramesOk) + "&lora_ng=" + String(s_loraCksumNg + s_loraBadFormat) +
             "&rejected=" + String(s_loraRejected) + "&dropped=" + String(s_recordsDropped) +
             "&gas_ok=" + String(s_gasOk) + "&gas_ng=" + String(s_gasNg) +
             "&cust_ok=" + String(s_cloudOk) + "&cust_ng=" + String(s_cloudNg) +
             "&dl_ok=" + String(s_dlOk) + "&dl_ng=" + String(s_dlNg) + "&run=" + String(runHex);
  HttpResult r = httpRequest(GAS_HOST, "/macros/s/" + String(GAS_SCRIPT_ID) + q, "GET", "", "", false);
  Serial.println(gatewayResultOk(r) ? F("[GW-CMD] ステータス報告 成功") : F("[GW-CMD] ステータス報告 失敗（再試行なし）"));
}

// 引用符・バックスラッシュ・制御文字を JSON 文字列としてエスケープする。
static String jsonEscape(const String& value) {
  String out;
  for (unsigned i = 0; i < value.length(); i++) {
    uint8_t c = (uint8_t)value[i];
    if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
    else if (c < 0x20) {
      char esc[7];
      snprintf(esc, sizeof(esc), "\\u%04x", c);
      out += esc;
    } else out += (char)c;
  }
  return out;
}

static void postEventLog() {
  char runHex[9];
  snprintf(runHex, sizeof(runHex), "%08lX", (unsigned long)s_runId);
  String body = "{\"type\":\"gw_log\",\"gw\":\"" + jsonEscape(GATEWAY_NAME) +
                "\",\"run\":\"" + String(runHex) + "\",\"lines\":[";
  // HTTP の待機中にも新しいログが入るため、送信前に古い順で本文を組み立てる。
  for (unsigned i = 0; i < s_eventLogCount; i++) {
    if (i) body += ",";
    unsigned idx = (s_eventLogNext + EVENT_LOG_MAX - s_eventLogCount + i) % EVENT_LOG_MAX;
    body += "\"" + jsonEscape(s_eventLog[idx]) + "\"";
  }
  body += "]}";
  HttpResult r = httpRequest(GAS_HOST, "/macros/s/" + String(GAS_SCRIPT_ID), "POST",
                             body, "Content-Type: application/json\r\n", false);
  Serial.println(gatewayResultOk(r) ? F("[GW-CMD] 診断ログ送信 成功") : F("[GW-CMD] 診断ログ送信 失敗"));
  // 成功しても消さない。次の吸い上げでも直近24件を確認できる。
}

static void handleGatewayCommand(const String& cmd) {
  if (cmd.length() == 0 || cmd == "none") return;
  if (cmd.startsWith("interval:")) { applyGatewayIntervalCommand(cmd.substring(9)); return; }

  // ★reset を ack より先に実行すると、予約が残って無限リセットになる。
  String q = "?action=ack_cmd&device_id=" + String(GATEWAY_NAME) + "&cmd=";
  // 未知コマンドも元の文字列のまま照合できるよう、クエリの値をエンコードする。
  for (unsigned i = 0; i < cmd.length(); i++) {
    uint8_t c = (uint8_t)cmd[i];
    if (isalnum(c) || c == '_' || c == '-' || c == '.' || c == '~') q += (char)c;
    else { char esc[4]; snprintf(esc, sizeof(esc), "%%%02X", c); q += esc; }
  }
  HttpResult r = httpRequest(GAS_HOST, "/macros/s/" + String(GAS_SCRIPT_ID) + q, "GET", "", "", false);
  if (!gatewayResultOk(r)) {
    Serial.print(F("[GW-CMD] ackに失敗したため ")); Serial.print(cmd); Serial.println(F(" は次のサイクルで実行します"));
    return;
  }
  if (cmd == "reset") {
    Serial.println(F("[GW-CMD] ★リモートリセット"));
    logEvent("Gatewayコマンド reset");
    // 再送待ちキューは RAM のため、この再起動で消える。
    Serial.flush();
    delay(100);
    NVIC_SystemReset();
  } else if (cmd == "stop" || cmd == "start") {
    bool previous = s_sendPaused;
    s_sendPaused = (cmd == "stop");
    if (!saveGatewaySettings()) {
      s_sendPaused = previous;
      logEvent("Gatewayコマンド " + cmd + " flash保存失敗");
      Serial.println(F("[GW-CMD] ✗ flash保存失敗。送信状態は変更しません"));
      return;
    }
    logEvent("Gatewayコマンド " + cmd);
    Serial.println(s_sendPaused ? F("[GW-CMD] ★送信停止（flash保存済み）") : F("[GW-CMD] 送信再開（flash保存済み）"));
  } else if (cmd == "send_now") {
    logEvent("Gatewayコマンド send_now");
    flushRecords();   // 停止中でも今あるデータと再送待ちを1回送る
  } else if (cmd == "status_now") {
    logEvent("Gatewayコマンド status_now");
    postStatusReport();
  } else if (cmd == "rtc_resync") {
    logEvent("Gatewayコマンド rtc_resync");
    syncRtcFromNetworkTime();
    postStatusReport();
  } else if (cmd == "log_dump") {
    logEvent("Gatewayコマンド log_dump");
    postEventLog();
  } else {
    Serial.print(F("[GW-CMD] 未対応のコマンドを破棄: ")); Serial.println(cmd);
  }
}

static bool fetchDownlink() {
  String nonce = makeNonce();

  // 1段目: GAS に集計と Drive 書き込みを依頼する（本文は読まない）
  // ★FW17: gw_interval で現在の送信間隔を毎回知らせる（GAS 側の完了確認と表示に使う）。
  String q = "?action=check_cmd&device_id=" + String(GATEWAY_NAME) + "&dl=1&group=" + String(GATEWAY_GROUP_ID) +
             "&gw_interval=" + String(s_sendIntervalMs / 60000UL) + "&nonce=" + nonce;
  HttpResult r1 = httpRequest(GAS_HOST, "/macros/s/" + String(GAS_SCRIPT_ID) + q,
                              "GET", "", "", false);
  // ★2026-09-23 FW14: 1段目が無応答でも、2段目のnonce照合で鮮度を判定する。
  if (r1.status == 0) {
    Serial.println(F("  [DL] 1段目の応答なし。2段目で nonce を照合して判定"));
  } else if (!(r1.status == 200 || (r1.status >= 300 && r1.status < 400))) {
    Serial.print(F("  [DL] 1段目(GAS依頼)失敗 status=")); Serial.println(r1.status);
    return false;
  }
  waitWithLora(1000); // ★2026-09-23 FW14: GASの書き込み待ちもLoRa受信を継続。

  // 2段目: Drive のファイルを読む
  String path = "/download?id=" + String(DOWNLINK_DRIVE_FILE_ID) + "&export=download";
  HttpResult r2 = httpRequest(DRIVE_HOST, path, "GET", "", "", true);
  if (r2.status != 200 || !r2.complete || r2.body.length() == 0) {
    Serial.print(F("  [DL] 2段目(Drive取得)失敗 status=")); Serial.print(r2.status);
    if (!r2.complete) Serial.print(F("（応答が途中で切れています）"));
    Serial.print(F(" 本文=")); Serial.print(r2.body.length());
    Serial.print(F("バイト Content-Length=")); Serial.print(r2.contentLength);
    Serial.print(F(" 受信計=")); Serial.print(r2.rawBytes); Serial.println(F("バイト"));
    return false;
  }

  int nl = r2.body.indexOf('\n');
  String got = (nl < 0) ? r2.body : r2.body.substring(0, nl);
  got.trim();
  if (got != nonce) {
    Serial.print(F("  [DL] nonce不一致（期待=")); Serial.print(nonce);
    Serial.print(F(" 実際=")); Serial.print(got);
    Serial.println(F("）→ 内容を破棄。GAS側がDriveへ書けていない可能性"));
    return false;
  }

  String payload = (nl < 0) ? "" : r2.body.substring(nl + 1);
  payload.trim();
  Serial.print(F("  [DL] 取得OK: ")); Serial.println(payload);

  // ★2026-09-23 FW15: nonce 検証済み本文だけを採用。
  // ★2026-09-24 FW24: 予約枠は1つ。子機予約を反映してから Gateway コマンドを1件だけ実行する。
  String command = payload.substring(0, payload.indexOf('\n') < 0 ? payload.length() : payload.indexOf('\n'));
  command.trim();
  applyDownlinkCache(payload);
  handleGatewayCommand(command);
  return true;
}
#endif

// ══════════════════════════════════════════════
// 送信サイクル
// ══════════════════════════════════════════════
static void flushRecords() {
  // バッファを取り出してクリアする（送信中に届いた分は次サイクルへ）
  static FlexRecord snap[MAX_RECORDS];
  int n = 0;
  for (int i = 0; i < s_recordCount; i++) snap[n++] = s_records[i];
  s_recordCount = 0;
  // ★FW20: 新しい分が無くても、再送待ちがあれば送る
  const bool haveRetry = (s_gasRetry.n > 0 || s_custRetry.n > 0);

  if (n == 0 && !haveRetry) {
    // ★2026-09-22 修正: ここで単に return すると s_lastCloudSuccessMs が一度も
    //   更新されず、アプリ層WDTが「クラウド送信が成功していない」と判断して
    //   延々と再起動を繰り返す（実機で無限リブートを起こした）。
    //
    //   アプリ層WDTの役目は「モデムは繋がっているのに送信だけが固着した状態」の
    //   検出であって、**そもそも送るデータが無い状態は故障ではない**。
    //   LoRa が受からないことは LoRa 側のストール検出が扱う担当で、
    //   ここで再起動を掛けても直らないうえ、再起動中はさらに受信できなくなる。
    Serial.println(F("  送信対象レコードなし（クラウド送信は試行せず。WDTは維持）"));
    s_lastCloudSuccessMs = millis();
    return;
  }
  // ★2026-09-24 FW21: seq は新しい計測を取り出したサイクル番号。再送だけなら増やさない。
  //   欠番はサイクル全体の欠落を表す。一部だけの欠落は件数で確認する。
  if (n > 0) s_sendSeq++;
  for (int i = 0; i < n; i++) snap[i].seq = s_sendSeq;   // ★FW20: 再送しても元の seq を保つ
  {
    int devices = 0;
    for (int i = 0; i < n; i++) {
      bool seen = false;
      for (int k = 0; k < i; k++) if (snap[k].deviceId == snap[i].deviceId) { seen = true; break; }
      if (!seen) devices++;
    }
    Serial.print(F("  送信対象: ")); Serial.print(n); Serial.print(F(" 件（"));
    Serial.print(devices); Serial.print(F(" 台）seq=")); Serial.println(s_sendSeq);
  }

  s_lastCsq = readCsq();

  bool anySuccess = false;

  static FlexRecord batch[RETRY_MAX + MAX_RECORDS];   // ★FW20: 再送待ち＋今回分
#if SEND_TO_GAS
  {
    int m = retryMerge(s_gasRetry, snap, n, batch);
    // ★2026-09-24 FW21: 他方の再送だけなら、この送信先へ空のPOSTをせず成否も数えない。
    if (m > 0) {
      if (postToGas(batch, m)) { s_gasOk++; s_gasConsecNg = 0; anySuccess = true; }
      else                     { s_gasNg++; s_gasConsecNg++; retryPush(s_gasRetry, batch, m, F("GAS")); }
    }
  }
  waitWithLora(1000); // ★2026-09-23 FW14: 待機中もLoRa受信を継続。
#endif

// ★2026-09-23 FW15: NONE では顧客送信と成否集計を行わない。
#if CUSTOMER_SINK != CUSTOMER_SINK_NONE
  // ★FW18: 送り方（まとめて1回／1件ずつ）は送信先ごとに customer_sink.h が決める。
  //   成否はその POST の回数で数える。
  {
    int m = retryMerge(s_custRetry, snap, n, batch);
    if (m > 0 && !postToCustomerBatch(batch, m, anySuccess)) retryPush(s_custRetry, batch, m, F("顧客"));
  }
#endif
#if CUSTOMER_SINK == CUSTOMER_SINK_NONE && !SEND_TO_GAS && !ENABLE_DOWNLINK
  anySuccess = true; // ★2026-09-23 FW15: 全送信を無効にした受信専用設定は通信故障ではない。
#endif

  // ★アプリ層WDTは「どちらか一方でも成功なら生存」とする。
  //   「両方成功」を条件にすると、片方のサーバー障害だけで再起動ループに入る。
  //   代わりに経路ごとの連続失敗数を別々に数えて、どちらが壊れたか分かるようにする。
  if (anySuccess) s_lastCloudSuccessMs = millis();
}

static void printSummary() {
  Serial.println(F("╔══════════ 累計 ══════════"));
  Serial.print(F("  LoRa: OK=")); Serial.print(s_loraFramesOk);
  Serial.print(F(" チェックサムNG=")); Serial.print(s_loraCksumNg);
  // ★2026-09-23 FW14: 形式不一致を累計にも表示する。
  Serial.print(F(" 形式NG=")); Serial.print(s_loraBadFormat);
  Serial.print(F(" 群外破棄=")); Serial.print(s_loraRejected);
  Serial.print(F(" あふれ破棄=")); Serial.println(s_recordsDropped);
  Serial.print(F("  再送待ち: GAS=")); Serial.print(s_gasRetry.n);
  Serial.print(F(" 顧客=")); Serial.print(s_custRetry.n);
  Serial.print(F("  再送あふれ: GAS=")); Serial.print(s_gasRetry.dropped);
  Serial.print(F(" 顧客=")); Serial.println(s_custRetry.dropped);
  Serial.print(F("  GAS : OK=")); Serial.print(s_gasOk);
  Serial.print(F(" NG=")); Serial.print(s_gasNg);
  Serial.print(F(" 連続NG=")); Serial.println(s_gasConsecNg);
// ★2026-09-23 FW15: 選択した顧客送信先を表示する。
  Serial.print(F("  顧客(")); Serial.print(CUSTOMER_SINK_NAME); Serial.print(F("): OK="));
  Serial.print(s_cloudOk); Serial.print(F(" NG=")); Serial.print(s_cloudNg);
  Serial.print(F(" 連続NG=")); Serial.println(s_cloudConsecNg);
#if ENABLE_DOWNLINK
  Serial.print(F("  ダウンリンク: OK=")); Serial.print(s_dlOk);
  Serial.print(F(" NG=")); Serial.println(s_dlNg);
  // ★2026-09-23 FW15: 子機送信と GAS 報告の進捗を分けて表示。
  Serial.print(F("  [DOWNLINK] 送信=")); Serial.print(s_downlinkSent);
  Serial.print(F(" ACK=")); Serial.print(s_downlinkAck);
  Serial.print(F(" 未達=")); Serial.print(s_downlinkNoAck);
  Serial.print(F(" 報告待ち=")); Serial.println(reportQueueCount());
  // ★長時間試験用。String を多用しているので、ヒープが減り続けていれば
  //   断片化・リークで最終的に固まる兆候。値が横ばいなら問題なし。
  Serial.print(F("  稼働: ")); Serial.print(millis() / 60000UL);
  Serial.print(F("分  空きヒープ: ")); Serial.print(dbgHeapTotal() - dbgHeapUsed());
  Serial.println(F("バイト"));
#endif
  Serial.println(F("╚══════════════════════════"));
}

// ══════════════════════════════════════════════
// Arduino エントリ
// ══════════════════════════════════════════════
// ★起動時に「なぜ再起動したか」を表示する。
//   USB シリアルは XIAO が再起動すると切断されるため、モニタ上では
//   「ポート接続が切れた」ようにしか見えない。長時間試験で再起動が起きたとき、
//   ハードWDT／アプリ層WDT／電源断のどれかを切り分けられないと原因を追えない。
//   （Adafruit コアが起動時に RESETREAS を退避してクリアするので readResetReason() で読む）
static void printResetReason() {
  uint32_t r = readResetReason();
  Serial.print(F(" 前回のリセット: 0x")); Serial.print(r, HEX); Serial.print(F(" → "));
  if (r == 0)                          Serial.println(F("電源投入／電圧低下（ブラウンアウト）"));
  else if (r & POWER_RESETREAS_DOG_Msk)     Serial.println(F("★ハードWDT（処理が固まって給餌が止まった）"));
  else if (r & POWER_RESETREAS_SREQ_Msk)    Serial.println(F("ソフトリセット（アプリ層WDT・書き込み直後など）"));
  else if (r & POWER_RESETREAS_LOCKUP_Msk)  Serial.println(F("★CPUロックアップ（異常動作）"));
  else if (r & POWER_RESETREAS_RESETPIN_Msk)Serial.println(F("リセットボタン／リセットピン"));
  else                                      Serial.println(F("その他"));
}

// ★2026-09-24 FW23: 起動ログ用の機器情報。取り方は本番 v1.21 と同じ（台帳の値と突き合わせられるように）。
static String getXiaoId() {
  char buf[17];
  snprintf(buf, sizeof(buf), "%08lX%08lX",
           (unsigned long)NRF_FICR->DEVICEID[1], (unsigned long)NRF_FICR->DEVICEID[0]);
  return String(buf);
}
// 応答の中から、minLen 桁以上続く数字を取り出す（IMEI は15桁、ICCID は19〜20桁）
static String firstDigits(const String& res, int minLen) {
  for (int i = 0; i < (int)res.length(); i++) {
    if (!isDigit(res[i])) continue;
    int j = i;
    while (j < (int)res.length() && isDigit(res[j])) j++;
    if (j - i >= minLen) return res.substring(i, j);
    i = j;
  }
  return "";
}
static const char* resetReasonName(uint32_t r) {
  if (r == 0)                               return "power_on";
  if (r & POWER_RESETREAS_DOG_Msk)          return "hard_wdt";
  if (r & POWER_RESETREAS_SREQ_Msk)         return "soft_reset";
  if (r & POWER_RESETREAS_LOCKUP_Msk)       return "lockup";
  if (r & POWER_RESETREAS_RESETPIN_Msk)     return "reset_pin";
  return "other";
}

static bool s_bootInfoPending = false;   // 起動ログを GAS に届けられていなければ、次の送信サイクルで送り直す
static uint32_t s_bootResetReason = 0;
static String   s_bootRtc = "";

// 起動ログを GAS へ送る。結果本文が ok なら完了。失敗しても動作は止めない（次サイクルで再送）。
static void postBootInfo() {
  char runHex[9];
  snprintf(runHex, sizeof(runHex), "%08lX", (unsigned long)s_runId);
  String q = "?action=gw_boot&gw=" + String(GATEWAY_NAME);
  q += "&group=" + String(GATEWAY_GROUP_ID);
  q += "&fw=" + String(GATEWAY_FW_VERSION);
  q += "&xiao=" + getXiaoId();
  q += "&imei=" + firstDigits(sendAT("AT+GSN", 3000), 10);
  q += "&iccid=" + firstDigits(sendAT("AT+CCID", 3000), 15);
  q += "&sim=" + String(SIM_NAME);
  q += "&csq=" + String(readCsq());
  q += "&interval=" + String(s_sendIntervalMs / 60000UL);
  q += "&appwdt=" + String(computeAppWdtMs(s_sendIntervalMs) / 60000UL);
  q += "&sink=" + String(CUSTOMER_SINK_NAME);
  q += "&reset=" + String(resetReasonName(s_bootResetReason));
  q += "&run=" + String(runHex);
  q += "&rtc=" + s_bootRtc;
  Serial.println(F("--- 起動ログを GAS へ送信 ---"));
  HttpResult r = httpRequest(GAS_HOST, "/macros/s/" + String(GAS_SCRIPT_ID) + q, "GET", "", "", false);
  String body;
  bool ok = r.headersComplete && r.status == 302 && fetchGasResultBody(r.location, body) && body.startsWith("ok");
  s_bootInfoPending = !ok;
  Serial.println(ok ? F("[BOOT] 起動ログを記録しました") : F("[BOOT] 起動ログの記録に失敗。次の送信サイクルで送り直します"));
  if (ok) s_lastCloudSuccessMs = millis();
}

static void printRunId() {
  char buf[9];
  snprintf(buf, sizeof(buf), "%08lX", (unsigned long)s_runId);   // 先頭の0も出す
  Serial.print(F(" runId        : ")); Serial.println(buf);
}

// ★2026-09-24 FW22: LTE-M に接続できなかったときの診断。
//   lteConnect() は送ったコマンドしか表示しないため、失敗しても「SIM が無いのか・電波が無いのか・
//   登録を拒否されたのか」が分からなかった（2026-09-24、アンテナの接触不良で2回失敗）。
//   返事をそのまま表示する。見方:
//     +CPIN: READY 以外 … SIM が認識されていない（挿さり・接点）
//     +CSQ: 99,99 / 0〜5 … 電波が無い・弱い（アンテナの接続・置き場所）
//     +CEREG: 0,2        … 基地局を探している最中 / 0,3 … 登録拒否（SIM の開通・APN）
static void lteDiagnose() {
  Serial.println(F("── LTE-M 診断（モジュールの返事）──"));
  const char* cmds[] = {"AT+CPIN?", "AT+CSQ", "AT+CREG?", "AT+CEREG?", "AT+CGATT?", "AT+COPS?", "AT+CPSI?"};
  for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
    String r = sendAT(cmds[i], 3000);
    Serial.print(F("   → ")); Serial.println(visible(r));
  }
}

// 起動時に LTE-M へつながらなかったときの復旧。SIM7080G を再起動し、少し待ってからマイコンも再起動する。
// ★起動直後なので送信待ちのデータは無く、再起動で失うものは無い。待っている間も LoRa は受信する
//   （受信した分は再起動で消えるが、LTE が無い間は送る手段も無い）。
static const uint32_t LTE_RETRY_WAIT_MS = 90000UL;   // 再起動までの待ち（電波の回復待ち）
static void lteFailRecover(bool modemResponds) {
  logEvent("LTE接続失敗 modemResponds=" + String(modemResponds ? 1 : 0));
  if (modemResponds) {
    lteDiagnose();
    Serial.println(F("SIM7080G を再起動します（AT+CFUN=1,1）"));
    sendAT("AT+CFUN=1,1", 10000);
  }
  Serial.print(LTE_RETRY_WAIT_MS / 1000UL);
  Serial.println(F("秒後にマイコンを再起動して、LTE-M への接続をやり直します"));
  waitWithLora(LTE_RETRY_WAIT_MS);
  Serial.flush();
  delay(100);
  NVIC_SystemReset();
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 5000) delay(10);
  Serial1.begin(115200);

  // ★2026-09-23 修正: ここで analogRead(A0) を呼んでいたのを削除した。
  //   XIAO nRF52840 では **A0 = D0 = P0.02 = LoRa の RX（E220 TXD → XIAO）**。
  //   乱数の種を取るつもりで LoRa の受信ピンを SAADC（アナログ入力）に繋いでしまい、
  //   FW 4 以降 E220 の設定読み戻し（config read）も LoRa 受信も一切できなくなった。
  //   **ピンを使う処理を追加するときは、必ず上のピン割当表と突き合わせること。**
  //
  //   runId の種はピンに触れないものだけで作る:
  //     ・チップ固有ID（FICR）… 個体の区別
  //     ・RTC の epoch        … 起動時刻の区別（RTC の電池が生きていれば）
  //     ・LTE 接続完了時の micros() … 網への登録時間は毎回数百ms単位でばらつく。
  //       RTC がリセット値（2000-01-01）でも起動ごとに確実に分かれる
  s_runId = NRF_FICR->DEVICEID[0] ^ 0xA5A5A5A5UL;

  Serial.println(F("\n================================================"));
  // ★2026-09-23 FW15: 案件名をバナーにも表示。
  Serial.print(F(" Gateway: ")); Serial.println(GATEWAY_NAME);
  Serial.println(F(" LoRa受信 → CAOPEN で GAS / クラウドへ POST"));
  Serial.println(F("================================================"));
  Serial.print(F(" FW           : ")); Serial.println(GATEWAY_FW_VERSION);
  printResetReason();
  s_bootResetReason = readResetReason();   // ★FW23: 起動ログ用
  Serial.print(F(" 群           : ")); Serial.println(GATEWAY_GROUP_ID);
#if ENABLE_DOWNLINK
  loadGatewaySettings();   // ★FW17: 送信間隔の変更はダウンリンク経由なので、その機能とセットで有効
#endif
  Serial.print(F(" 送信間隔     : ")); Serial.print(s_sendIntervalMs / 60000UL); Serial.print(F("分"));
  if (s_sendIntervalMs != SEND_INTERVAL_MS) {
    Serial.print(F("（GASから変更した値。project_config の既定=")); Serial.print(SEND_INTERVAL_MS / 60000UL); Serial.print(F("分）"));
  }
  Serial.println();
  if (s_sendPaused) Serial.println(F(" ★送信停止中（GAS の「データ送信を再開」で戻る）"));
  Serial.print(F(" アプリWDT    : "));
  Serial.print(computeAppWdtMs(s_sendIntervalMs) / 60000UL); Serial.println(F("分"));
  Serial.print(F(" GAS送信      : ")); Serial.println(SEND_TO_GAS ? "有効" : "無効");
  Serial.print(F(" クラウド送信 : ")); Serial.println(CUSTOMER_SINK_NAME);
  Serial.print(F(" ダウンリンク : ")); Serial.println(ENABLE_DOWNLINK ? "有効" : "無効");

  wdtInit(WDT_TIMEOUT_MS);

  // RTC
  Wire.begin();
  s_rtcAvailable = s_rtc.begin();
  if (s_rtcAvailable) {
    DateTime now = s_rtc.now();
    // ★runId を RTC の時刻で確定させる。
    //   起動直後の micros() はどの起動でもほぼ同じ値になるため、それだけでは
    //   runId が衝突する（実機で A61EECA9 が4回重複し、起動の区別がつかなかった）。
    //   RTC の epoch は起動ごとに必ず違うので、これを混ぜれば確実に分かれる。
    s_runId ^= now.unixtime() * 2654435761UL;
    Serial.print(F(" RTC          : "));
    Serial.print(now.year()); Serial.print('-'); Serial.print(now.month());
    Serial.print('-'); Serial.print(now.day()); Serial.print(' ');
    Serial.print(now.hour()); Serial.print(':'); Serial.println(now.minute());
  } else {
    Serial.println(F(" RTC          : ✗ 見つかりません（計測日時は空欄になります）"));
  }

  logEvent("起動 reset=" + String(resetReasonName(s_bootResetReason)) + " FW=" + String(GATEWAY_FW_VERSION) + " run=" + String(s_runId, HEX));

  // LoRa
  Serial.println(F("\n--- LoRa 初期化 ---"));
  pinMode(LORA_M0M1_PIN, OUTPUT);
  loraSerial.begin(9600);
  delay(500);
  loraCheckAndConfigure();
  s_loraLastRxMs = millis();

  // LTE-M
  Serial.println(F("\n--- LTE-M 初期化 ---"));
  delay(3000);
  if (!probeAT()) {
    Serial.println(F("SIM7080G に応答がありません。配線・電源を確認してください。"));
    lteFailRecover(false);   // ★FW22: 以前は return して、アプリ層WDTが切れるまで LTE なしのままだった
  }
  if (!lteConnect()) {
    Serial.println(F("LTE-M 接続に失敗しました。"));
    lteFailRecover(true);    // ★FW22
  }
  // ★2026-09-23 FW15: LTE 接続直後、電池切れ・古い RTC のときだけ起動時補正。
  s_lastRtcSyncMs = millis();
  if (s_rtcAvailable && (s_rtc.lostPower() || s_rtc.now().year() < 2026)) syncRtcFromNetworkTime();
  else Serial.println(s_rtcAvailable ? F("[RTC] 起動時補正不要") : F("[RTC] 補正不可: DS3231なし"));
  s_lastCsq = readCsq();

  // runId を確定（LTE 登録にかかった時間のばらつきを混ぜる）
  s_runId ^= (uint32_t)micros() * 2246822519UL;
  printRunId();
  char runHex[9];
  snprintf(runHex, sizeof(runHex), "%08lX", (unsigned long)s_runId);
  logEvent("起動 reset=" + String(resetReasonName(s_bootResetReason)) + " FW=" + String(GATEWAY_FW_VERSION) + " run=" + String(runHex));

  s_lastSendMs = millis();
  s_lastCloudSuccessMs = millis();   // アプリ層WDTの起点
  // ★FW23: 起動ログ（網時刻で補正したあとの RTC を載せる。URL に入れるので空白は _ にする）
  if (s_rtcAvailable) {
    DateTime now = s_rtc.now();
    char tb[24];
    snprintf(tb, sizeof(tb), "%04u-%02u-%02u_%02u:%02u:%02u",
             now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
    s_bootRtc = String(tb);
  }
#if SEND_TO_GAS || ENABLE_DOWNLINK
  postBootInfo();
#endif
  Serial.println(F("\n受信待機を開始します。"));
}

void loop() {
  wdtFeed();
  loraPoll();
  loraRxWatchdog();
  appWatchdogCheck();
  // ★2026-09-23 FW15: millis の差分で24時間周期を管理し、失敗時も連続問い合わせを避ける。
  if (millis() - s_lastRtcSyncMs >= 24UL * 60UL * 60UL * 1000UL) syncRtcFromNetworkTime();

  if (millis() - s_lastSendMs < s_sendIntervalMs) {
    delay(10);
    return;
  }
  s_lastSendMs = millis();
  s_cycle++;

  Serial.print(F("\n╭──────── 送信サイクル ")); Serial.print(s_cycle);
  Serial.print(F("  (")); Serial.print(millis() / 1000UL); Serial.println(F("秒) ────────"));

  if (s_sendPaused) {
    Serial.print(F("  ⏸ 送信停止中（バッファ ")); Serial.print(s_recordCount); Serial.println(F(" 件を保持）"));
  } else flushRecords();
#if SEND_TO_GAS || ENABLE_DOWNLINK
  if (s_bootInfoPending) postBootInfo();   // ★FW23: 起動時に記録できなかった起動ログを送り直す
#endif

#if ENABLE_DOWNLINK
  // ★2026-09-23 FW14: ダウンリンク開始前の待機でもLoRa受信を継続。
  waitWithLora(1000);
  Serial.println(F("  ── ダウンリンク予約の確認 ──"));
  // ★ダウンリンクの取得成功も「クラウドに到達できている」証拠なので、
  //   アプリ層WDTの生存判定に含める。テレメトリを送る相手が居ないサイクルでも
  //   ここが通っていれば通信は生きている。
  // ★2026-09-23 FW15: 報告（送信済み・結果）を先に GAS へ届けてから予約を取り直す。
  //   逆順だと、子機が適用済みの予約を GAS がまだ「送信済み」と見ている古い一覧を
  //   取得してしまい、1サイクル分の古い状態で動くことになる。
  //   報告は loop() の送信サイクル内でのみ行う（loraPoll() からは呼ばない）。
  processReportQueue();
  if (fetchDownlink()) { s_dlOk++; s_lastCloudSuccessMs = millis(); }
  else                 { s_dlNg++; }
#endif

  printSummary();
}
