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
static void sendDownlinkCommand(uint8_t targetDeviceId, uint16_t sleepMinutes,
                                 uint8_t samplesPerAvg, uint8_t measureCount) {
  loraModeNormal();  // Configモードへ入らず、Normalモードのまま送信する（実機確認済みの方式）

  uint8_t flags = DL_FLAG_SLEEP_MIN | DL_FLAG_AVG_MEDIAN;

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
  uint32_t seq;      // 予約の通し番号。報告に含めてGAS側で新旧を判別させる
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
};
#define MAX_REPORTS 8
static DownlinkReport s_reports[MAX_REPORTS];

static void queueReport(bool finalResult, uint8_t childId, uint8_t status,
                        uint16_t sleepMin, uint8_t avg, uint8_t median,
                        uint8_t attempts, uint32_t seq) {
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
    return;
  }
#if DEBUG_MODE
  Serial.println(F("[REPORT] キューが満杯のため報告を破棄しました"));
#endif
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
  sendDownlinkCommand(childId, p->sleepMin, p->avg, p->median);
  queueReport(false, childId, 0, p->sleepMin, p->avg, p->median, p->attempts, p->seq);
#endif  // DIAG_REPEAT_DOWNLINK
}

// 子機からの確認応答を受けた時の処理。予約を完了扱いにし、結果をGASへ報告する。
//   ack: [0]0x05 [1]DeviceID [2]status [3-4]適用sleepMin(BE) [5]適用avg [6]適用median
static void onDownlinkAckReceived(const uint8_t* ack, uint8_t len) {
  if (len < 7) return;
  uint8_t  childId = ack[1];
  uint8_t  status  = ack[2];
  uint16_t applied = ((uint16_t)ack[3] << 8) | ack[4];
  uint8_t  appliedAvg    = ack[5];
  uint8_t  appliedMedian = ack[6];

  PendingDownlink* p = findPending(childId);
  uint8_t  attempts = (p != nullptr) ? p->attempts : 0;
  uint32_t seq      = (p != nullptr) ? p->seq      : 0;

#if DEBUG_MODE
  Serial.print(F("[DOWNLINK] ★子機0x")); Serial.print(childId, HEX);
  Serial.print(F(" から確認応答: status=")); Serial.print(status);
  Serial.print(F(" 適用値 間隔=")); Serial.print(applied);
  Serial.print(F("分 平均=")); Serial.print(appliedAvg);
  Serial.print(F(" メジアン=")); Serial.println(appliedMedian);
#endif

  queueReport(true, childId, status, applied, appliedAvg, appliedMedian, attempts, seq);
  if (p != nullptr) p->active = false;  // 確認が取れたので再試行を止める
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

static String sendAT(String cmd, int waitMs = 5000, const char* waitForToken = nullptr) {
  Serial1.print(cmd + "\r\n");
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
  return res;
}

static String sendATFull(String cmd, int waitMs) {
  Serial1.print(cmd); Serial1.print("\r\n");
  String res = "";
  long start = millis();
  while (millis() - start < waitMs) {
    wdtFeed();
    while (Serial1.available()) res += (char)Serial1.read();
    loraPoll();
    yield();
  }
  return res;
}

// ★2026-08-08修正: 従来は本文以降の残りバイト全部（AT応答の終端"\r\n\r\nOK\r\n"を含む）を
// そのまま本文とみなしていたため、"none"のはずのcmdが"none\r\n\r\nOK"のような余分な文字列に
// なり、cmd=="none"の比較が一致せず異常な挙動（"OK"だけが残って見える等）を起こしていた。
// AT+SHREQの応答に含まれるDataLenが本文の正確なバイト数なので、それだけを切り出す。
static String extractShreadBody(const String& raw, int expectedLen) {
  int bi = raw.indexOf("+SHREAD: ");
  if (bi < 0) return "";
  int nlAfterLen = raw.indexOf('\n', bi);
  if (nlAfterLen < 0) return "";
  int bodyStart = nlAfterLen + 1;
  int available = raw.length() - bodyStart;
  int len = (expectedLen >= 0 && expectedLen < available) ? expectedLen : available;
  return raw.substring(bodyStart, bodyStart + len);
}

static void parseShreqResult(const String& result, int* outStatusCode, int* outDataLen) {
  *outStatusCode = 0;
  *outDataLen = 0;
  int si = result.indexOf("+SHREQ: ");
  if (si < 0) return;
  String s = result.substring(si + 8);
  int c1 = s.indexOf(",");
  int c2 = s.indexOf(",", c1 + 1);
  if (c1 < 0 || c2 < c1) return;
  *outStatusCode = s.substring(c1 + 1, c2).toInt();
  int c3 = c2 + 1;
  while (c3 < (int)s.length() && isDigit(s[c3])) c3++;
  *outDataLen = s.substring(c2 + 1, c3).toInt();
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

bool gasConnect() {
  Serial.println(F("[GAS] SSL設定中..."));
  sendAT("AT+SHDISC", 2000); delay(300);
  sendAT("AT+CSSLCFG=\"ignorertctime\",1,1"); delay(200);
  sendAT("AT+CSSLCFG=\"sslversion\",1,3");    delay(200);
  sendAT("AT+CSSLCFG=\"sni\",1,\"script.google.com\""); delay(200);
  sendAT("AT+SHSSL=1,\"\""); delay(200);
  sendAT("AT+SHCONF=\"BODYLEN\",1024");  delay(200);
  sendAT("AT+SHCONF=\"HEADERLEN\",350"); delay(200);
  sendAT("AT+SHCONF=\"URL\",\"https://script.google.com\""); delay(200);

  Serial.println(F("[GAS] 接続中(AT+SHCONN、最大15秒)..."));
  String conn = sendAT("AT+SHCONN", 15000);
  if (conn.indexOf("OK") < 0) {
    Serial.print(F("✗ 接続失敗（CSQ=")); Serial.print(getSimCsq()); Serial.println(F("）"));
    return false;
  }
  return true;
}

void gasDisconnect() { sendAT("AT+SHDISC"); }

// GASへGETし、応答本文を文字列で返す（失敗時は空文字列）。
// GAS Web Appは必ず一度302でscript.googleusercontent.comへリダイレクトされ、
// SIM7080GのAT+SHREQは3xxを自動追従しないため、ここで自前で追跡している
// （詳細は [[sim7080g_gas_webapp_redirect_handling]]）。
static String gasGetText(const String& queryParams) {
  if (!gasConnect()) return "";

  String path = "/macros/s/";
  path += GAS_SCRIPT_ID;
  path += "?";
  path += queryParams;

  String result = sendAT("AT+SHREQ=\"" + path + "\",1", 30000, "+SHREQ:");
  int statusCode = 0, dataLen = 0;
  parseShreqResult(result, &statusCode, &dataLen);

  if ((statusCode != 200 && statusCode != 302) || dataLen <= 0) {
    Serial.print(F("[GAS] 応答異常 ステータスコード=")); Serial.println(statusCode);
    gasDisconnect();
    return "";
  }

  String cmd;

  if (statusCode == 302) {
    // GAS Web AppのdoGet()は必ず一度302でscript.googleusercontent.com/macros/echo?...へ
    // リダイレクトされる。実際の応答本文はそちらにあるため自前で追いかける
    // （詳細はgateway_v1.1のcheckRemoteCmd()コメント、[[sim7080g_gas_webapp_redirect_handling]]参照）。
    String redirectBody = sendATFull("AT+SHREAD=0," + String(dataLen), 3000);
    gasDisconnect();
    String html = extractShreadBody(redirectBody, dataLen);
    int hi = html.indexOf("HREF=\"");
    if (hi < 0) { Serial.println(F("[GAS] リダイレクト先URLが見つからない")); return ""; }
    hi += 6;
    int hEnd = html.indexOf("\"", hi);
    if (hEnd < 0) { Serial.println(F("[GAS] リダイレクト先URLの終端が見つからない")); return ""; }
    String redirectUrl = html.substring(hi, hEnd);
    redirectUrl.replace("&amp;", "&");

    int hostStart = redirectUrl.indexOf("://") + 3;
    int pathStart = redirectUrl.indexOf("/", hostStart);
    if (pathStart < 0) { Serial.println(F("[GAS] リダイレクト先URLの形式が不正")); return ""; }
    String redirectHost = redirectUrl.substring(hostStart, pathStart);
    String redirectPath = redirectUrl.substring(pathStart);

    sendAT("AT+CSSLCFG=\"ignorertctime\",1,1", 200);
    sendAT("AT+CSSLCFG=\"sslversion\",1,3", 200);
    sendAT("AT+CSSLCFG=\"sni\",1,\"" + redirectHost + "\"", 200);
    sendAT("AT+SHSSL=1,\"\"", 200);
    sendAT("AT+SHCONF=\"URL\",\"https://" + redirectHost + "\"", 200);

    String conn2 = sendAT("AT+SHCONN", 15000);
    if (conn2.indexOf("OK") < 0) { Serial.println(F("[GAS] リダイレクト先への接続に失敗")); return ""; }

    String result2 = sendAT("AT+SHREQ=\"" + redirectPath + "\",1", 30000, "+SHREQ:");
    int statusCode2 = 0, dataLen2 = 0;
    parseShreqResult(result2, &statusCode2, &dataLen2);
    if (statusCode2 != 200 || dataLen2 <= 0) {
      Serial.print(F("[GAS] リダイレクト先の応答が異常 ステータスコード=")); Serial.println(statusCode2);
      gasDisconnect();
      return "";
    }

    String body2 = sendATFull("AT+SHREAD=0," + String(dataLen2), 3000);
    gasDisconnect();
    cmd = extractShreadBody(body2, dataLen2);
  } else {
    String body = sendATFull("AT+SHREAD=0," + String(dataLen), 3000);
    gasDisconnect();
    cmd = extractShreadBody(body, dataLen);
  }

  cmd.trim();
  return cmd;
}

// 応答だけ確認すればよい単発GET（報告系。本文は読まない）
static bool gasSimpleGet(const String& queryParams) {
  if (!gasConnect()) return false;
  String scriptPath = "/macros/s/";
  scriptPath += GAS_SCRIPT_ID;
  scriptPath += "?";
  scriptPath += queryParams;
  String result = sendAT("AT+SHREQ=\"" + scriptPath + "\",1", 30000, "+SHREQ:");
  int statusCode = 0, dataLen = 0;
  parseShreqResult(result, &statusCode, &dataLen);
  gasDisconnect();
  return statusCode == 200 || statusCode == 302;
}

// ============================================================
// ダウンリンク予約のキャッシュ更新・結果報告
// ============================================================

// GASから未完了の子機宛て予約を一括取得し、ローカルキャッシュへ反映する。
// 応答形式: 1行1件の "HEX2:sleepMin:avg:median"、無ければ "none"
//   例) "08:4:5:5\n0E:60:10:10"
void refreshDownlinkCache() {
  String body = gasGetText("action=check_downlinks&device_id=" + String(GW_DEVICE_ID));
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

    // 形式: HEX2:sleepMin:avg:median:attempts:seq（6項目）
    int pos[5];
    int found = 0;
    int from = 0;
    while (found < 5) {
      int c = line.indexOf(':', from);
      if (c < 0) break;
      pos[found++] = c;
      from = c + 1;
    }
    if (found < 5) {
      Serial.print(F("[CACHE] 書式不正のため無視: ")); Serial.println(line);
      continue;
    }

    uint8_t  childId  = (uint8_t)strtoul(line.substring(0, pos[0]).c_str(), nullptr, 16);
    uint16_t sleepMin = (uint16_t)line.substring(pos[0] + 1, pos[1]).toInt();
    uint8_t  avg      = (uint8_t)line.substring(pos[1] + 1, pos[2]).toInt();
    uint8_t  median   = (uint8_t)line.substring(pos[2] + 1, pos[3]).toInt();
    uint8_t  attempts = (uint8_t)line.substring(pos[3] + 1, pos[4]).toInt();
    uint32_t seq      = (uint32_t)line.substring(pos[4] + 1).toInt();

    if (childId == 0 || sleepMin < 1 || sleepMin > 1440 || avg < 1 || median < 1) {
      Serial.print(F("[CACHE] 値が範囲外のため無視: ")); Serial.println(line);
      continue;
    }

    s_pending[slot].active   = true;
    s_pending[slot].childId  = childId;
    s_pending[slot].sleepMin = sleepMin;
    s_pending[slot].avg      = avg;
    s_pending[slot].median   = median;
    s_pending[slot].attempts = attempts;  // ★GAS側が正（Gatewayが再起動しても引き継がれる）
    s_pending[slot].seq      = seq;
    slot++;

    Serial.print(F("[CACHE] 予約: 子機0x")); Serial.print(childId, HEX);
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

static bool s_netOk = false;

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
  sendAT("ATE0", 2000);

  Serial.println(F("[   ] ネットワーク初期化..."));
  s_netOk = initNetwork();
  simStage("ネットワーク接続", s_netOk);
  Serial.println(F("=====================================\n"));

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
                            s_pending[i].avg, s_pending[i].median);
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

  // 報告はキャッシュ更新より優先して送る（スプレッドシートへの結果反映を早くするため）
  if (s_netOk && hasQueuedReports()) {
    processReportQueue();
    return;
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
