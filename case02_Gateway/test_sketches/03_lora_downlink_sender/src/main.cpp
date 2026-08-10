/**
 * Monita LoRa 検証 Step03（Gateway側）: ダウンリンク送信テストツール（Gateway v1.1実機版）
 *
 * 【目的】
 *   case01_Flex/test_sketches/25_lora_downlink_child（Flex実機）が正しく
 *   ダウンリンクを受信・検証・適用できるかを確認するための送信専用ツール。
 *   Gateway v1.1実機（XIAO nRF52840 + E220-900T22S(JP) + SIM7080G）にそのまま書き込んで使う。
 *
 * 【★2026-08-08: スプレッドシート駆動に変更 / 08-09: Class A + 確認応答方式へ改訂】
 *   「スプレッドシートで子機に指示→Gateway→LoRa経由で子機の設定変更」の一連の流れを
 *   実機で検証するためのツール。SIM7080G初期化・AT通信層はgateway_v1.1から流用している。
 *
 * 【方式: Class A + 確認応答】
 *   子機は省電力のため常時受信できず、自分のアップリンク送信直後の2秒間しか受信窓を
 *   開けない。その2秒でGASへHTTPS問い合わせ（数秒〜数十秒）は不可能なので:
 *
 *     1. Gatewayは30秒おきにGASの `action=check_downlinks` を叩き、子機宛ての予約を
 *        「ローカルキャッシュ」に持っておく（s_pending[]）
 *     2. LoRaを常時受信し、子機のアップリンク(PktType 0x04)を検知したその瞬間に、
 *        キャッシュを照合してネットワークを介さず即座にダウンリンクを送る（往復100〜300ms）
 *     3. 送信しただけでは予約を消さない。子機からの確認フレーム(PktType 0x05)を受けて
 *        初めて完了とし、結果(成功/値域エラー/クランプ)をGASへ報告する
 *     4. DOWNLINK_MAX_ATTEMPTS 回送っても確認が返らなければ「未達(status=99)」として
 *        GASへ報告し予約を打ち切る
 *
 *   ★この「確認が取れるまで再試行する」意味論が肝。送りっぱなしだと、Gatewayが
 *     裸のdelay()で塞がっていた・電波が悪かった等の一時的な理由でコマンドが黙って
 *     消えてしまう。再試行前提にしたことで、取りこぼしは「1サイクル遅れる」だけになる。
 *   ★同じ理由で、AT応答待ちや長い待機中もloraPoll()を回し続ける（sendAT/sendATFull/
 *     loraDelay）。子機の窓は2秒しかないため、ここで止まると応答が間に合わない。
 *
 * 【GAS通信はAT+HTTPTOFS方式（★2026-08-10全面移行）】
 *   AT+SH*系（AT+SHCONN/SHREQ/SHREAD）はGASの302応答を安定して処理できないことが
 *   実機とcurlでの実測で確定したため使用しない。詳細はhttpGetViaFs()の先頭コメント参照。
 *   AT+SH*系とAT+HTTPTOFSは併用不可（混ぜるとAT+SHCONNがERRORになる）。
 *
 *   本番のgateway_v11_test/v3.10_loraとはGW_DEVICE_IDが別なので、通常運用の
 *   Gatewayオペレーションには一切影響しない。
 *
 * 【配線】Gateway v1.1実機そのまま（platformio.iniのコメント参照）
 *   XIAO D0 ← E220 TXD / XIAO D1 → E220 RXD / XIAO D2 → E220 M0・M1共通駆動
 *   XIAO D6 → SIM7080G RX / XIAO D7 ← SIM7080G TX（gateway_v1.1と同一）
 *
 * 【フレーム仕様】25_lora_downlink_childのコメント参照（同一仕様）
 *   ダウンリンク(GW→子機) PktType 0x81 / 確認応答(子機→GW) PktType 0x05
 */

#include <Arduino.h>
#include <nrf.h>
#include <Adafruit_TinyUSB.h>

// ============================================================
// ▼ 設定
// ============================================================
#define DEBUG_MODE            1

// GASへのコマンド確認間隔
#define CHECK_CMD_INTERVAL_MS  30000UL

// ★2026-08-09【一時的な切り分け用】1にすると、アップリンクの検知を待たずに、
// 保留予約があるかぎり DIAG_REPEAT_INTERVAL_MS ごとにダウンリンクを送り続ける。
//
// 【なぜ必要か】本来のClass A方式では「子機のアップリンクを検知した時に1回だけ」応答するため、
// 子機側で受信の条件を変えて試しても「そもそも送られていないだけ」なのか
// 「送られたが受信できない」のか区別がつかず、切り分けが進まない。
// 常時送信させておけば、子機がいつ・どんな状態で窓を開けても必ず電波が存在する状態を作れる。
// ★試行回数の消費・確認応答の判定も邪魔になるため、この間は再試行の打ち切りを行わない。
// 切り分けが済んだら必ず 0 に戻すこと。
#define DIAG_REPEAT_DOWNLINK      0
#define DIAG_REPEAT_INTERVAL_MS   2000UL

// SIM（gateway_v1.1と同じ切り替え方式。使う方のブロックだけ有効にする）
#define SIM_1NCE
// #define SIM_PLAN_D

#if defined(SIM_1NCE)
  const char* APN      = "iot.1nce.net";
  const char* APN_USER = "";
  const char* APN_PASS = "";
#elif defined(SIM_PLAN_D)
  const char* APN      = "planex.net";
  const char* APN_USER = "";
  const char* APN_PASS = "";
#else
  #error "SIM_1NCE または SIM_PLAN_D のどちらかを define してください"
#endif

// GAS スクリプトID・コマンド確認用デバイスID（本番gateway_v11_testとは別ID）
const char* GAS_SCRIPT_ID = "AKfycbzKVvW6vEUvJ28c_xJbHoS2ulvHiPD4OsNONdrTrf6u4kLebl4G7ADI6-YsAFSy2BBB/exec";
static char const* GW_DEVICE_ID = "lora_downlink_test";

// ★2026-08-10 検証済み（不採用）: スプレッドシート「ウェブに公開」CSV。
// 動機はリダイレクトを無くして1リクエスト・短いURLにすること。結果:
//   ・AT+HTTPTOFSは307を追従できる（status=200が返る）。ここは期待どおりだった
//   ・しかしリダイレクト先の応答が Transfer-Encoding: chunked（Content-Lengthが無い）で、
//     本文を1バイトも書けない。実機で5回中0回成功
// GASの2段構え（1段目の302本文からHREFを取り出す）を維持する。再検証する場合のみ1にする。
#define PROBE_PUBLISHED_CSV 0  // 実機で5回中0回。chunked応答を扱えず断念（下記参照）
const char* PUBLISHED_CSV_URL =
  "https://docs.google.com/spreadsheets/d/e/2PACX-1vTbWcfaLESOGmG3h11bOv6n2H9wNGOCtQ9Gn8J9-"
  "FsdqlZzeUZ4I9SrWTwrtG81sWAAlHqc91TH_vqR/pub?gid=1939102783&single=true&output=csv";

// ============================================================
// ピン割当（Gateway v1.1実機。gateway_v1.1/platformio.iniのコメント参照）
// ============================================================
static int const LORA_RX_PIN   = 0;  // D0: E220 TXD → XIAO RX
static int const LORA_TX_PIN   = 1;  // D1: XIAO TX → E220 RXD
static int const LORA_M0M1_PIN = 2;  // D2: E220 M0・M1 共通駆動（基板でM0/M1短絡済み）
#define LORA_UART_BAUD 9600

// ============================================================
// LoRa用UART（UARTE1、第2ハードウェアUART）
//
// gateway_v1.1/src/main.cpp と同じ理由で、割り込みハンドラを手動転送しないと
// write() が2回目の呼び出しで無期限にブロックする（実機デバッグ済みの既知の罠）。
// ============================================================
static Uart loraSerial(NRF_UARTE1, UARTE1_IRQn, LORA_RX_PIN, LORA_TX_PIN);

extern "C" void UARTE1_IRQHandler(void) {
  loraSerial.IrqHandler();
}

// ============================================================
// ステータスLED
// ============================================================
static void rgbHwBegin() {
#ifdef LED_RED
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
#endif
}
static void rgbHwShow(uint8_t r, uint8_t g, uint8_t b) {
#ifdef LED_RED
  analogWrite(LED_RED,   255 - r);
  analogWrite(LED_GREEN, 255 - g);
  analogWrite(LED_BLUE,  255 - b);
#endif
}
static void rgbOff()          { rgbHwShow(0, 0, 0); }
static void statusBootBlue()  { rgbHwShow(0, 0, 255); }
static void statusIdle()      { rgbHwShow(0, 0, 32); }   // 常時ダウンライトブルー: 待機中
static void statusSendGreen() { rgbHwShow(0, 255, 0); }
static void statusErrorRed()  { rgbHwShow(255, 0, 0); }

// ============================================================
// ウォッチドッグタイマー（nRF52840 内蔵 WDT。gateway_v1.1と同一値）
// ============================================================
static uint32_t const WDT_TIMEOUT_MS = 120000UL;  // 120秒: この間キックが無ければリセット

static void wdtInit(uint32_t timeoutMs) {
  NRF_WDT->CONFIG  = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);
  NRF_WDT->CRV     = (uint32_t)((uint64_t)timeoutMs * 32768ULL / 1000ULL);
  NRF_WDT->RREN    = WDT_RREN_RR0_Msk;
  NRF_WDT->TASKS_START = 1;
}
static inline void wdtFeed() { NRF_WDT->RR[0] = WDT_RR_RR_Reload; }

// ============================================================
// LoRa（E220-900T22S(JP)）— 子機・Gateway本番ファームと同一設定値
// ============================================================
#define LORA_MODE_SWITCH_DELAY_MS 100U
#define LORA_CFG_REG_START 0x00
#define LORA_CFG_REG_LEN   6

static const uint8_t LORA_CFG_ADDH = 0x00;
static const uint8_t LORA_CFG_ADDL = 0x00;
static const uint8_t LORA_CFG_REG0 = 0x68;
static const uint8_t LORA_CFG_REG1 = 0x01;
static const uint8_t LORA_CFG_REG2 = 0x00;
static const uint8_t LORA_CFG_REG3 = 0x80;

static bool loraSetMode(bool high) {
  digitalWrite(LORA_M0M1_PIN, high ? HIGH : LOW);
  delay(LORA_MODE_SWITCH_DELAY_MS);
  return true;
}
static inline bool loraModeNormal() { return loraSetMode(false); }
static inline bool loraModeConfig() { return loraSetMode(true); }

static bool loraReadConfig(uint8_t *out6) {
  while (loraSerial.available()) loraSerial.read();
  loraSerial.write((uint8_t)0xC1);
  loraSerial.write((uint8_t)LORA_CFG_REG_START);
  loraSerial.write((uint8_t)LORA_CFG_REG_LEN);

  const int respLen = 3 + LORA_CFG_REG_LEN;
  uint8_t resp[3 + LORA_CFG_REG_LEN];
  int idx = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 500UL && idx < respLen) {
    if (loraSerial.available()) resp[idx++] = (uint8_t)loraSerial.read();
  }
  if (idx < respLen) return false;
  if (resp[0] != 0xC1) return false;
  memcpy(out6, &resp[3], LORA_CFG_REG_LEN);
  return true;
}

static void loraWriteConfig() {
  loraSerial.write((uint8_t)0xC0);
  loraSerial.write((uint8_t)LORA_CFG_REG_START);
  loraSerial.write((uint8_t)LORA_CFG_REG_LEN);
  loraSerial.write(LORA_CFG_ADDH);
  loraSerial.write(LORA_CFG_ADDL);
  loraSerial.write(LORA_CFG_REG0);
  loraSerial.write(LORA_CFG_REG1);
  loraSerial.write(LORA_CFG_REG2);
  loraSerial.write(LORA_CFG_REG3);
  delay(200);
  unsigned long t0 = millis();
  while (millis() - t0 < 300UL) { while (loraSerial.available()) loraSerial.read(); }
}

static bool loraCheckAndConfigureOnce() {
  loraModeConfig();

  uint8_t cur[LORA_CFG_REG_LEN] = {0};
  bool readOk = loraReadConfig(cur);
  bool matches = readOk &&
      cur[0] == LORA_CFG_ADDH && cur[1] == LORA_CFG_ADDL &&
      cur[2] == LORA_CFG_REG0 && cur[3] == LORA_CFG_REG1 &&
      cur[4] == LORA_CFG_REG2 && cur[5] == LORA_CFG_REG3;

#if DEBUG_MODE
  Serial.print("[LORA] config read ");
  Serial.println(!readOk ? "失敗" : (matches ? "一致" : "不一致→書込"));
#endif

  if (!readOk) { loraModeNormal(); return false; }

  if (!matches) {
    loraWriteConfig();
    uint8_t verify[LORA_CFG_REG_LEN] = {0};
    bool verifyOk = loraReadConfig(verify) &&
        verify[0] == LORA_CFG_ADDH && verify[1] == LORA_CFG_ADDL &&
        verify[2] == LORA_CFG_REG0 && verify[3] == LORA_CFG_REG1 &&
        verify[4] == LORA_CFG_REG2 && verify[5] == LORA_CFG_REG3;
#if DEBUG_MODE
    Serial.println(verifyOk ? "[LORA] config write 確認OK" : "[LORA] config write 確認NG");
#endif
    if (!verifyOk) { loraModeNormal(); return false; }
  }

  loraModeNormal();
  return true;
}

#define LORA_CONFIG_RETRY 5
#define LORA_CONFIG_RETRY_DELAY_MS 500U

static bool loraCheckAndConfigure() {
  for (uint8_t attempt = 0; attempt < LORA_CONFIG_RETRY; attempt++) {
    if (loraCheckAndConfigureOnce()) return true;
#if DEBUG_MODE
    Serial.print("[LORA] config check 失敗、リトライ ");
    Serial.print(attempt + 1);
    Serial.print("/");
    Serial.println(LORA_CONFIG_RETRY);
#endif
    delay(LORA_CONFIG_RETRY_DELAY_MS);
  }
  return false;
}

static void loraPrintFrameHex(const uint8_t *msd, uint8_t msdLen, uint8_t sum) {
  Serial.print("[LORA] TXフレーム(HEX): AA ");
  if (msdLen < 0x10) Serial.print('0');
  Serial.print(msdLen, HEX);
  Serial.print(' ');
  for (uint8_t i = 0; i < msdLen; i++) {
    if (msd[i] < 0x10) Serial.print('0');
    Serial.print(msd[i], HEX);
    Serial.print(' ');
  }
  if (sum < 0x10) Serial.print('0');
  Serial.println(sum, HEX);
}

// フレーム全体をRAM上に組み立ててから1回のブロック書き込みで送る
// （バイト単位write()だとFlex側に届かなかった実機不具合の対策。19_lora_parentのPING送信で
//   実績のある方式に揃えている。詳細は旧版のコメント参照）
static void loraSendFrame(const uint8_t *msd, uint8_t msdLen) {
  uint8_t sum = (uint8_t)(0xAAU + msdLen);
  for (uint8_t i = 0; i < msdLen; i++) sum = (uint8_t)(sum + msd[i]);

#if DEBUG_MODE
  loraPrintFrameHex(msd, msdLen, sum);
#endif

  uint8_t frame[3 + 32];  // [SYNC][LEN][payload...][checksum]
  uint8_t n = 0;
  frame[n++] = 0xAA;
  frame[n++] = msdLen;
  for (uint8_t i = 0; i < msdLen; i++) frame[n++] = msd[i];
  frame[n++] = sum;

  loraSerial.write(frame, n);
  loraSerial.flush();
}

// ============================================================
// ビルド時刻の解析（__DATE__ "Mmm dd yyyy" / __TIME__ "hh:mm:ss"）
// 時刻補正フラグは使わないが、フレーム形式は25_lora_downlink_childと合わせておく
// ============================================================
static void getBuildTime(uint8_t *year2000, uint8_t *month, uint8_t *day,
                          uint8_t *hour, uint8_t *minute, uint8_t *sec) {
  static const char monthNames[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char monStr[4] = {0};
  int d = 0, y = 0, h = 0, mi = 0, s = 0;
  sscanf(__DATE__, "%3s %d %d", monStr, &d, &y);
  sscanf(__TIME__, "%d:%d:%d", &h, &mi, &s);
  int m = (strstr(monthNames, monStr) - monthNames) / 3 + 1;

  *year2000 = (uint8_t)(y - 2000);
  *month = (uint8_t)m;
  *day = (uint8_t)d;
  *hour = (uint8_t)h;
  *minute = (uint8_t)mi;
  *sec = (uint8_t)s;
}

// ============================================================
// ダウンリンクフレーム送信（25_lora_downlink_childと同一仕様）
// ============================================================
static const uint16_t DOWNLINK_COMPANY_ID = 0xC0DE;
static const uint8_t  DOWNLINK_PKT_TYPE   = 0x81;

enum DownlinkFlag {
  DL_FLAG_TIME       = 1u << 0,
  DL_FLAG_SLEEP_MIN  = 1u << 1,
  DL_FLAG_AVG_MEDIAN = 1u << 2,
};

// GASから受け取ったパラメータでダウンリンクを送信する（時刻補正は今回のスプレッドシート
// テストでは対象外のため常にフラグを立てない。将来対象に含める場合はDL_FLAG_TIMEを追加する）
//
// ★2026-08-10追加: statusOnly=trueの場合は変更フラグを一切立てない(flags=0)。
// 子機は変更が無くても確認応答（現在の設定値・WDTタイムアウト）を必ず返すため、
// 設定を変えずに「ステータス確認」だけを行いたい時に使う。sleepMinutes等の値は
// 送信はされるが子機側では無視される（flags=0のため）。
static void sendDownlinkCommand(uint8_t targetDeviceId, uint16_t sleepMinutes,
                                 uint8_t samplesPerAvg, uint8_t measureCount,
                                 bool statusOnly = false) {
  loraModeNormal();  // Configモードへ入らず、Normalモードのまま送信する（実機確認済みの方式）

  uint8_t flags = statusOnly ? 0 : (DL_FLAG_SLEEP_MIN | DL_FLAG_AVG_MEDIAN);

  uint8_t y, mo, da, h, mi, se;
  getBuildTime(&y, &mo, &da, &h, &mi, &se);

  uint8_t payload[15];
  payload[0] = (uint8_t)(DOWNLINK_COMPANY_ID >> 8);
  payload[1] = (uint8_t)(DOWNLINK_COMPANY_ID & 0xFF);
  payload[2] = DOWNLINK_PKT_TYPE;
  payload[3] = targetDeviceId;
  payload[4] = flags;
  payload[5] = y;
  payload[6] = mo;
  payload[7] = da;
  payload[8] = h;
  payload[9] = mi;
  payload[10] = se;
  payload[11] = (uint8_t)(sleepMinutes >> 8);
  payload[12] = (uint8_t)(sleepMinutes & 0xFF);
  payload[13] = samplesPerAvg;
  payload[14] = measureCount;

  statusSendGreen();
#if DEBUG_MODE
  Serial.print("[DOWNLINK] 送信: 宛先=0x"); Serial.print(targetDeviceId, HEX);
  Serial.print(" sleepMin="); Serial.print(sleepMinutes);
  Serial.print(" avg="); Serial.print(samplesPerAvg);
  Serial.print(" median="); Serial.println(measureCount);
#endif
  loraSendFrame(payload, sizeof(payload));
  delay(300);  // 送信完了待ち（AUX未接続のため固定ディレイ）

  // ★2026-08-09追加: 送信直後にUARTE1のDMA受信を明示的に再武装する。
  // 実機で「ダウンリンクを1回送信した直後からGatewayの受信が完全に止まる」事象を確認した。
  // 子機はこの直後（2秒の受信窓の中）に確認応答を返してくるため、ここで取りこぼすと
  // 永久に確認が取れず再試行を繰り返すことになる。30秒のストール検出を待たずに復帰させる。
  NRF_UARTE1->TASKS_STARTRX = 1;

  statusIdle();
}

// ============================================================
// ダウンリンク予約キャッシュ（★2026-08-09追加、Class A方式の中核）
//
// 【なぜキャッシュが要るか】
//   子機は省電力のため、自分がアップリンクを送った直後の2秒間しか受信できない。
//   その2秒の間にGASへHTTPSで問い合わせる（数秒〜数十秒かかる）ことは不可能なので、
//   Gatewayは事前に「どの子機に何を送るか」をローカルに持っておき、
//   アップリンクを受けた瞬間にネットワークを介さず即座にLoRa送信する。
//
// 【送りっぱなしにしない】
//   送信しただけでは予約を消さず、子機からの確認フレーム(PktType 0x05)を受けて初めて
//   完了とする。取りこぼしても失われず、子機の次サイクルで自動的に再試行される。
//   規定回数試しても確認が返らなければ「未達」としてGASへ報告し、予約を打ち切る。
// ============================================================
#define MAX_PENDING_CHILDREN   14   // 子機DeviceID 0x01〜0x0E
#define DOWNLINK_MAX_ATTEMPTS  3    // この回数送っても確認が返らなければ未達として打ち切る

// ★2026-08-09追加: アップリンクを検知してから応答するまでの待ち時間。
//
// 【なぜ「即座」ではダメだったか】実機の計測で以下が判明した（子機の送信開始を0msとする）:
//     85ms  子機のアップリンク送出完了
//    110ms  Gatewayが検知 → 即座に応答すると…
//    229ms  ダウンリンクが電波に乗る
//    289ms  子機のE220がUARTへ出力するが、子機はまだ送信直後で受信が止まっており取りこぼす
//    450ms  子機が受信復活＆受信窓を開く（もう手遅れ）
//   つまりダウンリンクが「窓が開く前」に通り過ぎていた。窓を10秒に広げても改善しなかったのは
//   このためで、受信機の故障でもタイミングの偶然でもなく、構造的に早すぎたのが原因。
//
// 【この値の決め方】子機が窓を開けるのは自分の送信開始から約450ms後。
// Gatewayの検知は約110ms後なので、そこから350ms以上待てば窓に入る。
// 余裕を見て400msとし、実際の送出は検知から約520ms後（子機の窓が開いた70ms後）になる。
// 子機の窓が2秒あるので十分内側に収まる。
#define DOWNLINK_RESPONSE_DELAY_MS 400UL

// ダウンリンク結果のステータスコード（子機ファーム・GASと一致させること）
#define DL_STATUS_OK          0
#define DL_STATUS_RANGE_ERROR 1
#define DL_STATUS_CLAMPED     2
#define DL_STATUS_NO_ACK      99   // Gateway自身が付ける「未達」

struct PendingDownlink {
  bool     active;
  uint8_t  childId;
  uint16_t sleepMin;
  uint8_t  avg;
  uint8_t  median;
  uint8_t  attempts;
  uint32_t seq;         // 予約の通し番号。報告に含めてGAS側で新旧を判別させる
  bool     statusOnly;  // ★2026-08-10追加: trueなら設定変更フラグを立てずに送る（ステータス確認）
};
static PendingDownlink s_pending[MAX_PENDING_CHILDREN];

// GASへの報告キュー。
// ★loraPoll()はsendAT()の待機ループからも呼ばれるため、その中で直接HTTPS通信を
//   始めると再帰的にAT通信が入れ子になって破綻する。受信処理では「キューに積む」だけにし、
//   実際の送信はloop()の安全な場所で行う。
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
  uint16_t wdtMin;  // ★2026-08-10追加: 子機の確認応答に載る、適用後有効になるWDTタイムアウト（分）
};
#define MAX_REPORTS 8
static DownlinkReport s_reports[MAX_REPORTS];

static void queueReport(bool finalResult, uint8_t childId, uint8_t status,
                        uint16_t sleepMin, uint8_t avg, uint8_t median,
                        uint8_t attempts, uint32_t seq, uint16_t wdtMin = 0) {
  for (int i = 0; i < MAX_REPORTS; i++) {
    if (s_reports[i].used) continue;
    s_reports[i].used        = true;
    s_reports[i].finalResult = finalResult;
    s_reports[i].childId     = childId;
    s_reports[i].status      = status;
    s_reports[i].sleepMin    = sleepMin;
    s_reports[i].avg         = avg;
    s_reports[i].median      = median;
    s_reports[i].attempts    = attempts;
    s_reports[i].seq         = seq;
    s_reports[i].wdtMin      = wdtMin;
    return;
  }
#if DEBUG_MODE
  Serial.println(F("[REPORT] キューが満杯のため報告を破棄しました"));
#endif
}

// ★2026-08-09追加: 「実際に送信したダウンリンク」の控え。
//
// 【なぜ必要か】確認応答を受けた時点でs_pending[]を見てseqを決めていたところ、
// 送信してから応答が返るまでの間にキャッシュ更新が走って新しい予約(seq)へ
// 入れ替わっていると、古い応答を新しい予約の完了として報告してしまう。
// 実機で「seq=7を送信 → キャッシュがseq=8に更新 → seq=7の応答をseq=8として報告」が発生し、
// 一度も送信していないseq=8が完了扱いで消えた。
// 送信時点の内容をここに控えておき、応答はこれと突き合わせる。
struct SentDownlink {
  bool     valid;
  uint8_t  childId;
  uint8_t  attempts;
  uint32_t seq;
};
static SentDownlink s_lastSent[MAX_PENDING_CHILDREN];

static SentDownlink* findLastSent(uint8_t childId) {
  for (int i = 0; i < MAX_PENDING_CHILDREN; i++) {
    if (s_lastSent[i].valid && s_lastSent[i].childId == childId) return &s_lastSent[i];
  }
  return nullptr;
}

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

static PendingDownlink* findPending(uint8_t childId) {
  for (int i = 0; i < MAX_PENDING_CHILDREN; i++) {
    if (s_pending[i].active && s_pending[i].childId == childId) return &s_pending[i];
  }
  return nullptr;
}

// ============================================================
// LoRa受信（Class A方式のため、Gatewayは常時受信して子機の起床を検知する）
// gateway_v1.1のloraPoll()と同じフレーム形式・状態機械。
//   [0xAA][LEN][payload...][checksum][RSSI]
// ============================================================
#define LORA_FIELD_TIMEOUT_MS 500UL
#define UPLINK_PKT_TYPE       0x04   // Flex v3.10 LoRa のアップリンク
#define DOWNLINK_ACK_PKT_TYPE 0x05   // 子機からの確認応答（★2026-08-09追加）

enum LoraRxState { LORA_WAIT_SYNC, LORA_WAIT_LEN, LORA_WAIT_BODY, LORA_WAIT_CKSUM, LORA_WAIT_RSSI };
static LoraRxState s_loraState = LORA_WAIT_SYNC;
static uint8_t     s_loraLen = 0;
static uint8_t     s_loraBody[32];
static uint8_t     s_loraBodyIdx = 0;
static uint8_t     s_loraSum = 0;
static uint8_t     s_loraRssiRaw = 0;
static uint32_t    s_loraFieldStartMs = 0;
static uint32_t    s_loraFramesOk = 0;

static bool loraFeedByte(uint8_t b) {
  switch (s_loraState) {
    case LORA_WAIT_SYNC:
      if (b == 0xAA) { s_loraSum = b; s_loraState = LORA_WAIT_LEN; s_loraFieldStartMs = millis(); }
      return false;
    case LORA_WAIT_LEN:
      s_loraLen = b;
      s_loraSum = (uint8_t)(s_loraSum + b);
      s_loraBodyIdx = 0;
      if (s_loraLen == 0 || s_loraLen > sizeof(s_loraBody)) { s_loraState = LORA_WAIT_SYNC; return false; }
      s_loraState = LORA_WAIT_BODY;
      return false;
    case LORA_WAIT_BODY:
      s_loraBody[s_loraBodyIdx++] = b;
      s_loraSum = (uint8_t)(s_loraSum + b);
      if (s_loraBodyIdx >= s_loraLen) s_loraState = LORA_WAIT_CKSUM;
      return false;
    case LORA_WAIT_CKSUM:
      if (b != s_loraSum) { s_loraState = LORA_WAIT_SYNC; return false; }
      s_loraState = LORA_WAIT_RSSI;
      return false;
    case LORA_WAIT_RSSI:
      s_loraRssiRaw = b;
      s_loraState = LORA_WAIT_SYNC;
      return true;
    default:
      s_loraState = LORA_WAIT_SYNC;
      return false;
  }
}

// アップリンクを受けた瞬間に呼ばれる。予約があればその場でダウンリンクを返す。
// ★ここでネットワーク通信をしてはいけない（子機の受信窓は2秒しかない）。
static void onUplinkReceived(uint8_t childId) {
  PendingDownlink* p = findPending(childId);
  if (p == nullptr) return;

#if DIAG_REPEAT_DOWNLINK
  // 【切り分け中】送信はloop()の定期送信に任せ、ここでは試行回数を進めない。
  // （打ち切られて予約が消えると定期送信も止まり、切り分けにならないため）
  Serial.print(F("[DIAG] 子機0x")); Serial.print(childId, HEX);
  Serial.println(F(" のアップリンクを検知（定期送信モード中のため個別応答はしません）"));
  return;
#else

  if (p->attempts >= DOWNLINK_MAX_ATTEMPTS) {
    // 規定回数送っても確認が返らなかった → 未達として打ち切り、GASへ報告する
#if DEBUG_MODE
    Serial.print(F("[DOWNLINK] 子機0x")); Serial.print(childId, HEX);
    Serial.print(F(" へ")); Serial.print(p->attempts);
    Serial.println(F("回送信しましたが確認が返りません。未達として打ち切ります"));
#endif
    queueReport(true, childId, DL_STATUS_NO_ACK, 0, 0, 0, p->attempts, p->seq);
    p->active = false;
    return;
  }

  p->attempts++;
#if DEBUG_MODE
  Serial.print(F("[DOWNLINK] 子機0x")); Serial.print(childId, HEX);
  Serial.print(F(" のアップリンクを検知（")); Serial.print(p->attempts);
  Serial.print(F("回目/")); Serial.print(DOWNLINK_MAX_ATTEMPTS);
  Serial.print(F("）→ ")); Serial.print(DOWNLINK_RESPONSE_DELAY_MS);
  Serial.println(F("ms待ってから送信します（子機が受信窓を開くのを待つ）"));
#endif
  // ★即座に送ると子機の受信窓が開く前に到着してしまう（上のDOWNLINK_RESPONSE_DELAY_MS参照）。
  //   ここでloraDelay()（＝loraPoll付き）を使ってはいけない。この関数自体がloraPoll()から
  //   呼ばれているため、待機中に別の子機のアップリンクが来ると再帰してしまう。
  //   受信バイトはUARTE割り込みでリングバッファに積まれるので、ここでポーリングを止めても
  //   取りこぼしにはならず、処理が数百ms遅れるだけで済む。
  {
    uint32_t t0 = millis();
    while (millis() - t0 < DOWNLINK_RESPONSE_DELAY_MS) { wdtFeed(); yield(); }
  }
  sendDownlinkCommand(childId, p->sleepMin, p->avg, p->median, p->statusOnly);
  recordSent(childId, p->attempts, p->seq);  // 確認応答をこの予約に紐付けるための控え
  queueReport(false, childId, 0, p->sleepMin, p->avg, p->median, p->attempts, p->seq);
#endif  // DIAG_REPEAT_DOWNLINK
}

// 子機からの確認応答を受けた時の処理。予約を完了扱いにし、結果をGASへ報告する。
//   ack: [0]0x05 [1]DeviceID [2]status [3-4]適用sleepMin(BE) [5]適用avg [6]適用median
//        [7-8]適用後に有効になるWDTタイムアウト(分,BE) ★2026-08-10追加
static void onDownlinkAckReceived(const uint8_t* ack, uint8_t len) {
  if (len < 7) return;
  uint8_t  childId = ack[1];
  uint8_t  status  = ack[2];
  uint16_t applied = ((uint16_t)ack[3] << 8) | ack[4];
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
  uint8_t  attempts = sent->attempts;
  uint32_t seq      = sent->seq;

#if DEBUG_MODE
  Serial.print(F("[DOWNLINK] ★子機0x")); Serial.print(childId, HEX);
  Serial.print(F(" から確認応答: status=")); Serial.print(status);
  Serial.print(F(" 適用値 間隔=")); Serial.print(applied);
  Serial.print(F("分 平均=")); Serial.print(appliedAvg);
  Serial.print(F(" メジアン=")); Serial.print(appliedMedian);
  Serial.print(F(" WDT=")); Serial.print(appliedWdtMin); Serial.println(F("分"));
#endif

  queueReport(true, childId, status, applied, appliedAvg, appliedMedian, attempts, seq, appliedWdtMin);
  sent->valid = false;  // この控えは消費した

  // ★再試行を止めてよいのは「今キャッシュにある予約」＝「今受け取った応答の予約」の時だけ。
  //   応答待ちの間に新しい予約へ入れ替わっていた場合、その新しい予約はまだ未送信なので
  //   activeのまま残し、次のアップリンクで送信されるようにする。
  PendingDownlink* p = findPending(childId);
  if (p != nullptr && p->seq == seq) p->active = false;
}

// ★2026-08-09追加: E220の「受信不能ラッチ」解除とUARTE1受信ストールからの復帰。
//
// 【経緯】この対策が無い状態で実機テストしたところ、Gatewayは設定コマンド(0xC1)には
// 正常応答するのに、子機のアップリンクを1バイトも受信できなかった（受信フレーム=0のまま）。
// これは [[e220_rx_latch_on_battery_poweron]] に記録済みの既知の症状で、
// E220モジュール内部が受信不能状態でラッチしており、MCUのリセットでは解除されない
// （E220は給電され続けるため）。通常モードで数バイト送信させると受信が復活する。
// 19_lora_parent でも「受信ストール検出→RX再起動」の後から初めて受信が始まっていた。
//
// 送出する3バイトは全て0x00。受信側の状態機械は同期バイト0xAAを探すため、
// 他機がこれをフレームとして誤認することはない。
static uint32_t s_loraLastRxMs  = 0;
static uint32_t s_loraRekicks   = 0;
static uint8_t  s_loraErrSrcAcc = 0;
#define LORA_RX_STALL_MS 30000UL

// ★2026-08-10追加: sendAT()の待機ループ中はloraPoll()が呼ばれ続けるため、その中で
// loraRxWatchdog()がストールを検出すると、SIM7080Gへの AT+SHREQ 文字列送信の
// 真っ最中に loraKickTx()（E220への書き込み＋delay(200)）が割り込むことがあった。
// 実機でこの割り込みと同時にSIM7080G側へ送るAT文字列がバイト単位で化ける事象
// （GAS_SCRIPT_IDの一部が別の文字に置き換わりERRORになる）を確認した。
// AT通信中はキック処理（実際にUARTへ書き込み、delay()するもの）だけを止める。
// 受信そのもの・ストール検出のカウントは止めない（次にAT通信が終わった時点で
// 改めてキックされる）。
static bool s_atBusy = false;

static void loraKickTx() {
  const uint8_t dummy[3] = {0x00, 0x00, 0x00};
  loraSerial.write(dummy, sizeof(dummy));
  loraSerial.flush();
  delay(200);  // 送信完了待ち（AUX未接続のため固定ディレイ）
  while (loraSerial.available()) loraSerial.read();  // 反射・エコーがあれば捨てる
}

static void loraRxWatchdog() {
  uint32_t errsrc = NRF_UARTE1->ERRORSRC;
  if (errsrc) {
    NRF_UARTE1->ERRORSRC = errsrc;  // 書き戻すとクリアされる
    s_loraErrSrcAcc |= (uint8_t)(errsrc & 0x0F);
  }
  if (NRF_UARTE1->EVENTS_ERROR) NRF_UARTE1->EVENTS_ERROR = 0;

  if (millis() - s_loraLastRxMs < LORA_RX_STALL_MS) return;

  // ★AT通信中はキック処理を先送りする。s_loraLastRxMsを更新しないため、
  // AT通信が終わった直後のloraPoll()呼び出しで改めてこの条件に入り、確実にキックされる。
  if (s_atBusy) return;

  s_loraLastRxMs = millis();
  s_loraRekicks++;
  NRF_UARTE1->TASKS_STARTRX = 1;  // DMA受信を再武装する（既に動作中でも実害はない）

  Serial.print(F("[LORA] 受信ストール検出 → RX再起動＋ラッチ解除キック #"));
  Serial.print(s_loraRekicks);
  Serial.print(F(" ERRORSRC累積=0x")); Serial.println(s_loraErrSrcAcc, HEX);

  // ★2026-08-09: 当初は「一度でも受信できていればキックは不要」として
  // s_loraFramesOk==0 の時だけキックする実装にしたが、実機で
  // 「Gatewayがダウンリンクを1回送信した直後から受信が完全に止まる」事象が発生した。
  // 受信実績があってもE220は受信不能状態に落ちうるため、19_lora_parentで実績のある
  // 「無条件にキックする」実装に戻す。
  // キック送信(3バイト・約50ms)が子機のアップリンクと衝突する確率は
  // 30秒に1回の頻度なら0.2%程度で、受信が完全に死ぬリスクの方がはるかに大きい。
  loraModeNormal();
  loraKickTx();
}

static void loraPoll() {
  while (loraSerial.available()) {
    uint8_t b = (uint8_t)loraSerial.read();
    s_loraLastRxMs = millis();  // 受信が生きている証跡（ストール監視の基準）
    if (loraFeedByte(b)) {
      s_loraFramesOk++;
      uint8_t pktType = s_loraBody[0];
      if (pktType == UPLINK_PKT_TYPE) {
        onUplinkReceived(s_loraBody[1]);
      } else if (pktType == DOWNLINK_ACK_PKT_TYPE) {
        onDownlinkAckReceived(s_loraBody, s_loraLen);
      }
      // それ以外のPktTypeは無視（他プロジェクトのフレーム等）
    }
  }

  if (s_loraState != LORA_WAIT_SYNC && millis() - s_loraFieldStartMs > LORA_FIELD_TIMEOUT_MS) {
    s_loraState = LORA_WAIT_SYNC;  // フレーム途中で詰まったら同期探索へ戻す
  }

  loraRxWatchdog();  // 受信が止まっていないか監視し、必要ならRX再起動＋ラッチ解除
}

// ★裸のdelay()の代わりに使う。待っている間もLoRa受信を処理し続ける。
//
// 【なぜ必要か】子機の受信窓は2秒しかない。この間にGatewayがdelay()で止まっていると、
// アップリンクの検知が遅れてダウンリンクが窓に間に合わない。特に旧実装には
// ネットワーク再接続時のdelay(5000)や送信失敗時のdelay(30000)があり、
// そこに当たると丸ごと1サイクル取りこぼしていた（確認応答方式により失われはしないが、
// 無駄な再試行を減らすためポーリングを止めない）。
static void loraDelay(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    wdtFeed();
    loraPoll();
    yield();
  }
}

// ============================================================
// SIM7080G（LTE-Mモデム）AT通信層
// gateway_v1.1/src/main.cpp からGASコマンド確認に必要な部分だけを移植。
// ============================================================
static uint16_t const SENDAT_MAX_RESPONSE_LEN = 2048;

static bool sendAtHasTerminator(const String& res, const char* waitForToken) {
  if (waitForToken != nullptr) return res.indexOf(waitForToken) >= 0;
  return res.indexOf("OK\r\n") >= 0 || res.indexOf("ERROR") >= 0;
}

// ★2026-08-10追加: ATコマンドを小分けにして送る。
//
// 【なぜ必要か】XIAOとSIM7080Gの間にハードウェアフロー制御(RTS/CTS)が配線されていない。
// そのため115200bpsで長いATコマンドを一気に流し込むと、モデム側のUART受信バッファが
// 溢れてバイトを取りこぼす。実機で以下の文字化けを確認した:
//   ・約500文字の AT+HTTPTOFS（リダイレクト先URL付き）→ URL途中に制御文字が混入
//     例: ...MyQC1Sxc7ONhXx@[0x04]BDE[0x05]QD[0x00]Hna1wWm1cYgbU4M9...
//   ・約180文字の AT+SHREQ でも低頻度で発生（例: AT+SHREQ → AT#R[0x05]Q=）
// コマンド長に比例して発生率が上がる（500文字級はほぼ毎回）。
// 64バイトずつflush()しながら送り、モデム側が捌く時間を与えることで回避する。
static void writeAtCommand(const String& cmd) {
  // 送信開始前に受信バッファを空にしておく（前のコマンドの残骸を持ち越さない）
  while (Serial1.available()) Serial1.read();

  // ★2026-08-10: 64バイト/15msでもURLの切り詰めが残ったため32バイト/25msへ強化。
  //   500文字のコマンドで約400msかかるが、GAS通信は30秒に1回なので許容する。
  const size_t CHUNK = 32;
  size_t len = cmd.length();
  for (size_t i = 0; i < len; i += CHUNK) {
    size_t n = (i + CHUNK < len) ? CHUNK : (len - i);
    Serial1.print(cmd.substring(i, i + n));
    Serial1.flush();   // このチャンクの送出完了を待つ
    // ★送信中も受信バッファを読み捨てる。エコーが有効なまま残っていた場合、
    //   送信中に同じ量が返ってきてnRF52の受信リングバッファを溢れさせ、
    //   以降の応答受信まで壊してしまうため（ATE0で切っているはずだが保険）。
    while (Serial1.available()) Serial1.read();
    delay(25);         // モデム側の受信処理に猶予を与える
    while (Serial1.available()) Serial1.read();
  }
  Serial1.print("\r\n");
  Serial1.flush();
}

static String sendAT(String cmd, int waitMs = 5000, const char* waitForToken = nullptr) {
  // ★s_atBusyはコマンド送信の"前"に立てる。送信中のコマンド文字列そのものが
  // loraKickTx()の割り込みで化ける事象を実機で確認したため（詳細はloraRxWatchdog()参照）。
  s_atBusy = true;
  writeAtCommand(cmd);
  long start = millis();
  String res = "";
  while (millis() - start < waitMs) {
    wdtFeed();
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      if (res.length() < SENDAT_MAX_RESPONSE_LEN) res += c;
    }
    loraPoll();  // AT応答待ちの間も子機の起床を取りこぼさない（gateway_v1.1と同じ配慮）
    if (sendAtHasTerminator(res, waitForToken)) {
      delay(20);
      while (Serial1.available()) {
        char c = (char)Serial1.read();
        if (res.length() < SENDAT_MAX_RESPONSE_LEN) res += c;
      }
      break;
    }
    yield();
  }
  s_atBusy = false;
  return res;
}

static String sendATFull(String cmd, int waitMs) {
  s_atBusy = true;  // 理由はsendAT()のコメント参照
  writeAtCommand(cmd);
  String res = "";
  long start = millis();
  while (millis() - start < waitMs) {
    wdtFeed();
    while (Serial1.available()) res += (char)Serial1.read();
    loraPoll();
    yield();
  }
  s_atBusy = false;
  return res;
}

void simStage(const char* name, bool ok) {
  Serial.print(ok ? F("[OK] ") : F("[NG] "));
  Serial.println(name);
}

int getSimCsq() {
  String csq = sendAT("AT+CSQ", 3000);
  int idx = csq.indexOf("+CSQ: ");
  if (idx < 0) return 99;
  return csq.substring(idx + 6, csq.indexOf(",", idx)).toInt();
}

// ネットワーク初期化（gateway_v1.1のinitNetwork()を簡略化。詳細な障害診断ログは省略）
bool initNetwork() {
  sendAT("AT+CREG=0", 2000); delay(200);
  sendAT("AT+CBANDCFG=\"CAT-M\",1,2,3,4,5,8,12,13,18,19,20,25,26,28,66,71,85", 3000); delay(500);
  sendAT("AT+CNMP=38"); delay(500);
  sendAT("AT+CMNB=1");  delay(500);
  sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\""); delay(500);
  if (strlen(APN_USER) > 0) {
    sendAT("AT+CGAUTH=1,1,\"" + String(APN_PASS) + "\",\"" + String(APN_USER) + "\""); delay(500);
  }

  bool cregOk = false;
  Serial.print(F("[   ] NET: ネットワーク登録待ち (最大60秒)"));
  for (int i = 0; i < 12; i++) {
    Serial.print('.');
    String reg = sendAT("AT+CREG?", 3000);
    int cregComma = reg.indexOf("+CREG: ");
    if (cregComma >= 0) {
      int statComma = reg.indexOf(",", cregComma + 7);
      if (statComma >= 0) {
        char stat = reg.charAt(statComma + 1);
        if (stat == '1' || stat == '5') { cregOk = true; break; }
      }
    }
    if (i < 11) loraDelay(5000);
  }
  Serial.println();
  simStage("NET: ネットワーク登録 (CREG=1 or 5)", cregOk);
  if (!cregOk) return false;

  bool attachOk = false;
  Serial.print(F("[   ] NET: データ Attach 待ち"));
  for (int i = 0; i < 12; i++) {
    Serial.print('.');
    loraDelay(5000);
    String att = sendAT("AT+CGATT?", 3000);
    if (att.indexOf("+CGATT: 1") >= 0) { attachOk = true; break; }
  }
  if (!attachOk) {
    sendAT("AT+CGACT=1,1", 10000);
    loraDelay(3000);
    String att2 = sendAT("AT+CGATT?", 3000);
    if (att2.indexOf("+CGATT: 1") >= 0) attachOk = true;
  }
  Serial.println();
  simStage("NET: データ Attach (CGATT=1)", attachOk);
  if (!attachOk) return false;
  loraDelay(1000);

  sendAT("AT+CNACT=0,1", 15000); loraDelay(3000);
  String cnact = sendAT("AT+CNACT?", 3000);
  bool ipOk = cnact.indexOf("0,1") >= 0;
  simStage("NET: IP アドレス取得 (CNACT)", ipOk);
  return ipOk;
}

// ============================================================
// ★2026-08-10: GAS通信をAT+HTTPTOFS方式へ全面移行（AT+SH*系は使わない）
//
// 【AT+SH*系をやめた理由】実機とcurlでの実測により、以下が確定した:
//   ・GoogleはHTTP/1.1で Content-Length を返さず Transfer-Encoding: chunked を使う
//   ・その Transfer-Encoding は応答ヘッダーのオフセット978に位置する
//   ・Locationヘッダーだけで470バイト、応答ヘッダー全体は1010バイト
//   ・一方 AT+SHCONF="HEADERLEN" の仕様上の最大値は350（マニュアルV1.04 §13.2.1）
//   → モデムは Transfer-Encoding を構造的に見られず、+SHREQ が DataLen=0 を返すことがある。
//   さらに2段目（googleusercontent.comへの再接続）も不安定で、実機で常用に耐えなかった。
//
// 【AT+HTTPTOFSの実機確認結果（2026-08-10）】
//   ・302応答でも status=200 / DataLen=671 で本文を確実に取得できる（chunkedを正しく処理）
//   ・ただし リダイレクトは追従しない。302のHTMLページ本体がそのまま得られる
//   → 1段目でこのHTMLからLocation URLを取り出し、2段目で同じくHTTPTOFSで取得する
//
// 【AT+SH*系と混ぜてはいけない】
//   HTTPTOFS実行後に AT+SHCONN が ERROR になる事象を実機で確認した（同じHTTP/SSL
//   リソースを共有しているため状態が壊れると思われる）。この2系統は併用せず、
//   本スケッチではHTTPTOFSに一本化する。
//
// 【報告系(downlink_sent/result)は1段目だけでよい】
//   GAS Web Appは doGet() を実行し終えてからリダイレクトを返す。つまり副作用
//   （Script Propertiesの更新等）は1段目の時点で確定している。本文が不要な用途では
//   リダイレクトを追う必要がない。
// ============================================================
#define HTTPTOFS_DIR_INDEX 3            // 3 = "/customer/"（AT+CFSRFILEの<index>）
#define HTTPTOFS_FILENAME  "gasdl.txt"

// AT+CFSRFILE の応答から本文を取り出す。
// 応答形式: "AT+CFSRFILE=...\r\nOK\r\n\r\n+CFSRFILE: <len>\r\n<data>\r\n\r\nOK\r\n"
static String extractCfsrfileBody(const String& raw, int expectedLen) {
  int bi = raw.indexOf("+CFSRFILE: ");
  if (bi < 0) return "";
  int nl = raw.indexOf('\n', bi);
  if (nl < 0) return "";
  int bodyStart = nl + 1;
  int available = raw.length() - bodyStart;
  int len = (expectedLen > 0 && expectedLen < available) ? expectedLen : available;
  return raw.substring(bodyStart, bodyStart + len);
}

// 任意のURLをAT+HTTPTOFSで取得する。成功時は本文、失敗時は空文字列。
// wantBody=false なら本文の読み出しを省略する（副作用だけが目的の報告系で使う）。
// AT+HTTPTOFSのダウンロード状態がIdle(0)になるまで待つ。
// ★発行"前"にも必要。前回の状態が残っているうちに次のAT+HTTPTOFSを出すと、
//   ERRORで弾かれるか、受け付けても0バイトしか書かれない（実機で確認）。
static bool waitHttpToFsIdle(int maxWaitMs) {
  long start = millis();
  while (millis() - start < maxWaitMs) {
    String rl = sendAT("AT+HTTPTOFSRL?", 3000);
    int ri = rl.indexOf("+HTTPTOFSRL: ");
    if (ri >= 0 && rl.substring(ri + 13).toInt() == 0) return true;
    delay(250);
  }
  return false;
}

// ★2026-08-10: モデム内部のHTTP/ファイル系リソース枯渇からの復旧。
// 起動直後は成功するのに、サイクルを重ねるほど「status=200 len=0」が増えて最後は
// ほぼ全滅する事象を実機で確認した（CFSINITはOK、HTTPTOFSRLも0,0,0で、AT手順側に
// 問題は見えない）。マニュアルの<StatusCode>には602 No memory / 604 Stack Busyが
// 定義されており、モジュール内部の枯渇と考えられる。段階的に強い手段で復旧する。
static int s_fsFailStreak = 0;
static bool s_netOk = false;

static void recoverHttpStack() {
  if (s_fsFailStreak == 3) {
    // 第1段階: PDPコンテキストを張り直してHTTP/ソケット資源を解放させる
    Serial.println(F("[GAS] 復旧: PDPコンテキストを張り直します"));
    sendAT("AT+CNACT=0,0", 15000);
    loraDelay(3000);
    sendAT("AT+CNACT=0,1", 15000);
    loraDelay(3000);
  } else if (s_fsFailStreak >= 6) {
    // 第2段階: モデム再起動（PDP張り直しでも戻らない場合）
    Serial.println(F("[GAS] 復旧: モデムを再起動します"));
    s_fsFailStreak = 0;
    sendAT("AT+CFUN=1,1", 10000);
    loraDelay(8000);
    for (int t = 0; t < 20; t++) {
      if (sendAT("AT", 1000).indexOf("OK") >= 0) break;
      loraDelay(500);
    }
    sendAT("ATE0", 2000);
    s_netOk = initNetwork();
  }
}

static String httpGetViaFs(const String& url, bool wantBody) {
  // ★AT+HTTPTOFSの"発行前"にAT+HTTPTOFSRL?を打ってはいけない。
  //   実機で、直前に状態読み出しを入れた途端に「status=200 len=0」が起動直後から
  //   100%発生するようになった（外すと元に戻る）。完了待ちは発行"後"だけにする。
  loraDelay(300);

  // ★CFSINIT/CFSTERMは「フラッシュバッファの確保/解放」。ダウンロードを挟むと
  //   確保状態が無効になるらしく、ダウンロード前に確保したままCFSRFILEすると
  //   空が返ることがあった（実機で発生）。削除用と読み出し用でペアを分ける。
  // ★2026-08-10: 確保が残っていると AT+HTTPTOFS が書き込めず「status=200 len=0」になる
  //   （実機で AT+CFSINIT が ERROR を返した回だけ len=0 になることを確認）。
  //   前回の解放漏れを取り除くため、まず無条件に CFSTERM する（未確保なら ERROR だが無害）。
  sendAT("AT+CFSTERM", 3000);

  String delRes = sendAT("AT+CFSINIT", 3000);
  if (delRes.indexOf("OK") < 0) {
    Serial.println(F("[GAS] 警告: AT+CFSINITがERROR（確保が残っている可能性）"));
  }
  delRes += sendAT("AT+CFSDFILE=" + String(HTTPTOFS_DIR_INDEX) + ",\"" + HTTPTOFS_FILENAME + "\"", 3000);
  delRes += sendAT("AT+CFSTERM", 3000);

  String res = sendAT("AT+HTTPTOFS=\"" + url + "\",\"/customer/" + HTTPTOFS_FILENAME + "\",50,5",
                      60000, "+HTTPTOFS:");

  // 応答: +HTTPTOFS: <StatusCode>,<DataLen>
  int statusCode = 0, dataLen = 0;
  int si = res.indexOf("+HTTPTOFS: ");
  if (si >= 0) {
    String s = res.substring(si + 11);
    int c1 = s.indexOf(',');
    if (c1 > 0) {
      statusCode = s.substring(0, c1).toInt();
      int e = c1 + 1;
      while (e < (int)s.length() && isDigit(s[e])) e++;
      dataLen = s.substring(c1 + 1, e).toInt();
    }
  }
  Serial.print(F("[GAS] HTTPTOFS status=")); Serial.print(statusCode);
  Serial.print(F(" len=")); Serial.println(dataLen);

  if (statusCode != 200 || (wantBody && dataLen <= 0)) {
#if DEBUG_MODE
    // ★失敗時は原因を追えるよう生の応答とファイルシステム状態を出す。
    // len=0が続く場合、フラッシュの空き不足・ファイル削除失敗・書き込み失敗などが疑われる。
    Serial.print(F("[DEBUG] 削除シーケンス応答=[")); Serial.print(delRes); Serial.println(F("]"));
    Serial.print(F("[DEBUG] HTTPTOFS raw=[")); Serial.print(res); Serial.println(F("]"));
    Serial.print(F("[DEBUG] ダウンロード状態: ")); Serial.println(sendAT("AT+HTTPTOFSRL?", 3000));
    // ★CFSGFIS/CFSGFRSはAT+CFSINITでフラッシュバッファを確保していないとERRORになる。
    //   確保せずに呼んでいたため、これまでERRORしか見えていなかった。
    sendAT("AT+CFSINIT", 3000);
    Serial.print(F("[DEBUG] ファイルサイズ: "));
    Serial.println(sendAT("AT+CFSGFIS=" + String(HTTPTOFS_DIR_INDEX) + ",\"" + HTTPTOFS_FILENAME + "\"", 3000));
    // ★AT+CFSGFRSは"読み出しコマンド"。末尾の?を落とすとERRORになる（過去の誤り）。
    Serial.print(F("[DEBUG] FS空き容量: ")); Serial.println(sendAT("AT+CFSGFRS?", 3000));
    sendAT("AT+CFSTERM", 3000);
#endif
    s_fsFailStreak++;
    recoverHttpStack();
    return "";
  }
  s_fsFailStreak = 0;
  if (!wantBody) return "ok";        // 本文不要（副作用だけが目的）

  // ★2026-08-10追加: ダウンロード完了を待つ。
  // AT+HTTPTOFSは非同期で、+HTTPTOFS URCが返った時点ではファイル書き込みが
  // 完了していないことがある（マニュアルに状態確認用のAT+HTTPTOFSRLが用意されている
  // ことがその裏付け）。実機で「status=200 len=671なのに読み出すと空」という事象が
  // 頻発したのはこれが原因と考えられる。<status> 0=Idle（完了）/ 1=ダウンロード中。
  waitHttpToFsIdle(10000);

  // ★実際のファイルサイズを確認してから読む。
  // 古いファイルが残っていた場合にそれを読んでしまうと、期限切れのリダイレクトURLを
  // 掴んでGoogleの「ページが見つかりません」ページに飛ばされる（実機で発生）。
  // ★CFSGFISはCFSINITでフラッシュバッファを確保していないと正しい値を返さない
  //   （確保せずに呼んでいたため「期待=671 実際=0」で正常なデータを捨てていた）。
  //   サイズ確認と読み出しを同じCFSINIT/CFSTERMの中で行う。
  sendAT("AT+CFSINIT", 3000);

  int fileSize = 0;
  String gfis = sendAT("AT+CFSGFIS=" + String(HTTPTOFS_DIR_INDEX) + ",\"" + HTTPTOFS_FILENAME + "\"", 3000);
  int gi = gfis.indexOf("+CFSGFIS: ");
  if (gi >= 0) fileSize = gfis.substring(gi + 10).toInt();

  if (fileSize != dataLen) {
    sendAT("AT+CFSTERM", 3000);
    Serial.print(F("[GAS] ファイルサイズ不一致（期待=")); Serial.print(dataLen);
    Serial.print(F(" 実際=")); Serial.print(fileSize); Serial.println(F("）→ 破棄"));
    return "";
  }

  String fileRes = sendATFull("AT+CFSRFILE=" + String(HTTPTOFS_DIR_INDEX) + ",\"" + HTTPTOFS_FILENAME +
                              "\",0," + String(dataLen) + ",0", 5000);
  sendAT("AT+CFSTERM", 3000);

  String body = extractCfsrfileBody(fileRes, dataLen);
  body.trim();
  return body;
}

// 302のHTMLページ本体から <A HREF="..."> のURLを取り出す（&amp;も復元する）
static String extractRedirectUrl(const String& html) {
  int hi = html.indexOf("HREF=\"");
  if (hi < 0) return "";
  hi += 6;
  int hEnd = html.indexOf("\"", hi);
  if (hEnd < 0) return "";
  String url = html.substring(hi, hEnd);
  url.replace("&amp;", "&");
  return url;
}

// GASへGETし、応答本文を文字列で返す（失敗時は空文字列）。
// GAS Web Appは必ず一度302でscript.googleusercontent.comへリダイレクトされるため、
// 1段目でリダイレクト先URLを取り出し、2段目でその本文を取得する。
// どちらもAT+HTTPTOFS経由（理由はhttpGetViaFs()の先頭コメント参照）。
static String gasGetText(const String& queryParams) {
  String url1 = "https://script.google.com/macros/s/";
  url1 += GAS_SCRIPT_ID;
  url1 += "?";
  url1 += queryParams;

  // ★URLの切り詰め（フロー制御なしUARTの取りこぼし）が起きると、Googleの404ページが
  //   返ってくる。1回だけ即リトライする（次サイクルまで30秒待たずに済む）。
  String html;
  for (int attempt = 0; attempt < 3; attempt++) {
    // 連続で叩くとモデムが601（ネットワークエラー）を返すことがあるため間隔を空ける
    if (attempt > 0) delay(1000);
    Serial.println(attempt == 0 ? F("[GAS] 1段目を取得中...") : F("[GAS] 1段目を再取得中..."));
    html = httpGetViaFs(url1, true);
    if (html.length() == 0) continue;
    // Googleのエラーページ（404等）を本文として扱わない。URLが途中で切れた証拠。
    if (html.indexOf("<!DOCTYPE html>") >= 0 && html.indexOf("HREF=\"") < 0) {
      Serial.println(F("[GAS] Googleのエラーページが返された（URLが壊れている可能性）"));
      html = "";
      continue;
    }
    break;
  }
  if (html.length() == 0) {
    Serial.println(F("[GAS] 1段目の取得に失敗"));
    return "";
  }

  // GASが（リダイレクトせず）直接本文を返した場合はそのまま使う
  if (html.indexOf("Moved Temporarily") < 0 && html.indexOf("HREF=\"") < 0) {
    return html;
  }

  String url2 = extractRedirectUrl(html);
  if (url2.length() == 0) {
    Serial.println(F("[GAS] リダイレクト先URLが取り出せない"));
    return "";
  }

  // 2段目も1段目と同様にリトライする（リダイレクト先URLは時限性があるので使い回してよい）
  String body;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) delay(1000);
    Serial.println(attempt == 0 ? F("[GAS] 2段目（リダイレクト先）を取得中...")
                                : F("[GAS] 2段目を再取得中..."));
    body = httpGetViaFs(url2, true);
    if (body.length() > 0) break;
  }
#if DEBUG_MODE
  Serial.print(F("[DEBUG] 本文=[")); Serial.print(body); Serial.println(F("]"));
#endif
  return body;
}

// 応答だけ確認すればよい単発GET（報告系。本文は読まない）。
// ★GAS Web AppはdoGet()を実行し終えてからリダイレクトを返すため、
//   1段目が成功した時点でGAS側の処理（Script Propertiesの更新等）は完了している。
//   本文が不要なこの用途では、リダイレクトを追う必要がない。
static bool gasSimpleGet(const String& queryParams) {
  String url = "https://script.google.com/macros/s/";
  url += GAS_SCRIPT_ID;
  url += "?";
  url += queryParams;
  return httpGetViaFs(url, false).length() > 0;
}

// ============================================================
// ダウンリンク予約のキャッシュ更新・結果報告
// ============================================================

// GASから未完了の子機宛て予約を一括取得し、ローカルキャッシュへ反映する。
// 応答形式: 1行1件の "HEX2:sleepMin:avg:median:attempts:seq:mode"、無ければ "none"
//   例) "08:4:5:5:0:1:0\n0E:0:0:0:0:2:1"（0Eはステータス確認のみ、mode=1）
void refreshDownlinkCache() {
  String q = "action=check_downlinks&device_id=" + String(GW_DEVICE_ID);

  String body = gasGetText(q);
  if (body.length() == 0) {
    Serial.println(F("[CACHE] 予約の取得に失敗（次サイクルで再試行します）"));
    return;
  }
  if (body == "none") {
    Serial.println(F("[CACHE] 保留中のダウンリンク予約はありません"));
    // ★既存キャッシュは消さない。GAS側は確認が取れるまで予約を返し続ける仕様なので、
    //   "none" が返る＝すべて完了済み。ここでクリアしてよい。
    for (int i = 0; i < MAX_PENDING_CHILDREN; i++) s_pending[i].active = false;
    return;
  }

  // 取得した内容で丸ごと作り直す（試行回数も通し番号もGAS側が正）
  for (int i = 0; i < MAX_PENDING_CHILDREN; i++) s_pending[i].active = false;

  int slot = 0;
  int lineStart = 0;
  while (lineStart < (int)body.length() && slot < MAX_PENDING_CHILDREN) {
    int nl = body.indexOf('\n', lineStart);
    String line = (nl < 0) ? body.substring(lineStart) : body.substring(lineStart, nl);
    lineStart = (nl < 0) ? body.length() : nl + 1;
    line.trim();
    if (line.length() == 0) continue;

    // 形式: HEX2:sleepMin:avg:median:attempts:seq:mode（7項目）
    int pos[6];
    int found = 0;
    int from = 0;
    while (found < 6) {
      int c = line.indexOf(':', from);
      if (c < 0) break;
      pos[found++] = c;
      from = c + 1;
    }
    if (found < 6) {
      Serial.print(F("[CACHE] 書式不正のため無視: ")); Serial.println(line);
      continue;
    }

    uint8_t  childId    = (uint8_t)strtoul(line.substring(0, pos[0]).c_str(), nullptr, 16);
    uint16_t sleepMin   = (uint16_t)line.substring(pos[0] + 1, pos[1]).toInt();
    uint8_t  avg        = (uint8_t)line.substring(pos[1] + 1, pos[2]).toInt();
    uint8_t  median     = (uint8_t)line.substring(pos[2] + 1, pos[3]).toInt();
    uint8_t  attempts   = (uint8_t)line.substring(pos[3] + 1, pos[4]).toInt();
    uint32_t seq        = (uint32_t)line.substring(pos[4] + 1, pos[5]).toInt();
    bool     statusOnly = line.substring(pos[5] + 1).toInt() != 0;

    // ★ステータス確認(statusOnly)はsleep/avg/medianを使わない（GAS側は0を送ってくる）ので
    //   値域チェックの対象外にする。通常の設定変更だけ範囲を検証する。
    if (childId == 0) {
      Serial.print(F("[CACHE] 値が範囲外のため無視: ")); Serial.println(line);
      continue;
    }
    if (!statusOnly && (sleepMin < 1 || sleepMin > 1440 || avg < 1 || median < 1)) {
      Serial.print(F("[CACHE] 値が範囲外のため無視: ")); Serial.println(line);
      continue;
    }

    s_pending[slot].active     = true;
    s_pending[slot].childId    = childId;
    s_pending[slot].sleepMin   = sleepMin;
    s_pending[slot].avg        = avg;
    s_pending[slot].median     = median;
    s_pending[slot].attempts   = attempts;  // ★GAS側が正（Gatewayが再起動しても引き継がれる）
    s_pending[slot].seq        = seq;
    s_pending[slot].statusOnly = statusOnly;
    slot++;

    Serial.print(F("[CACHE] 予約: 子機0x")); Serial.print(childId, HEX);
    if (statusOnly) { Serial.print(F(" ステータス確認のみ")); }
    Serial.print(F(" 間隔=")); Serial.print(sleepMin);
    Serial.print(F("分 平均=")); Serial.print(avg);
    Serial.print(F(" メジアン=")); Serial.print(median);
    Serial.print(F(" 試行済=")); Serial.print(attempts);
    Serial.print(F(" seq=")); Serial.println(seq);
  }
  Serial.print(F("[CACHE] 有効な予約 ")); Serial.print(slot); Serial.println(F(" 件を保持しました"));
}

// 溜まった報告をGASへ送る。★loop()の安全な場所からのみ呼ぶこと
// （loraPoll()の中から呼ぶとAT通信が入れ子になって破綻する）。
void processReportQueue() {
  for (int i = 0; i < MAX_REPORTS; i++) {
    if (!s_reports[i].used) continue;

    // GAS側は child を大文字16進2桁で判定する（/^[0-9A-F]{2}$/）ため、ここで整形する
    char childHex[3];
    snprintf(childHex, sizeof(childHex), "%02X", s_reports[i].childId);

    String q;
    if (s_reports[i].finalResult) {
      q  = "action=downlink_result&child="; q += childHex;
      q += "&status=";   q += String(s_reports[i].status);
      q += "&sleep=";    q += String(s_reports[i].sleepMin);
      q += "&avg=";      q += String(s_reports[i].avg);
      q += "&median=";   q += String(s_reports[i].median);
      q += "&attempts="; q += String(s_reports[i].attempts);
      q += "&seq=";      q += String(s_reports[i].seq);
      q += "&wdt=";      q += String(s_reports[i].wdtMin);
    } else {
      q  = "action=downlink_sent&child="; q += childHex;
      q += "&attempts="; q += String(s_reports[i].attempts);
      q += "&seq=";      q += String(s_reports[i].seq);
    }

    Serial.print(F("[REPORT] GASへ報告: ")); Serial.println(q);
    if (gasSimpleGet(q)) {
      s_reports[i].used = false;
    } else {
      Serial.println(F("[REPORT] 報告に失敗。次サイクルで再送します"));
      return;  // 通信不調とみなし、残りは次回に回す
    }
  }
}

// ============================================================
// Arduino エントリ
// ============================================================
static void loraUartBegin() {
  loraSerial.begin(LORA_UART_BAUD);
  delay(500);
}


void setup() {
  wdtInit(WDT_TIMEOUT_MS);

  rgbHwBegin();
  statusBootBlue();
  delay(500);

  pinMode(LORA_M0M1_PIN, OUTPUT);
  digitalWrite(LORA_M0M1_PIN, LOW);  // Normalモードで起動

#if DEBUG_MODE
  Serial.begin(115200);
  for (int rep = 0; rep < 3; rep++) {
    delay(1000);
    Serial.println("\n[Step03] LoRaダウンリンク送信テストツール起動（Gateway v1.1実機版・GAS駆動）");
    Serial.print("GW_DEVICE_ID="); Serial.println(GW_DEVICE_ID);
    Serial.print(CHECK_CMD_INTERVAL_MS / 1000UL); Serial.println("秒間隔でGASのコマンドを確認します");
  }
#endif

  loraUartBegin();
  if (!loraCheckAndConfigure()) {
#if DEBUG_MODE
    Serial.println("[LORA] ★起動時の設定確認に失敗しました（配線・給電を確認してください）");
#endif
  }
  loraModeNormal();

  // ★起動時に必ず1回キックしてE220の受信不能ラッチを解除しておく。
  // これが無いと「設定コマンドには応答するのにRF受信だけ一切効かない」状態に
  // 陥ることがある（[[e220_rx_latch_on_battery_poweron]]、実機で再現済み）。
  loraKickTx();
  s_loraLastRxMs = millis();
  Serial.println(F("[LORA] 受信待機を開始します（ラッチ解除キック送信済み）"));

  // ── SIM7080G 初期化（gateway_v1.1と同一シーケンス） ──
  Serial.println(F("\n========== SIM7080G 初期化 =========="));
  Serial1.setPins(7, 6);  // RX=D7, TX=D6
  Serial1.begin(115200);
  for (int i = 0; i < 15; i++) {
    delay(1000);
    while (Serial1.available()) Serial1.read();
  }

  bool atOk = false;
  Serial.print(F("[   ] AT 疎通確認"));
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
  simStage("AT 疎通", atOk);
  if (!atOk) {
    Serial.println(F("  → 配線・電源を確認してください（D6=TX/D7=RX、5V供給）"));
    statusErrorRed();
    return;
  }

  sendAT("AT&F", 3000); delay(500);
  sendAT("AT+CFUN=1,1", 3000); delay(5000);

  // ★2026-08-10修正: AT+CFUN=1,1 はモジュールを再起動させるため、delay(5000)だけでは
  // 再起動が完了していないことがあり、直後のATE0が失われてエコーが有効なまま残っていた
  // （実機ログで、送信したコマンドがそのまま復唱されて返ってきていた）。
  //
  // 【エコーが致命的な理由】エコーが有効だと、こちらが500バイトのATコマンドを送っている
  // 最中にモジュールが同じ500バイトを返してくる。writeAtCommand()の送信中(約120ms)は
  // Serial1を読んでいないため、nRF52のUART受信リングバッファが溢れ、以降の応答受信まで
  // 巻き込んで壊れる。長いコマンドほど失敗しやすかった原因の一つ。
  {
    bool ready = false;
    for (int t = 0; t < 20 && !ready; t++) {
      String r = sendAT("AT", 1000);
      if (r.indexOf("OK") >= 0) ready = true;
      else delay(500);
    }
    simStage("モデム再起動後のAT疎通", ready);

    // エコーを確実に切る（効いたかどうかを応答の中身で検証する）
    bool echoOff = false;
    for (int t = 0; t < 5 && !echoOff; t++) {
      String r = sendAT("ATE0", 2000);
      // エコーが切れていれば応答にコマンド文字列("ATE0")は含まれない
      if (r.indexOf("OK") >= 0 && r.indexOf("ATE0") < 0) echoOff = true;
      else delay(300);
    }
    simStage("エコー無効化 (ATE0)", echoOff);
  }

  Serial.println(F("[   ] ネットワーク初期化..."));
  s_netOk = initNetwork();
  simStage("ネットワーク接続", s_netOk);
  Serial.println(F("=====================================\n"));

#if PROBE_PUBLISHED_CSV
  // 起動時に一度だけ、公開CSVを直接取得してみる（307追従の可否を実機で確認する）
  if (s_netOk) {
    Serial.println(F("\n===== 公開CSV 取得テスト（5回） ====="));
    // 307の追従自体は成功している（status=200が返る）。問題はchunked応答の本文長。
    // 常に0バイトなのか、GASと同様にたまに取れるのかを見る。
    int ok = 0;
    for (int i = 0; i < 5; i++) {
      String csv = httpGetViaFs(PUBLISHED_CSV_URL, true);
      Serial.print(F("[PROBE] ")); Serial.print(i + 1); Serial.print(F("回目: "));
      if (csv.length() > 0) { ok++; Serial.print(F("成功 本文=[")); Serial.print(csv); Serial.println(F("]")); }
      else Serial.println(F("0バイト"));
      loraDelay(2000);
    }
    Serial.print(F("[PROBE] 結果: 5回中 ")); Serial.print(ok); Serial.println(F(" 回成功"));
    Serial.println(F("================================\n"));
  }
#endif

  if (s_netOk) refreshDownlinkCache();

  statusIdle();
}

static unsigned long s_lastCheckMs = 0;
static unsigned long s_lastHeartbeatMs = 0;

static bool hasQueuedReports() {
  for (int i = 0; i < MAX_REPORTS; i++) if (s_reports[i].used) return true;
  return false;
}

void loop() {
  wdtFeed();

  // ★最優先。子機の受信窓は2秒しかないため、毎周回で必ずLoRa受信を処理する。
  // 予約があれば onUplinkReceived() の中でその場でダウンリンクを返す（ネットワークは介さない）。
  loraPoll();

  uint32_t now = millis();

#if DIAG_REPEAT_DOWNLINK
  // 【切り分け中】アップリンクを待たず、保留予約を一定間隔で送り続ける。
  // 試行回数は増やさない（打ち切られると送信が止まって切り分けにならないため）。
  {
    static uint32_t s_lastDiagSendMs = 0;
    if (now - s_lastDiagSendMs >= DIAG_REPEAT_INTERVAL_MS) {
      s_lastDiagSendMs = now;
      for (int i = 0; i < MAX_PENDING_CHILDREN; i++) {
        if (!s_pending[i].active) continue;
        Serial.print(F("[DIAG] 定期送信: 宛先=0x")); Serial.println(s_pending[i].childId, HEX);
        sendDownlinkCommand(s_pending[i].childId, s_pending[i].sleepMin,
                            s_pending[i].avg, s_pending[i].median, s_pending[i].statusOnly);
        break;  // 1周期につき1台だけ（連続送信で電波を占有しないため）
      }
    }
  }
#endif

  if (now - s_lastHeartbeatMs >= 10000) {
    s_lastHeartbeatMs = now;
    int activeCount = 0;
    for (int i = 0; i < MAX_PENDING_CHILDREN; i++) if (s_pending[i].active) activeCount++;
    Serial.print(F("[HB] 受信フレーム=")); Serial.print(s_loraFramesOk);
    Serial.print(F(" 保留予約=")); Serial.print(activeCount);
    Serial.print(F(" 未送信報告=")); Serial.print(hasQueuedReports() ? F("あり") : F("なし"));
    Serial.print(F(" 次回キャッシュ更新まで="));
    Serial.print((long)(CHECK_CMD_INTERVAL_MS - (now - s_lastCheckMs)));
    Serial.println(F("ms"));
  }

  // 報告はキャッシュ更新より優先して送る（スプレッドシートへの結果反映を早くするため）。
  //
  // ★2026-08-10修正: 従来はここで無条件にreturnしていたため、報告のGAS送信が
  // （302リダイレクト追跡の一時的な失敗等で）連続して失敗し続けると、下のキャッシュ
  // 更新チェックに永久に到達できなくなり、新しい予約を何分も拾えなくなるバグがあった
  // （実機で予約から送信まで約5分かかる事象として発覚）。
  // report処理を「試すだけ」にして、成功・失敗に関わらずキャッシュ更新のタイマー
  // チェックへ必ず進むようにする。
  if (s_netOk && hasQueuedReports()) {
    processReportQueue();
  }

  if (now - s_lastCheckMs >= CHECK_CMD_INTERVAL_MS) {
    s_lastCheckMs = now;
    if (!s_netOk) {
      s_netOk = initNetwork();
      if (!s_netOk) {
#if DEBUG_MODE
        Serial.println(F("[NET] 再接続失敗、次回リトライします"));
#endif
        return;
      }
    }
    refreshDownlinkCache();
  }
}
