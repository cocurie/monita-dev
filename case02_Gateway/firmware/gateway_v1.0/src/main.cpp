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
const char* GAS_SCRIPT_ID = "AKfycbywRcyl3059evcw-kFo9ypeejbhZWRyY9rILX9TUjlEWJ-4K2nGkZqIrZymA9cYGZ8maQ/exec";

// LTE-M送信のON/OFF切替（★2026-07-23追加）
// false にすると SIM7080G の初期化・ネットワーク接続・GAS送信を一切行わず、
// BLE受信データを直接SDカードへ記録するだけの「SD記録のみモード」になる。
// SIM7080GのTX系統故障が疑われる現場での暫定運用（アンテナ交換ができない場合等）を想定。
#define LTEM_SEND_ENABLED false

// SD記録のみモード（LTEM_SEND_ENABLED=false）でのSDカード記録インターバル（ms）★ここを変える
// 受信のたびに書くとSDの書き込み回数・ファイルサイズが増えるため、この間隔ごとに
// 「その時点で受信済みの全子機の最新データ」をまとめて1回記録する（LTE-M送信時と同じ考え方）。
static uint32_t const SD_LOG_INTERVAL_MS = 300000;  // 既定 5 分

// 起動確認送信 — true にするとネットワーク接続直後に、機器の設定情報の行と、
// それまでに BLE スキャンで受信できていた実際の子機データ（実 RSSI 含む）を
// GAS へ送信する（通信経路とアンテナ状況を起動のたびに確認できる）
#define BOOT_SCAN_SEND true

// 再送キュー・即時リトライの動作確認用: 送信を N 回だけ強制的に失敗させる
// （実際の通信は行わず即座に失敗を返すため、タイムアウト待ちなしで検証できる）
// 0 = 無効（通常運用時は必ず 0 に戻すこと）
#define TEST_FORCE_SEND_FAIL 0

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
// Gateway は USB-C 常時給電のため省電力を気にせずデューティ比をほぼ100%にする
// （interval と window をほぼ同値にすることでほぼ常時受信状態にし、取りこぼしを減らす）
static uint16_t const SCAN_INTERVAL_MS   = 100;     // スキャンインターバル (ms)
static uint16_t const SCAN_WINDOW_MS     = 100;      // スキャンウィンドウ (ms)
static uint32_t const SEND_INTERVAL_MS   = 300000;  // GAS 送信インターバル (ms) ★ここを変える
static uint8_t  const MFR_COMPANY_ID_H   = 0xFF;    // Flex の Company ID (上位)
static uint8_t  const MFR_COMPANY_ID_L   = 0xFF;    // Flex の Company ID (下位)

// Company ID 0xFFFF は Bluetooth 仕様上「未登録・テスト用」の予約値のため、
// 近隣の無関係な BLE 機器（他社のテスト機器等）が偶然同じ ID で
// Manufacturer Data を送信していると誤って拾ってしまうことがある。
// そのため Flex の MSD フォーマット（Pkt type・Device ID）でも二重に検証する。
static uint8_t  const EXPECTED_PKT_TYPE  = 0x03;             // Flex v3.03 の Pkt type
static uint8_t  const ALLOWED_DEVICE_IDS[] = {0x01, 0x02};   // ★子機を増やしたらここに追加する
static size_t   const ALLOWED_DEVICE_IDS_COUNT = sizeof(ALLOWED_DEVICE_IDS) / sizeof(ALLOWED_DEVICE_IDS[0]);

// pktType・deviceId が Flex として許可された組み合わせか判定する
bool isAllowedFlexPacket(uint8_t pktType, uint8_t deviceId) {
  if (pktType != EXPECTED_PKT_TYPE) return false;
  for (size_t i = 0; i < ALLOWED_DEVICE_IDS_COUNT; i++) {
    if (deviceId == ALLOWED_DEVICE_IDS[i]) return true;
  }
  return false;
}

// SD カード
static int const SD_CS_PIN = 3;  // D3

// ══════════════════════════════════════════════
// BLE 受信バッファ
// ══════════════════════════════════════════════
#define MAX_DEVICES 20
// Flex v3.03（COMM_MODE_BLE）の MSD は19バイト（PktType+DeviceID+FWVersion+CH1-4+BATT+Hour+Min+CH1-4Range）。
// 16バイトのままだと末尾のCH2〜CH4レンジが切り捨てられるため24に拡張（将来の拡張余地も確保）。
#define MAX_PAYLOAD 24

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

// 送信失敗時に保持する再送キュー（次回まとめてライブデータとマージして再送する）
static FlexRecord pendingRecords[MAX_DEVICES];
static int        pendingCount = 0;

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
// ウォッチドッグタイマー（nRF52840 内蔵 WDT）
// 無人運用中にファームがハングした場合、自動リセットで復旧するための安全網。
// 一度 START すると停止不可（電源再投入かリセットまで動作し続ける）。
// ══════════════════════════════════════════════
static uint32_t const WDT_TIMEOUT_MS = 120000UL;  // 120秒: この間キックが無ければリセット

static void wdtInit(uint32_t timeoutMs) {
  NRF_WDT->CONFIG  = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);  // スリープ中も継続動作
  NRF_WDT->CRV     = (uint32_t)((uint64_t)timeoutMs * 32768ULL / 1000ULL);  // 32768Hz ティック換算
  NRF_WDT->RREN    = WDT_RREN_RR0_Msk;  // チャンネル0のみ使用
  NRF_WDT->TASKS_START = 1;
}

static inline void wdtFeed() {
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;  // キック（既定のリロードマジック値）
}

// ══════════════════════════════════════════════
// アプリ層ウォッチドッグ（★2026-07-21 追加）
//
// ハードWDT（上記）は「wdtFeedが完全に止まる＝MCUフリーズ」した時だけ復旧する。
// しかし有野川現場（2026-07-17〜停止）で発生した障害は、モデムはネットワークに接続した
// まま、GASへの送信（SHCONN/SHREQ）だけが失敗し続け、ファームは送信リトライのループを
// 回して wdtFeed を呼び続ける「ソフトハング」だった。この場合ハードWDTには餌が入り続けて
// リセットがかからず、無人現場では復旧不能になった（SIMはオンラインのままだが送信されない）。
//
// 対策として「一定時間 GAS 送信が1回も成功しなかったら NVIC_SystemReset で強制再起動」する。
// 再起動後は setup() が AT&F + CFUN=1,1 でモデムもソフトリセットするため、モデム側スタックの
// 詰まりも合わせて解消される。
// ══════════════════════════════════════════════
static uint32_t const APP_WDT_NO_SEND_RESET_MS = 1800000UL;  // 30分: この間GAS送信成功が無ければ再起動
static uint32_t lastGasSuccessMs = 0;  // 最後にGAS送信が成功した millis()（setup先頭で初期化）

// 段階的復旧（★2026-07-21 追加、有野川障害の教訓）:
// アプリWDTの「30分無送信で全再起動」の前に、より軽く速い一段目として
// 「送信が規定回数連続で失敗したら、モデムだけソフトリセット(CFUN=0/1)して再接続」を挟む。
// 有野川で疑われたモデムのSSL/HTTPスタック固着（CGATT正常＝アタッチ維持のまま送信だけ失敗）は
// setup()でしか実行されないモデムリセットに降りていけず永続化した。送信失敗時にもモデムリセットへ
// 降りられるようにする。これでも復旧しなければ30分の全再起動が最終backstop。
static int const MODEM_RESET_FAIL_THRESHOLD = 3;  // 連続失敗した送信サイクル数（5分間隔なら約15分）
static int consecutiveSendFailures = 0;

// ══════════════════════════════════════════════
// アンテナ未接続保護（★2026-07-23 追加、有野川でのSIM7080G故障を受けて）
//
// 【経緯】有野川ではアンテナ接続が不良な状態のまま、ファームが登録要求＝送信を数日間
// 繰り返した。その後モジュールは通信不能になり、別基板・別アンテナに載せ替えても復旧
// しなかったためモジュール故障と判定した。
// 症状（CSQは正常値・網登録不成立・網側にイベント痕跡なし・CEERに拒否理由なし）から
// 送信系の異常が疑われるが、★故障部位も原因も特定できていない（ESD等の可能性も残る）。
//
// 【対策の根拠】信号を全く検出できない状態で登録要求を繰り返しても成功する見込みはなく、
// 電力とデータを消費するだけで無意味である。加えて、不整合な負荷への送信継続がハードへ
// 与える影響も避けたい（機序は未確認）。よって CSQ=99（圏外またはアンテナ未接続）が
// 連続で規定回数観測されたら AT+CFUN=0 でRFを停止する。
// クールダウン後に AT+CFUN=1 で復帰して再試行するので、一時的な圏外なら自動回復する。
// ══════════════════════════════════════════════
static int      const RF_PROTECT_CSQ99_THRESHOLD = 3;        // CSQ=99 が連続この回数で保護発動
static uint32_t const RF_PROTECT_COOLDOWN_MS     = 1800000UL; // 30分 RF停止して待機
static int      s_csq99Count       = 0;
static bool     s_rfProtected      = false;
static uint32_t s_rfProtectStartMs = 0;

String sendAT(String cmd, int waitMs);  // 前方宣言（RF保護から使用）

// RF保護中かどうかを返す。クールダウンが明けていればRFを復帰させて false を返す。
// true の間は一切送信してはならない。
static bool rfProtectActive() {
  if (!s_rfProtected) return false;
  if (millis() - s_rfProtectStartMs >= RF_PROTECT_COOLDOWN_MS) {
    Serial.println(F("[RF保護] クールダウン終了 → AT+CFUN=1 でRFを再開し再試行します"));
    sendAT("AT+CFUN=1", 5000);
    delay(5000);
    s_rfProtected = false;
    s_csq99Count  = 0;
    return false;
  }
  return true;
}

// CSQ値を観測し、信号が全く無い状態が続いたらRFを停止する。
// csq: 0-31 = 受信強度、99 または負値 = 信号検出不可
static void rfProtectObserveCsq(int csq) {
  if (s_rfProtected) return;  // 保護中はRF停止のためCSQ=99が当然。判定しない

  if (csq == 99 || csq < 0) {
    s_csq99Count++;
    Serial.print(F("[RF保護] 信号検出不可(CSQ=99) 連続 "));
    Serial.print(s_csq99Count); Serial.print(F("/"));
    Serial.println(RF_PROTECT_CSQ99_THRESHOLD);

    if (s_csq99Count >= RF_PROTECT_CSQ99_THRESHOLD) {
      Serial.println(F("\n‼ [RF保護] 信号を全く検出できません（アンテナ未接続・接続不良の可能性）"));
      Serial.println(F("   送信しても成功する見込みが無いため AT+CFUN=0 でRFを停止します。"));
      Serial.println(F("   アンテナ・ケーブル・コネクタを確認してください。"));
      sendAT("AT+CFUN=0", 5000);
      delay(1000);
      s_rfProtected      = true;
      s_rfProtectStartMs = millis();
    }
  } else {
    s_csq99Count = 0;  // 信号を検出できたのでカウンタをクリア
  }
}

static void appWatchdogCheck() {
  if (millis() - lastGasSuccessMs >= APP_WDT_NO_SEND_RESET_MS) {
    Serial.println(F("\n‼ アプリWDT: 規定時間 GAS 送信成功なし → NVIC_SystemReset で強制再起動"));
    Serial.flush();
    delay(200);
    NVIC_SystemReset();  // setup() から全再初期化（モデムも AT&F + CFUN=1,1 でリセットされる）
  }
}

// ══════════════════════════════════════════════
// AT コマンド送受信
// ══════════════════════════════════════════════
// 応答バッファの上限（★2026-07-21）: 配線ノイズ・SIM7080Gの異常URC等でRX1に
// ゴミデータが流れ込み続けた場合、上限が無いと res が際限なく肥大化してヒープを
// 食い尽くす（AT+SHREQ=60秒・AT+COPS=?=180秒待機時に特にリスクが高い）。
// 電波状況の良否とは無関係に発生しうるMCUハングの一因と推定されるため、
// 上限超過分は読み捨てて（HWバッファは溢れさせない）ヒープ確保量を頭打ちにする。
static uint16_t const SENDAT_MAX_RESPONSE_LEN = 2048;

String sendAT(String cmd, int waitMs = 5000) {
  DLOG2("--");
  DLOGV2(">> ", cmd);
  Serial1.print(cmd + "\r\n");
  long start = millis();
  String res = "";
  while (millis() - start < waitMs) {
    wdtFeed();  // 長時間の AT 応答待ち（COPS スキャン等 最大3分）でもハング扱いされないよう給餌
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      if (res.length() < SENDAT_MAX_RESPONSE_LEN) res += c;
    }
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
  // ★2026-07-23: RF保護中（アンテナ未接続と判断してCFUN=0中）は送信を一切行わない
  if (rfProtectActive()) {
    Serial.println(F("[RF保護] RF停止中のためネットワーク初期化をスキップします（アンテナ確認要）"));
    return false;
  }

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

    // ★2026-07-23: 信号を全く検出できない状態が続くならRFを停止する（無意味な送信の停止）
    rfProtectObserveCsq(csqVal);
    if (rfProtectActive()) {
      Serial.println(F("[RF保護] RFを停止しました。今回のネットワーク初期化を中断します"));
      return false;
    }

    // 詳細ネットワーク状態
    String cpsi = sendAT("AT+CPSI?", 3000);
    Serial.print(F("  CPSI: "));
    if (cpsi.indexOf("NO SERVICE") >= 0) Serial.println(F("NO SERVICE → 電波なし / アンテナ未接続"));
    else {
      int pi = cpsi.indexOf("+CPSI:");
      Serial.println(pi >= 0 ? cpsi.substring(pi) : cpsi);
    }

    // 拡張エラーレポート（★2026-07-23追加）: CSQ良好・COPSでキャリアが見えている
    // (stat=1=利用可能)のにCREGが進まない場合、ネットワーク側のアタッチ拒否理由
    // （Illegal MS / Roaming not allowed / PLMN not allowed 等）をここで特定できることがある
    String ceer = sendAT("AT+CEER", 3000);
    Serial.print(F("  CEER(拒否理由): "));
    int ceerIdx = ceer.indexOf("+CEER:");
    Serial.println(ceerIdx >= 0 ? ceer.substring(ceerIdx) : F("(応答なし)"));

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
    String ceer = sendAT("AT+CEER", 3000);
    Serial.print(F("  CEER(拒否理由): "));
    int ceerIdx = ceer.indexOf("+CEER:");
    Serial.println(ceerIdx >= 0 ? ceer.substring(ceerIdx) : F("(応答なし)"));
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
#if TEST_FORCE_SEND_FAIL > 0
  static int s_forceFailRemaining = TEST_FORCE_SEND_FAIL;
  if (s_forceFailRemaining > 0) {
    s_forceFailRemaining--;
    Serial.print(F("[TEST] 強制送信失敗（実通信スキップ）残り "));
    Serial.print(s_forceFailRemaining);
    Serial.println(F(" 回"));
    return false;
  }
#endif

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
    lastGasSuccessMs = millis();  // アプリ層ウォッチドッグ: 送信成功を記録
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
  // MSD フォーマット: msd[0]=Pkt type, msd[1]=Device ID
  uint8_t pktType  = (msdLen >= 1) ? msd[0] : 0xFF;
  uint8_t deviceId = (msdLen >= 2) ? msd[1] : 0xFF;

  // Company ID だけでは無関係な BLE 機器を誤検出することがあるため、
  // Pkt type・Device ID がホワイトリストに一致するものだけを Flex とみなす
  if (!isAllowedFlexPacket(pktType, deviceId)) {
    DLOG2("[BLE] 未登録の Company ID 0xFFFF パケットを無視（Flex 以外の可能性）");
    Bluefruit.Scanner.resume();
    return;
  }

  Serial.print(F("[BLE] MSD パケット受信 Device ID=0x"));
  if (deviceId < 0x10) Serial.print('0');
  Serial.println(deviceId, HEX);

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

// ネットワーク登録状態を簡易確認し、切れていれば再接続する
// （長時間運用中に基地局都合で接続が切れるケースへの対策）
bool ensureNetworkReady() {
  String att = sendAT("AT+CGATT?", 3000);
  if (att.indexOf("+CGATT: 1") >= 0) return true;

  Serial.println(F("⚠ ネットワーク切断を検知。再接続を試みます..."));
  return initNetwork();
}

// モデムのソフトリセット（★2026-07-21）:
// 送信が連続で全滅した際、CGATTがOKでも上位(SSL/HTTP/PDP)が固着している可能性があるため、
// 無線を一旦落として(CFUN=0/1)から再登録・再接続する。全再起動より軽く速い一段目の復旧手段。
bool modemSoftReset() {
  Serial.println(F("\n⚠ 送信が連続失敗 → モデムをソフトリセット(CFUN=0/1)して再接続します"));
  sendAT("AT+CFUN=0", 5000); delay(2000);
  sendAT("AT+CFUN=1", 5000); delay(5000);
  sendAT("AT+CNMP=38", 2000); delay(500);  // LTE only
  sendAT("AT+CMNB=1",  2000); delay(500);  // Cat-M1
  bool ok = initNetwork();                 // CREG/CGATT/CNACT を張り直す
  Serial.println(ok ? F("✓ モデムソフトリセット後 再接続成功")
                    : F("✗ ソフトリセット後も再接続失敗（30分の全再起動backstopに委ねる）"));
  return ok;
}

// SIM7080G（LTE-M モデム）自身の受信電波強度を取得する
// 戻り値: 0-31（値が大きいほど良好）、99=圏外/取得失敗
int getSimCsq() {
  String csq = sendAT("AT+CSQ", 3000);
  int idx = csq.indexOf("+CSQ: ");
  if (idx < 0) return 99;
  return csq.substring(idx + 6, csq.indexOf(",", idx)).toInt();
}

// XIAO nRF52840 固有の Device ID（工場設定レジスタ FICR、64bit）を16進文字列で返す
String getXiaoId() {
  char buf[17];
  snprintf(buf, sizeof(buf), "%08lX%08lX",
           (unsigned long)NRF_FICR->DEVICEID[1],
           (unsigned long)NRF_FICR->DEVICEID[0]);
  return String(buf);
}

// SIM7080G の IMEI（15桁の一意な番号）を取得する
String getSimImei() {
  String res = sendAT("AT+GSN", 3000);
  for (int i = 0; i < (int)res.length(); i++) {
    if (isDigit(res[i])) {
      int j = i;
      while (j < (int)res.length() && isDigit(res[j])) j++;
      if (j - i >= 10) return res.substring(i, j);  // IMEI は15桁程度の連続した数字
      i = j;
    }
  }
  return "";
}

// 起動確認送信: 機器の設定情報を1行だけ GAS へ送る（子機データとは別行）
void postBootInfoRow() {
  Serial.println(F("--- 起動情報送信 ---"));
  String xiaoId  = getXiaoId();
  String simImei = getSimImei();
  int    csq     = getSimCsq();

  String params = "ts=";
  params += getTimestamp();
  params += "&sim=";
  params += SIM_NAME;
  params += "&csq=";
  params += String(csq);
  params += "&row_type=info";
  params += "&xiao_id=";
  params += xiaoId;
  params += "&sim_imei=";
  params += simImei;
  params += "&sd=";
  params += (sdAvailable ? "1" : "0");
  params += "&interval_min=";
  params += String(SEND_INTERVAL_MS / 60000UL);
  params += "&devcount=";
  params += String(recordCount);

  postToGAS(params);
}

// ══════════════════════════════════════════════
// SD記録のみモード（LTEM_SEND_ENABLED=false）用のSD書き出し（★2026-07-23追加）
//
// SD_LOG_INTERVAL_MS ごとに呼ばれ、その時点で受信済みの全子機の最新データを
// まとめてSDへ記録し、バッファをクリアする。LTE-M送信は一切行わない。
// ══════════════════════════════════════════════
#if !LTEM_SEND_ENABLED
void flushRecordsToSdOnly() {
  if (xSemaphoreTake(recordMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

  int n = recordCount;
  FlexRecord snap[MAX_DEVICES];
  if (n > 0) memcpy(snap, records, sizeof(FlexRecord) * n);
  recordCount = 0;

  xSemaphoreGive(recordMutex);

  if (n == 0) { Serial.println(F("[SD] 記録対象レコードなし")); return; }

  String ts = getTimestamp();
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
    sdLog(ts + "," + String(mac) + "," + hex + "," + String(snap[i].rssi));
  }
  Serial.print(F("[SD] ")); Serial.print(n); Serial.println(F(" 件を記録しました"));
}
#endif

// ══════════════════════════════════════════════
// バッファを GAS へ送信＆SD へ記録（全台を1回の POST にまとめる）
//
// 送信失敗時は再送キュー（pendingRecords）に保持し、次回サイクルで
// ライブ受信データとマージして再送する（同一 MAC はライブ側を優先）。
// これにより一時的な通信断でデータをロストしない。
// ══════════════════════════════════════════════
void flushRecords() {
  if (xSemaphoreTake(recordMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

  int liveN = recordCount;
  FlexRecord liveSnap[MAX_DEVICES];
  memcpy(liveSnap, records, sizeof(FlexRecord) * liveN);
  recordCount = 0;

  xSemaphoreGive(recordMutex);

  // ライブデータ＋再送キューをマージ（同一 MAC はライブ側＝最新を優先）
  FlexRecord merged[MAX_DEVICES];
  int n = 0;
  for (int i = 0; i < liveN && n < MAX_DEVICES; i++) merged[n++] = liveSnap[i];

  int pendingSpaceDropped = 0;  // バッファ上限で本当に破棄された件数のみカウント
  for (int i = 0; i < pendingCount; i++) {
    bool dup = false;
    for (int j = 0; j < liveN; j++) {
      if (memcmp(pendingRecords[i].mac, liveSnap[j].mac, 6) == 0) { dup = true; break; }
    }
    if (dup) continue;  // ライブ側に同一 MAC の新しいデータがあるので再送キュー側は不要（正常な重複排除）
    if (n < MAX_DEVICES) merged[n++] = pendingRecords[i];
    else pendingSpaceDropped++;
  }
  pendingCount = 0;

  if (n == 0) { Serial.println(F("送信対象レコードなし")); return; }
  if (pendingSpaceDropped > 0) {
    Serial.print(F("  → 再送キュー ")); Serial.print(pendingSpaceDropped);
    Serial.println(F(" 件はバッファ上限のため破棄"));
  }

  String ts = getTimestamp();
  Serial.print(F("フラッシュ: ")); Serial.print(n); Serial.println(F(" 件"));

  // ★2026-07-23: SD記録は LTE-M の状態に関わらず、送信を試みる前に必ず実行する。
  // 旧実装は下の ensureNetworkReady() 失敗時に early return しており、LTE-Mが停止すると
  // SDにも一切残らずデータが完全に失われていた（有野川の教訓）。
  // 記録対象はライブ受信分のみ（再送キュー分は前回のサイクルで記録済みのため重複させない）。
  for (int i = 0; i < liveN; i++) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X-%02X-%02X-%02X-%02X-%02X",
             liveSnap[i].mac[5], liveSnap[i].mac[4], liveSnap[i].mac[3],
             liveSnap[i].mac[2], liveSnap[i].mac[1], liveSnap[i].mac[0]);
    String hex = "";
    for (int j = 0; j < liveSnap[i].payloadLen; j++) {
      if (liveSnap[i].payload[j] < 0x10) hex += '0';
      hex += String(liveSnap[i].payload[j], HEX);
    }
    sdLog(ts + "," + String(mac) + "," + hex + "," + String(liveSnap[i].rssi));
  }
  if (liveN > 0) { Serial.print(F("[SD] ")); Serial.print(liveN); Serial.println(F(" 件を記録")); }

  // ネットワーク状態確認・必要なら再接続
  if (!ensureNetworkReady()) {
    Serial.println(F("✗ ネットワーク再接続失敗（SD記録は完了済み）。今回分は再送キューへ保留"));
    pendingCount = n;
    memcpy(pendingRecords, merged, sizeof(FlexRecord) * n);
    return;
  }

  // SIM7080G 自身のセルラー受信電波強度（BLE RSSI とは別物）
  int csq = getSimCsq();

  // クエリ文字列を1本に組み立てる
  // 形式: ts=...&sim=...&csq=...&n=3&mac0=...&payload0=...&rssi0=...&mac1=...
  String params = "ts=";
  params += ts;
  params += "&sim=";
  params += SIM_NAME;
  params += "&csq=";
  params += String(csq);
  params += "&n=";
  params += String(n);

  for (int i = 0; i < n; i++) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X-%02X-%02X-%02X-%02X-%02X",
             merged[i].mac[5], merged[i].mac[4], merged[i].mac[3],
             merged[i].mac[2], merged[i].mac[1], merged[i].mac[0]);

    String hex = "";
    for (int j = 0; j < merged[i].payloadLen; j++) {
      if (merged[i].payload[j] < 0x10) hex += '0';
      hex += String(merged[i].payload[j], HEX);
    }

    params += "&m"; params += i; params += "="; params += mac;
    params += "&p"; params += i; params += "="; params += hex;
    params += "&r"; params += i; params += "="; params += String(merged[i].rssi);
    // ※ SD記録はネットワーク確認より前に実施済み（LTE-M停止時もデータを残すため）
  }

  bool ok = postToGAS(params);

  if (!ok) {
    // 一時的な通信不良を想定し、30秒待って1回だけ即時リトライ
    Serial.println(F("✗ 送信失敗。30秒後に再試行..."));
    delay(30000);
    ok = postToGAS(params);
  }

  if (ok) {
    Serial.println(F("✓ 送信成功（再送キュー クリア）"));
    consecutiveSendFailures = 0;
  } else {
    Serial.println(F("✗ 再試行も失敗。次回送信サイクルで再送キューとしてリトライします"));
    pendingCount = n;
    memcpy(pendingRecords, merged, sizeof(FlexRecord) * n);

    // 段階的復旧: 連続失敗をカウントし、閾値でモデムをソフトリセット
    consecutiveSendFailures++;
    Serial.print(F("連続送信失敗: ")); Serial.print(consecutiveSendFailures);
    Serial.print(F("/")); Serial.println(MODEM_RESET_FAIL_THRESHOLD);
    if (consecutiveSendFailures >= MODEM_RESET_FAIL_THRESHOLD) {
      modemSoftReset();
      consecutiveSendFailures = 0;  // リセットを試みたので一旦クリア
    }
  }
}

static uint32_t lastSend = 0;

// ══════════════════════════════════════════════
// setup
// ══════════════════════════════════════════════
void setup() {
  wdtInit(WDT_TIMEOUT_MS);  // 無人運用の安全網。以降 120 秒キックが無ければ自動リセット
  lastGasSuccessMs = millis();  // アプリ層WDTの起点。以降 APP_WDT_NO_SEND_RESET_MS 内に送信成功が無ければ再起動

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
  Serial.println(F("✓ BLE 初期化完了"));

#if !LTEM_SEND_ENABLED
  // ★2026-07-23: LTE-M送信無効（SD記録のみモード）。SIM7080Gの初期化・ネットワーク接続は
  // 一切行わず、BLE受信データを scanCallback() 内で直接SDへ記録する。
  Serial.println(F("\n△ LTEM_SEND_ENABLED=false: SIM7080G初期化・LTE-M送信をスキップ（SD記録のみモード）"));
  Bluefruit.Scanner.start(0);
  Serial.println(F("✓ BLE スキャン開始（SD記録のみモード）"));
#else
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

  // SIM7080G 起動待ちが終わった時点で BLE スキャンを開始する。
  // UART（Serial1, SIM7080G通信）と BLE 無線はハードウェア的に独立しているため、
  // STAGE3〜6（AT疎通確認〜ネットワーク接続）の間もスキャンを継続してよい。
  // これにより起動確認送信までに子機データがより多く貯まる。
  Bluefruit.Scanner.start(0);
  Serial.println(F("✓ BLE スキャン開始"));

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

#if BOOT_SCAN_SEND
  if (netOk) {
    Serial.println(F("===== 起動確認: 設定情報＋受信済み子機データを送信 ====="));
    postBootInfoRow();
    flushRecords();
    lastSend = millis();  // 次の定期送信サイクルはここから起算する
    Serial.println(F("=======================================================\n"));
  }
#endif
#endif  // LTEM_SEND_ENABLED
}

// ══════════════════════════════════════════════
// loop
// ══════════════════════════════════════════════
static uint32_t lastHeartbeat = 0;

void loop() {
  wdtFeed();          // BLE スキャンのみで sendAT が呼ばれない期間もハング扱いされないよう給餌
#if LTEM_SEND_ENABLED
  appWatchdogCheck();  // 一定時間 GAS 送信成功が無ければ強制再起動（ソフトハング対策、有野川現場の教訓）
#endif
  uint32_t now = millis();

  // デバッグ心拍: 10秒ごとに状態を表示
  if (now - lastHeartbeat >= 10000) {
    lastHeartbeat = now;
#if LTEM_SEND_ENABLED
    Serial.print(F("[HB] now=")); Serial.print(now);
    Serial.print(F(" lastSend=")); Serial.print(lastSend);
    Serial.print(F(" 残り=")); Serial.print((long)(SEND_INTERVAL_MS - (now - lastSend)));
    Serial.print(F("ms 受信台数=")); Serial.println(recordCount);
#else
    Serial.print(F("[HB/SDのみ] now=")); Serial.print(now);
    Serial.print(F(" 次回SD記録まで=")); Serial.print((long)(SD_LOG_INTERVAL_MS - (now - lastSend)));
    Serial.print(F("ms 受信台数=")); Serial.print(recordCount);
    Serial.print(F(" SD=")); Serial.println(sdAvailable ? F("OK") : F("NG"));
#endif
  }

#if LTEM_SEND_ENABLED
  if (now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;
    Serial.println(F("\n=== 定期送信 ==="));
    Serial.print(F("時刻: ")); Serial.println(getTimestamp());
    Serial.print(F("受信済み Flex 台数: ")); Serial.println(recordCount);

    // 送信中は BLE スキャンを停止（LTE-M 通信中の割り込み負荷を減らす）
    Bluefruit.Scanner.stop();
    flushRecords();
    Bluefruit.Scanner.start(0);
  }

  // 手動 AT コマンドモード（シリアルから入力）
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) sendAT(line);
  }
  while (Serial1.available()) Serial.write(Serial1.read());
#else
  // SD記録のみモード: SD_LOG_INTERVAL_MS ごとに受信済みデータをまとめてSDへ記録
  if (now - lastSend >= SD_LOG_INTERVAL_MS) {
    lastSend = now;
    Serial.println(F("\n=== SD記録 ==="));
    Serial.print(F("時刻: ")); Serial.println(getTimestamp());
    Serial.print(F("受信済み Flex 台数: ")); Serial.println(recordCount);
    flushRecordsToSdOnly();
  }
#endif  // LTEM_SEND_ENABLED
}
