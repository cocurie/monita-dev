/**
 * Monita Gateway v1.1 — BLE/LoRa 受信 + LTE-M → GAS 送信（AC電源版）
 *
 * MCU    : Seeed XIAO nRF52840
 * 通信   : M5Stamp CAT-M（SIM7080G）、（LoRaビルドのみ）E220-900T22S(JP)-EV2
 * RTC    : DS3231（I2C: D4=SDA, D5=SCL）
 * SD     : microSD SPI（D1=CS, SCK/MISO/MOSI=D8/D9/D10）
 * 電源   : XIAO nRF52840 Type-C給電（AC/USBアダプタ）。SIM7080GはXIAOの5Vへ直結、常時給電。
 *          全部品DIP対応。★2026-07-17: LiPoバッテリー駆動＋昇圧/ロードスイッチ/TCA9534構成から変更。
 *          旧設計は `gateway_v1.10_ARCHIVE_battery_TCA9534_design.md` にアーカイブ済み（復活する可能性あり）。
 *
 * 配線（v1.1 基板、AC電源版）:
 *   XIAO D6 (TX) → SIM7080G RX
 *   XIAO D7 (RX) ← SIM7080G TX
 *   XIAO D4(SDA) → DS3231 SDA（4.7kΩ プルアップ）
 *   XIAO D5(SCL) → DS3231 SCL（4.7kΩ プルアップ）
 *   XIAO 3V3     → DS3231 VCC / SD VDD
 *   XIAO 5V      → SIM7080G 5V（USB Type-C給電時のみ通電。v1.0と同じ直結方式）
 *   XIAO D3      → SD CS（直結、net N$6）
 *   XIAO D8(SCK) → SD CLK
 *   XIAO D9(MISO)→ SD DAT0
 *   XIAO D10(MOSI)→ SD CMD
 *   XIAO D0      ← （LoRaビルドのみ）E220 TXD（Gateway RX側、net UART_RX_2）
 *   XIAO D1      → （LoRaビルドのみ）E220 RXD（Gateway TX側、net UART_TX_2）
 *   XIAO D2      → （LoRaビルドのみ）E220 M0・M1 共通駆動（net LORA_SETTING）
 *                  ※ 本ファームはM0とM1を常に同じ値で駆動するため（Normal:両方LOW／Config:両方HIGH）、
 *                    E220基板上でM0・M1ピンを物理的に接続し、GPIO1本で両方駆動する（回路図 ver1.10 で短絡済み）
 *
 * v1.1 の主な変更点:
 *   - 電源をLiPoバッテリー駆動からAC電源（XIAO Type-C給電）に変更、全部品DIP化
 *   - それに伴いTCA9534・AO3401・MMBT3904・TPS61232・TPS22965・RC遅延回路一式を削除
 *   - SIM7080Gの電源投入シーケンス（P-MOSFET→昇圧→ロードスイッチ）は不要（XIAO 5Vに常時直結、v1.0方式に回帰）
 *   - PMOSFET制御・SD CS・ロードスイッチON用に導入していたTCA9534を撤去し、SD CSは直結ピン(D1)に戻した
 *   - LoRaのE220 M0/M1はTCA9534無しで直結する必要があるため、M0とM1を1本のGPIO(D3)にまとめる方式に変更
 *     （基板側でM0・M1ピンを物理的に短絡する必要がある。回路図修正時に反映すること）
 *   - BLE / LoRa（E220-900T22S(JP)）のビルド時選択（COMM_MODE_BLE / COMM_MODE_LORA）は維持
 *   - バッテリー駆動用のディープスリープ間欠動作は不要になったため対象外（AC電源の常時稼働、v1.0と同じ動作モデル）
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

// ============================================================
// 通信モード選択（v1.1新規）
// platformio.ini の build_flags で指定する:
//   -D COMM_MODE_BLE   … BLE スキャン受信モード（v1.0からの既定動作）
//   -D COMM_MODE_LORA  … LoRa(E220-900T22S(JP)) UART受信モード
// ============================================================
#if !defined(COMM_MODE_BLE) && !defined(COMM_MODE_LORA)
  #error "platformio.ini の build_flags に -D COMM_MODE_BLE または -D COMM_MODE_LORA を指定してください"
#endif

// BLE は両ビルドで使用する:
//   COMM_MODE_BLE  … Flex受信（Central/スキャナ）
//   COMM_MODE_LORA … コントローラー連携（Peripheral/GATTサーバ）。Flex受信はLoRaが担う
#include <bluefruit.h>

#ifdef COMM_MODE_LORA
// コントローラーからの設定変更（送信間隔）を再起動後も維持するための内蔵フラッシュ保存。
// ※ using namespace は付けない（SDライブラリの File 型と衝突するため、LittleFS の File は
//    Adafruit_LittleFS_Namespace::File と完全修飾で使う）
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#endif

// ── デバッグレベル ─────────────────────────────
// 0: 無効  1: ステージ結果のみ  2: AT コマンド生ログも表示
#define DEBUG_LEVEL 0

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

#include <Wire.h>
#include <RTClib.h>
#include <SD.h>
#include <SPI.h>

// ══════════════════════════════════════════════
// ▼ ユーザー設定
// ══════════════════════════════════════════════

// GAS スクリプトID（デプロイURLの "AKfycb..." 部分）
const char* GAS_SCRIPT_ID = "AKfycbw2IIQ1GyxtGh2Uis_zmwXW3VhftDy9HWKw5tSsUbwNOhNo6p9PnNv3ftfs3MvcMDT5ww/exec";

// LTE-M送信のON/OFF切替（★2026-07-23追加）
// false にすると SIM7080G の初期化・ネットワーク接続・GAS送信を一切行わず、
// BLE（またはLoRa）受信データを直接SDカードへ記録するだけの「SD記録のみモード」になる。
// SIM7080GのTX系統故障が疑われる現場での暫定運用（アンテナ交換ができない場合等）を想定。
#define LTEM_SEND_ENABLED true

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

#ifdef COMM_MODE_BLE
// BLE スキャン / 送信設定
// Gateway は USB-C 常時給電のため省電力を気にせずデューティ比をほぼ100%にする
// （interval と window をほぼ同値にすることでほぼ常時受信状態にし、取りこぼしを減らす）
static uint16_t const SCAN_INTERVAL_MS   = 100;     // スキャンインターバル (ms)
static uint16_t const SCAN_WINDOW_MS     = 100;      // スキャンウィンドウ (ms)
static uint8_t  const MFR_COMPANY_ID_H   = 0xFF;    // Flex の Company ID (上位)
static uint8_t  const MFR_COMPANY_ID_L   = 0xFF;    // Flex の Company ID (下位)
// Company ID 0xFFFF は Bluetooth 仕様上「未登録・テスト用」の予約値のため、
// 近隣の無関係な BLE 機器（他社のテスト機器等）が偶然同じ ID で
// Manufacturer Data を送信していると誤って拾ってしまうことがある。
// そのため Flex の MSD フォーマット（Pkt type・Device ID）でも二重に検証する。
static uint8_t  const EXPECTED_PKT_TYPE  = 0x03;             // Flex v3.03 BLE の Pkt type
#endif

#ifdef COMM_MODE_LORA
static uint8_t  const EXPECTED_PKT_TYPE  = 0x04;             // Flex v3.10 LoRa の Pkt type
#endif

// GAS 送信インターバル。LoRaビルドではコントローラーからBLE経由で変更可能（内蔵フラッシュに保存し
// 再起動後も維持）。BLEビルドではこの既定値のまま（変更手段なし）。
static uint32_t const SEND_INTERVAL_DEFAULT_MS = 300000;  // 既定 5 分
static uint32_t       sendIntervalMs           = SEND_INTERVAL_DEFAULT_MS;  // 実行時可変

// Pkt type と併せて二重チェックする Device ID ホワイトリスト（BLE/LoRa共通）
static uint8_t  const ALLOWED_DEVICE_IDS[] = {0x01, 0x02};   // ★子機を増やしたらここに追加する
static size_t   const ALLOWED_DEVICE_IDS_COUNT = sizeof(ALLOWED_DEVICE_IDS) / sizeof(ALLOWED_DEVICE_IDS[0]);

// Gateway（本ファーム）自身のバージョン。コミットのたびに+1すること。
// info行（row_type=info）でGASへ送信し、GAS側のシートで実機バージョンを追跡できるようにする。
static uint8_t  const GATEWAY_FW_VERSION = 13;

// pktType・deviceId が Flex として許可された組み合わせか判定する
bool isAllowedFlexPacket(uint8_t pktType, uint8_t deviceId) {
  if (pktType != EXPECTED_PKT_TYPE) return false;
  for (size_t i = 0; i < ALLOWED_DEVICE_IDS_COUNT; i++) {
    if (deviceId == ALLOWED_DEVICE_IDS[i]) return true;
  }
  return false;
}

// ── ピン割当（v1.1 基板、AC電源版）─────────────────
// ★ TCA9534を撤去し、全て直結ピンに戻した（詳細はファイル冒頭コメント参照）。
// ★2026-07-19: 回路図 ver1.10.sch（netlist_gateway_1）に合わせて確定。
//   D0=LoRa RX, D1=LoRa TX, D2=M0/M1, D3=SD CS（旧割当から入れ替え）。
static int const SD_CS_PIN = 3;  // D3（直結、net N$6）

#ifdef COMM_MODE_LORA
static int const LORA_RX_PIN   = 0;  // D0: E220 TXD → XIAO RX（net UART_RX_2）
static int const LORA_TX_PIN   = 1;  // D1: XIAO TX → E220 RXD（net UART_TX_2）
static int const LORA_M0M1_PIN = 2;  // D2: E220 M0・M1 共通駆動（基板側でM0/M1短絡済み、net LORA_SETTING）
#endif

// ══════════════════════════════════════════════
// BLE 受信バッファ
// ══════════════════════════════════════════════
#define MAX_DEVICES 20
// v3.03 の MSD は Company ID(2B) を除くと 19 バイト
// （PktType+DeviceID+FW_VERSION+CH1-4+BATT+Hour+Min+CH1-4Range）。
// 将来の拡張余地を見て 24 バイトを確保する。
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

// コントローラー向けステータス表示用のキャッシュ（BLE status characteristic へ反映）
static int  s_lastCsq   = 99;     // 最後に取得した SIM7080G の CSQ（0-31, 99=圏外）
static bool s_lastNetOk = false;  // 最後のネットワーク接続結果

// ══════════════════════════════════════════════
// RTC
// ══════════════════════════════════════════════
String sendAT(String cmd, int waitMs);  // 後方で定義（RTC の網時刻同期から使うため前方宣言。デフォルト引数は本体側のみで指定）

static RTC_DS3231 rtc;
static bool rtcAvailable = false;
static bool s_rtcNeedsTimeSet = false;  // lostPower() 検知時にtrue。ネットワーク接続後にAT+CCLKで時刻セットする

String getTimestamp() {
  if (!rtcAvailable) return String(millis() / 1000UL) + "s";
  DateTime now = rtc.now();
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute(), now.second());
  return String(buf);
}

// SIM7080G の AT+CCLK?（網時刻）を取得し、有効な応答であれば DS3231 に反映する。
// 応答形式: +CCLK: "yy/MM/dd,hh:mm:ss+zz"（zzはUTCオフセット15分単位、符号あり）
bool syncRtcFromNetworkTime() {
  String res = sendAT("AT+CCLK?", 3000);
  int idx = res.indexOf("+CCLK: \"");
  if (idx < 0) return false;
  idx += 8;  // "+CCLK: \"" の直後
  if ((int)res.length() < idx + 17) return false;

  int yy  = res.substring(idx,      idx + 2).toInt();
  int mon = res.substring(idx + 3,  idx + 5).toInt();
  int day = res.substring(idx + 6,  idx + 8).toInt();
  int hh  = res.substring(idx + 9,  idx + 11).toInt();
  int mi  = res.substring(idx + 12, idx + 14).toInt();
  int ss  = res.substring(idx + 15, idx + 17).toInt();
  if (mon < 1 || mon > 12 || day < 1 || day > 31) return false;  // 未同期時は 80/01/06 等の無効値を返すため弾く

  rtc.adjust(DateTime(2000 + yy, mon, day, hh, mi, ss));
  Serial.print(F("✓ DS3231 を網時刻で設定: "));
  Serial.println(getTimestamp());
  return true;
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
// しかし実運用で発生した障害は、モデムはネットワークに接続したまま、GASへの送信
// （SHCONN/SHREQ）だけが失敗し続け、ファームは送信リトライのループを回して
// wdtFeedを呼び続ける「ソフトハング」だった。この場合ハードWDTには餌が入り続けるため
// リセットがかからず、無人現場では復旧不能になった（有野川現場、2026-07-17〜）。
//
// 対策として「一定時間 GAS 送信が1回も成功しなかったら NVIC_SystemReset で強制再起動」
// するアプリ層ウォッチドッグを設ける。再起動後は setup() が AT&F + CFUN=1,1 で
// モデムもソフトリセットするため、モデム側スタックの詰まりも合わせて解消される。
// ══════════════════════════════════════════════
static uint32_t const APP_WDT_NO_SEND_RESET_MS = 1800000UL;  // 30分: この間GAS送信成功が無ければ再起動
static uint32_t lastGasSuccessMs = 0;  // 最後にGAS送信が成功した millis()（setup先頭で初期化）

// 段階的復旧（★2026-07-21 追加、有野川障害の教訓）:
// アプリWDTの「30分無送信で全再起動(NVIC_SystemReset)」の前に、より軽く速い一段目として
// 「送信サイクルが規定回数連続で全滅したら、モデムだけソフトリセット(CFUN=0/1)して再接続」を挟む。
// 有野川で疑われたモデムのSSL/HTTPスタック固着（CGATTは正常＝アタッチ維持のまま送信だけ失敗）は、
// setup()でしか実行されないモデムリセットに降りていけず永続化した。この一段目で送信失敗時にも
// モデムリセットへ降りられるようにする。これでも復旧しなければ30分の全再起動が最終backstop。
static int const MODEM_RESET_FAIL_THRESHOLD = 3;  // 連続で全滅した送信サイクル数（5分間隔なら約15分）
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
static int      const RF_PROTECT_CSQ99_THRESHOLD = 3;         // CSQ=99 が連続この回数で保護発動
static uint32_t const RF_PROTECT_COOLDOWN_MS     = 1800000UL; // 30分 RF停止して待機
static int      s_csq99Count       = 0;
static bool     s_rfProtected      = false;
static uint32_t s_rfProtectStartMs = 0;

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

#ifdef COMM_MODE_LORA
// LoRa 受信ポーリング（後方で定義）。sendAT() の待機ループ中にも呼ぶことで、
// LTE-M送信中（数十秒〜数分）のUARTE1受信バッファ溢れによる取りこぼしを防ぐ。
static void loraPoll();
#endif

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
#ifdef COMM_MODE_LORA
    loraPoll();
#endif
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
// レコード更新（BLE/LoRa共通）
//
// mac は識別キー。BLEは実MACアドレス、LoRaはMACを持たないため
// {0,0,0,0,0,DeviceID} の疑似MACで代用する（Device IDで一意性を担保）。
// ══════════════════════════════════════════════
static void updateRecordFromPayload(const uint8_t mac[6], const uint8_t *payload, uint8_t payloadLen, int rssi) {
  if (xSemaphoreTake(recordMutex, 0) == pdTRUE) {
    int idx = -1;
    for (int i = 0; i < recordCount; i++) {
      if (memcmp(records[i].mac, mac, 6) == 0) { idx = i; break; }
    }
    if (idx < 0 && recordCount < MAX_DEVICES) idx = recordCount++;
    if (idx >= 0) {
      memcpy(records[idx].mac, mac, 6);
      memcpy(records[idx].payload, payload, payloadLen);
      records[idx].payloadLen = payloadLen;
      records[idx].rssi       = rssi;
      records[idx].lastSeen   = millis();
    }
    xSemaphoreGive(recordMutex);
  }

#if !LTEM_SEND_ENABLED
  // LTE-M送信が無効な「SD記録のみモード」では、GAS送信サイクルを待たず
  // 受信のたびに即座にSDへ記録する（flushRecords()は呼ばれないため）。
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X-%02X-%02X-%02X-%02X-%02X",
           mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
  String hex = "";
  for (uint8_t j = 0; j < payloadLen; j++) {
    if (payload[j] < 0x10) hex += '0';
    hex += String(payload[j], HEX);
  }
  sdLog(getTimestamp() + "," + String(macStr) + "," + hex + "," + String(rssi));
#endif
}

#ifdef COMM_MODE_BLE
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
    if (fieldLen == 0) break;
    if (i + fieldLen + 1 > len) break;  // 破損/不正パケットによるバッファ外読み出しを防ぐ
    uint8_t fieldType = data[i + 1];
    if (fieldType == 0xFF && fieldLen >= 3) {  // AD Type: Manufacturer Specific
      if (data[i + 2] == MFR_COMPANY_ID_L && data[i + 3] == MFR_COMPANY_ID_H) {
        msd    = &data[i + 4];               // Company ID の後ろがペイロード
        msdLen = fieldLen - 3;
        if (msdLen > MAX_PAYLOAD) msdLen = MAX_PAYLOAD;
      }
    }
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

  updateRecordFromPayload(report->peer_addr.addr, msd, msdLen, report->rssi);

  Bluefruit.Scanner.resume();
}
#endif  // COMM_MODE_BLE

#ifdef COMM_MODE_LORA
// ══════════════════════════════════════════════
// LoRa（E220-900T22S(JP)）受信（v1.1新規）
//
// Flex側（Monita_Flex_構成_v3.10.md §6、v3.10_lora/main.cpp）が送るフレーム:
//   [0]SYNC=0xAA [1]LEN [2..LEN+1]MSDペイロード [LEN+2]チェックサム(単純和)
// MSDペイロードはBLEのMSDからCompany ID(2B)を除いた部分と同一レイアウト
//   [0]PktType [1]DeviceID [2]FWVersion [3-10]CH1-4(int16 LE) [11-12]BATT
//   [13]Hour [14]Min [15-18]CH1-4 Range
//
// M0・M1はGPIO1本（D3）で共通駆動する（本ファームは常にM0=M1で駆動するため、
// 基板側でE220のM0・M1ピンを物理的に短絡しておくこと。回路図に反映要）。
// UARTE1（第2ハードウェアUART）をRX専用として使う。
// ★実機未検証: UARTE1経由の受信・チェックサム・M0/M1タイミングは基板完成後に実測・確認すること。
// ══════════════════════════════════════════════
#define LORA_MODE_SWITCH_DELAY_MS 100U  // 暫定値、要実測

static Uart loraSerial(NRF_UARTE1, UARTE1_IRQn, LORA_RX_PIN, LORA_TX_PIN);

// ★2026-07-23: Adafruit nRF52コアで第2UART(UARTE1)を自前で使う場合、割り込みハンドラを
// このように手動で転送しないと send/receive の完了通知が届かず、write()が2バイト目以降で
// 永久にブロックする（1バイト目は空バッファへ直接載るため気づかれにくい）。
// 実機デバッグで loraSerial.write() が2回目の呼び出しで無期限にハングすることを確認し、
// この転送関数が抜けていたことが原因と特定した。
extern "C" void UARTE1_IRQHandler(void) {
  loraSerial.IrqHandler();
}

static bool loraSetMode(bool high) {
  digitalWrite(LORA_M0M1_PIN, high ? HIGH : LOW);
  delay(LORA_MODE_SWITCH_DELAY_MS);
  return true;
}
static inline bool loraModeNormal() { return loraSetMode(false); }
static inline bool loraModeConfig() { return loraSetMode(true); }

// 設定コマンド（Flex側 v3.10_lora/main.cpp と同一値。全台共通）。
// レジスタ配置は E220-900T22S(JP) 公式データシート（CLEALINK TECHNOLOGY、Rev.2.1.1）
// 表8〜11で確認済み（2026-07-17、test_sketches 18〜21 で実機検証済み）:
//   0x03 REG1: bit1:0=送信出力(00=Not available/01=13dBm(default)/10=7dBm/11=0dBm)
//   0x05 REG3: bit7=RSSIバイト有効化(0=無効(default)/1=有効) / bit6=送信方式(0=透過(default)/1=固定)
// ★実機の工場出荷状態はREG1=0x00(送信出力Not available)・REG3=0x40(固定送信モード)
//   という異常値だったため、透過送信・有効な送信出力になるよう明示的に書き込む。
// ★Gateway（受信側）はRSSIバイト有効化(REG3 bit7=1)も設定し、受信データの直後に
//   付加されるRSSIバイトをloraPoll()で読み取ってrecords[].rssiに反映する。
#define LORA_CFG_REG_START 0x00
#define LORA_CFG_REG_LEN   6
static const uint8_t LORA_CFG_ADDH = 0x00;
static const uint8_t LORA_CFG_ADDL = 0x00;
static const uint8_t LORA_CFG_REG0 = 0x68;  // UART9600bps + エア速度(SF7/BW125kHz、実機確認値)
static const uint8_t LORA_CFG_REG1 = 0x01;  // ペイロード長200B(default)/RSSIノイズ無効/送信出力13dBm
static const uint8_t LORA_CFG_REG2 = 0x00;  // チャンネル0
static const uint8_t LORA_CFG_REG3 = 0x80;  // RSSIバイト有効化ON/透過送信モード

static bool loraReadConfig(uint8_t *out6) {
  // ★2026-07-23: 受信バッファの掃除ループに時間制限が無く、E220が継続的にバイトを
  // 送り続ける状態（Configモードへの切替失敗等でNormalモードのまま無線ノイズを
  // 垂れ流している場合等）になるとここで無限ループしフリーズすることが判明。
  // 掃除は最大300msまでとし、それでも終わらなければ異常とみなして打ち切る。
  {
    unsigned long drainStart = millis();
    while (loraSerial.available()) {
      loraSerial.read();
      if (millis() - drainStart > 300UL) {
        Serial.println(F("[LORA] 受信バッファの掃除がタイムアウト（E220がConfigモードに"
                          "切り替わっていない、またはノイズを継続受信している可能性）"));
        break;
      }
    }
  }
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

// デバッグ用: 6バイトをHEXで出力（期待値と実測値の突き合わせに使う）
static void loraPrintRegs(const char* label, const uint8_t regs[LORA_CFG_REG_LEN]) {
  Serial.print(F("[LORA] ")); Serial.print(label); Serial.print(F(": "));
  for (int i = 0; i < LORA_CFG_REG_LEN; i++) {
    if (regs[i] < 0x10) Serial.print('0');
    Serial.print(regs[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
}

// 起動時に1回呼ぶ（Gatewayは常時稼働のためFlexのように毎起床では確認しない）。
// 現在の設定値を確認し、想定値と異なれば書き込む。
static bool loraCheckAndConfigure() {
  if (!loraModeConfig()) return false;

  uint8_t cur[LORA_CFG_REG_LEN] = {0};
  bool readOk = loraReadConfig(cur);
  bool matches = readOk &&
      cur[0] == LORA_CFG_ADDH && cur[1] == LORA_CFG_ADDL &&
      cur[2] == LORA_CFG_REG0 && cur[3] == LORA_CFG_REG1 &&
      cur[4] == LORA_CFG_REG2 && cur[5] == LORA_CFG_REG3;

  Serial.print(F("[LORA] config read "));
  Serial.println(!readOk ? F("失敗") : (matches ? F("一致") : F("不一致→書込")));
  if (readOk) {
    loraPrintRegs("実測値(読込)", cur);
    uint8_t expected[LORA_CFG_REG_LEN] = {LORA_CFG_ADDH, LORA_CFG_ADDL, LORA_CFG_REG0,
                                           LORA_CFG_REG1, LORA_CFG_REG2, LORA_CFG_REG3};
    loraPrintRegs("期待値      ", expected);
  }

  if (!readOk) { loraModeNormal(); return false; }

  if (!matches) {
    // ★2026-07-23: 書込直後の確認読み込みがタイミング次第で失敗することがある
    // （E220内部のレジスタ書込処理完了前に読み返してしまう等）と実機で確認したため、
    // 最大2回まで「書込→確認」をリトライする。
    bool verifyOk = false;
    for (int attempt = 1; attempt <= 2 && !verifyOk; attempt++) {
      loraWriteConfig();
      // 書込後に読み返して実際に反映されたか確認する（配線不良等で書込が効いていないケースの検出）
      uint8_t verify[LORA_CFG_REG_LEN] = {0};
      bool verifyReadOk = loraReadConfig(verify);
      verifyOk = verifyReadOk &&
          verify[0] == LORA_CFG_ADDH && verify[1] == LORA_CFG_ADDL &&
          verify[2] == LORA_CFG_REG0 && verify[3] == LORA_CFG_REG1 &&
          verify[4] == LORA_CFG_REG2 && verify[5] == LORA_CFG_REG3;

      Serial.print(F("[LORA] config write 確認("));
      Serial.print(attempt); Serial.print(F("/2): "));
      Serial.println(verifyOk ? F("OK") : F("NG"));
      if (verifyReadOk) loraPrintRegs("書込後の実測値", verify);
      else              Serial.println(F("[LORA] 書込後の読込自体に失敗（応答なし）"));
    }
    if (!verifyOk) {
      Serial.println(F("[LORA] 2回とも書込確認NG（配線・電源を確認）"));
      loraModeNormal();
      return false;
    }
  }

  return loraModeNormal();
}

// 受信フレームの組み立てバッファ（状態機械）
//
// E220はREG3(bit7=RSSIバイト有効化)の設定により、受信データの直後にRSSIバイトを
// 自動付加する（dBm = RSSIバイト－256、公式データシート記載の式）。
// そのためチェックサム確認後、もう1バイト（RSSI）を待ってからフレーム完成とする。
//
// ★E220-900T22S(JP)の受信感度限界は公式データシート記載でおよそ-140dBm付近
//   （それを下回ると復調できずパケットロスになる）。実運用でRSSIが-140dBm近く
//   まで下がってくる現場では、送信距離・アンテナ設置・障害物を見直すこと。
//   （2026-07-17、test_sketches 18〜21 での実機検証・データシート確認より）
// フレーム途中（SYNC受信後〜RSSI受信前）で一定時間バイトが届かない場合、
// バイト抜け等で永久に詰まった状態になるのを防ぐため探索状態へ強制的に戻す
// （19_lora_parentで発見・対策した「途中で受信が止まり続きのバイトが来ない」
// 問題と同種の対策。2026-07-17追加）。
#define LORA_FIELD_TIMEOUT_MS 500UL

enum LoraRxState { LORA_WAIT_SYNC, LORA_WAIT_LEN, LORA_WAIT_BODY, LORA_WAIT_CKSUM, LORA_WAIT_RSSI };
static LoraRxState s_loraState = LORA_WAIT_SYNC;
static uint8_t     s_loraLen = 0;
static uint8_t     s_loraBody[MAX_PAYLOAD];
static uint8_t     s_loraBodyIdx = 0;
static uint8_t     s_loraSum = 0;
static uint8_t     s_loraRssiRaw = 0;
static uint32_t    s_loraFieldStartMs = 0;

// 1バイト処理して、フレーム＋RSSIまで完成したら true を返す
// （s_loraBody[0..s_loraLen-1]・s_loraRssiRaw が有効）
static bool loraFeedByte(uint8_t b) {
  switch (s_loraState) {
    case LORA_WAIT_SYNC:
      if (b == 0xAA) { s_loraSum = b; s_loraState = LORA_WAIT_LEN; s_loraFieldStartMs = millis(); }
      return false;
    case LORA_WAIT_LEN:
      s_loraLen = b;
      s_loraSum = (uint8_t)(s_loraSum + b);
      s_loraBodyIdx = 0;
      if (s_loraLen == 0 || s_loraLen > MAX_PAYLOAD) { s_loraState = LORA_WAIT_SYNC; return false; }
      s_loraState = LORA_WAIT_BODY;
      return false;
    case LORA_WAIT_BODY:
      s_loraBody[s_loraBodyIdx++] = b;
      s_loraSum = (uint8_t)(s_loraSum + b);
      if (s_loraBodyIdx >= s_loraLen) s_loraState = LORA_WAIT_CKSUM;
      return false;
    case LORA_WAIT_CKSUM:
      if (b != s_loraSum) { s_loraState = LORA_WAIT_SYNC; return false; }  // チェックサム不一致は破棄
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

// loop() から毎回呼ぶ。受信バッファを読み切り、完成したフレームがあればレコードへ反映する。
static void loraPoll() {
  while (loraSerial.available()) {
    uint8_t b = (uint8_t)loraSerial.read();
    if (loraFeedByte(b)) {
      uint8_t pktType  = s_loraBody[0];
      uint8_t deviceId = s_loraBody[1];
      if (!isAllowedFlexPacket(pktType, deviceId)) {
        DLOG2("[LORA] 未登録のPktType/DeviceIDを無視");
        continue;
      }
      int rssiDbm = (int)s_loraRssiRaw - 256;
      Serial.print(F("[LORA] フレーム受信 Device ID=0x"));
      if (deviceId < 0x10) Serial.print('0');
      Serial.print(deviceId, HEX);
      Serial.print(F(" RSSI="));
      Serial.print(rssiDbm);
      Serial.println(F("dBm"));

      // LoRaにはBLEのようなMACアドレスが無いため、DeviceIDで一意化した疑似MACを使う
      uint8_t pseudoMac[6] = {0, 0, 0, 0, 0, deviceId};
      updateRecordFromPayload(pseudoMac, s_loraBody, s_loraLen, rssiDbm);
    }
  }

  // フレーム途中で一定時間バイトが届かない場合は同期探索状態へ強制的に戻す
  // （バイト抜け等で永久に詰まった状態になるのを防ぐ）
  if (s_loraState != LORA_WAIT_SYNC && millis() - s_loraFieldStartMs > LORA_FIELD_TIMEOUT_MS) {
    DLOG2("[LORA] フレーム途中でタイムアウト。再同期します");
    s_loraState = LORA_WAIT_SYNC;
  }
}
#endif  // COMM_MODE_LORA

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
  params += String(sendIntervalMs / 60000UL);
  params += "&devcount=";
  params += String(recordCount);
  params += "&gw_fw=";
  params += String(GATEWAY_FW_VERSION);

  postToGAS(params);
}

// ══════════════════════════════════════════════
// バッファを GAS へ送信＆SD へ記録（全台を1回の POST にまとめる）
//
// 送信失敗時は再送キュー（pendingRecords）に保持し、次回サイクルで
// ライブ受信データとマージして再送する（同一 MAC はライブ側を優先）。
// これにより一時的な通信断でデータをロストしない。
// ══════════════════════════════════════════════
// 1回のGETに含める最大台数。台数が多いとクエリ文字列がSIM7080GのAT+SHREQ長制限
// （HEADERLEN/BODYLENおよびAT実装上の実用上限）に達し送信が黙って失敗しうるため、
// この件数ごとに複数回のGETへ分割する。
static int const MAX_DEVICES_PER_REQUEST = 8;

// merged[0..n-1] のうち [start, start+count) だけを1回のGETで送信する
bool postBatch(const FlexRecord* merged, int start, int count, const String& ts, int csq) {
  String params = "ts=";
  params += ts;
  params += "&sim=";
  params += SIM_NAME;
  params += "&csq=";
  params += String(csq);
  params += "&n=";
  params += String(count);

  for (int k = 0; k < count; k++) {
    const FlexRecord& rec = merged[start + k];
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X-%02X-%02X-%02X-%02X-%02X",
             rec.mac[5], rec.mac[4], rec.mac[3], rec.mac[2], rec.mac[1], rec.mac[0]);

    String hex = "";
    for (int j = 0; j < rec.payloadLen; j++) {
      if (rec.payload[j] < 0x10) hex += '0';
      hex += String(rec.payload[j], HEX);
    }

    params += "&m"; params += k; params += "="; params += mac;
    params += "&p"; params += k; params += "="; params += hex;
    params += "&r"; params += k; params += "="; params += String(rec.rssi);
    // ※ SD記録はネットワーク確認より前に flushRecords() 内で実施済み
    //   （LTE-M停止時もデータを残すため）
  }

  bool ok = postToGAS(params);
  if (!ok) {
    // 一時的な通信不良を想定し、30秒待って1回だけ即時リトライ
    Serial.println(F("✗ 送信失敗。30秒後に再試行..."));
    delay(30000);
    ok = postToGAS(params);
  }
  return ok;
}

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
  s_lastCsq = csq;  // コントローラー向けステータス用にキャッシュ

  // クエリ長がAT+SHREQの実用上限に達しないよう、MAX_DEVICES_PER_REQUEST台ずつに分割して送信する
  FlexRecord failedMerged[MAX_DEVICES];
  int failedN = 0;
  bool cycleHadSuccess = false;  // このサイクルで1バッチでも送信成功したか
  for (int start = 0; start < n; start += MAX_DEVICES_PER_REQUEST) {
    int count = n - start;
    if (count > MAX_DEVICES_PER_REQUEST) count = MAX_DEVICES_PER_REQUEST;

    bool ok = postBatch(merged, start, count, ts, csq);
    if (ok) {
      cycleHadSuccess = true;
      Serial.print(F("✓ 送信成功（")); Serial.print(count); Serial.println(F(" 件、再送キュー クリア）"));
    } else {
      Serial.print(F("✗ 再試行も失敗。")); Serial.print(count);
      Serial.println(F(" 件を次回送信サイクルで再送キューとしてリトライします"));
      for (int k = 0; k < count && failedN < MAX_DEVICES; k++) failedMerged[failedN++] = merged[start + k];
    }
  }

  pendingCount = failedN;
  if (failedN > 0) memcpy(pendingRecords, failedMerged, sizeof(FlexRecord) * failedN);

  // 段階的復旧: 送信サイクルが全滅したら連続失敗をカウント。閾値でモデムをソフトリセット。
  if (cycleHadSuccess) {
    consecutiveSendFailures = 0;
  } else {
    consecutiveSendFailures++;
    Serial.print(F("連続送信失敗: ")); Serial.print(consecutiveSendFailures);
    Serial.print(F("/")); Serial.println(MODEM_RESET_FAIL_THRESHOLD);
    if (consecutiveSendFailures >= MODEM_RESET_FAIL_THRESHOLD) {
      modemSoftReset();
      consecutiveSendFailures = 0;  // リセットを試みたので一旦クリア（それでも失敗が続けば再度カウント→再試行）
    }
  }
}

static uint32_t lastSend = 0;

#ifdef COMM_MODE_LORA
// ══════════════════════════════════════════════
// コントローラー連携（BLE GATT サーバ）— LoRaビルド専用（v1.1新規）
//
// LoRaビルドではBLE無線がFlex受信に使われないため、これをコントローラー(Monitaコントローラー)
// との設定通信用のGATTペリフェラルとして使う。GPIOは消費しない（内蔵無線のみ）。
//
// ★暫定インターフェース仕様（コントローラー側ファームと合わせること）:
//   サービスUUID:  6f5e4d3c-2b1a-9e8d-7c6b-01000000-... （下記配列、[12]バイトで各要素を区別）
//   - 送信間隔設定 (Write, uint16 LE, 単位:分)   [12]=0x02  … 0は無視。書込で即保存・反映
//   - コマンド     (Write, uint8)               [12]=0x03  … 0x01=即時送信 0x02=NW再登録
//                                                            0x03=起動確認送信 0xFF=フルリセット
//   - ステータス   (Read/Notify, 8バイト)        [12]=0x04  … 下記 buildStatus() 参照
//
// ★セキュリティ: 動作確認優先のため現段階は無認証（SECMODE_OPEN）。実運用前にボンディング必須化する。
// ★重い処理（送信・再登録・リセット）はBLEコールバック内で直接実行せず、フラグを立てて
//   loop()先頭の handlePendingBleCommands() で実行する（BLEスタックをブロックしないため）。
// ══════════════════════════════════════════════

// 128bit UUID（LSB→MSB順）。[12]バイトでサービス/各キャラクタリスティックを区別する。
#define GWCFG_UUID(code) { \
  0x00,0x00,0x00,0x00,0x6b,0x7c,0x8d,0x9e, \
  0x1a,0x2b,0x3c,0x4d,(code),0x00,0x5e,0x6f }
static const uint8_t GWCFG_UUID_SVC[16]      = GWCFG_UUID(0x01);
static const uint8_t GWCFG_UUID_INTERVAL[16] = GWCFG_UUID(0x02);
static const uint8_t GWCFG_UUID_COMMAND[16]  = GWCFG_UUID(0x03);
static const uint8_t GWCFG_UUID_STATUS[16]   = GWCFG_UUID(0x04);

static BLEService        gwCfgService(GWCFG_UUID_SVC);
static BLECharacteristic gwCharInterval(GWCFG_UUID_INTERVAL);
static BLECharacteristic gwCharCommand(GWCFG_UUID_COMMAND);
static BLECharacteristic gwCharStatus(GWCFG_UUID_STATUS);

// BLEコールバックからloop()へ渡す保留要求（コールバックはこれを立てるだけ）
static volatile bool     s_pendingIntervalChange = false;
static volatile uint32_t s_pendingIntervalMs     = 0;
static volatile uint8_t  s_pendingCommand         = 0;  // 0=なし

#define GWCFG_FILENAME  "/gwcfg.bin"
#define GWCFG_MAGIC     0x4D47  // 'MG'

// 内蔵フラッシュから送信間隔を読み込む（無ければ既定値のまま）
static void loadConfig() {
  InternalFS.begin();
  Adafruit_LittleFS_Namespace::File f(InternalFS);
  f = InternalFS.open(GWCFG_FILENAME, Adafruit_LittleFS_Namespace::FILE_O_READ);
  if (!f) return;
  uint16_t magic = 0; uint32_t val = 0;
  bool ok = (f.read((uint8_t*)&magic, sizeof(magic)) == (int)sizeof(magic)) &&
            (f.read((uint8_t*)&val,   sizeof(val))   == (int)sizeof(val));
  f.close();
  if (ok && magic == GWCFG_MAGIC && val >= 60000UL) {  // 1分未満は誤設定として弾く
    sendIntervalMs = val;
    Serial.print(F("[CFG] 保存済み送信間隔を読込: ")); Serial.print(val / 60000UL); Serial.println(F("分"));
  }
}

// 現在の送信間隔を内蔵フラッシュへ保存（LittleFSは追記のため一旦削除してから書く）
static void saveConfig() {
  if (InternalFS.exists(GWCFG_FILENAME)) InternalFS.remove(GWCFG_FILENAME);
  Adafruit_LittleFS_Namespace::File f(InternalFS);
  f = InternalFS.open(GWCFG_FILENAME, Adafruit_LittleFS_Namespace::FILE_O_WRITE);
  if (!f) { Serial.println(F("[CFG] 保存失敗（フラッシュ書込不可）")); return; }
  uint16_t magic = GWCFG_MAGIC;
  f.write((uint8_t*)&magic, sizeof(magic));
  f.write((uint8_t*)&sendIntervalMs, sizeof(sendIntervalMs));
  f.close();
}

// ステータス8バイトを組み立てる
// [0]FWバージョン [1]受信台数 [2]SD有無 [3]NW接続OK [4-5]送信間隔(分,LE) [6]最終CSQ [7]予約
static void buildStatus(uint8_t out[8]) {
  uint16_t mins = (uint16_t)(sendIntervalMs / 60000UL);
  out[0] = GATEWAY_FW_VERSION;
  out[1] = (uint8_t)recordCount;
  out[2] = sdAvailable ? 1 : 0;
  out[3] = s_lastNetOk ? 1 : 0;
  out[4] = mins & 0xFF;
  out[5] = (mins >> 8) & 0xFF;
  out[6] = (uint8_t)s_lastCsq;
  out[7] = 0;
}

static void updateStatusChar() {
  uint8_t st[8];
  buildStatus(st);
  gwCharStatus.write(st, sizeof(st));
  if (Bluefruit.connected()) gwCharStatus.notify(st, sizeof(st));
}

// ── BLE書き込みコールバック（最小限：保留フラグを立てるだけ）──
static void cbWriteInterval(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  (void)conn_hdl; (void)chr;
  if (len < 2) return;
  uint16_t minutes = (uint16_t)(data[0] | (data[1] << 8));
  if (minutes == 0) return;  // 0は無視
  s_pendingIntervalMs     = (uint32_t)minutes * 60000UL;
  s_pendingIntervalChange = true;
}

static void cbWriteCommand(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  (void)conn_hdl; (void)chr;
  if (len < 1) return;
  s_pendingCommand = data[0];
}

// コントローラー連携用BLEペリフェラルを初期化して広告開始する
static void bleControllerBegin() {
  Bluefruit.begin(1, 0);  // peripheral=1, central=0（LoRaビルドはBLEスキャンしない）
  Bluefruit.setName("MonitaGateway");
  Bluefruit.setTxPower(4);

  gwCfgService.begin();

  gwCharInterval.setProperties(CHR_PROPS_WRITE);
  gwCharInterval.setPermission(SECMODE_OPEN, SECMODE_OPEN);  // ★動作確認優先で無認証
  gwCharInterval.setFixedLen(2);
  gwCharInterval.setWriteCallback(cbWriteInterval);
  gwCharInterval.begin();

  gwCharCommand.setProperties(CHR_PROPS_WRITE);
  gwCharCommand.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  gwCharCommand.setFixedLen(1);
  gwCharCommand.setWriteCallback(cbWriteCommand);
  gwCharCommand.begin();

  gwCharStatus.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
  gwCharStatus.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  gwCharStatus.setFixedLen(8);
  gwCharStatus.begin();
  updateStatusChar();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addService(gwCfgService);
  Bluefruit.Advertising.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);  // 20ms / 152.5ms
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);  // 0=タイムアウトなし（常時コネクタブル広告）
  Serial.println(F("✓ コントローラー連携BLE（GATTサーバ）広告開始"));
}

// loop()先頭で呼ぶ。BLEコールバックが立てた保留要求を安全なタイミングで実行する。
static void handlePendingBleCommands() {
  if (s_pendingIntervalChange) {
    s_pendingIntervalChange = false;
    sendIntervalMs = s_pendingIntervalMs;
    saveConfig();
    Serial.print(F("[BLE] 送信間隔を変更: ")); Serial.print(sendIntervalMs / 60000UL); Serial.println(F("分"));
    updateStatusChar();
  }

  uint8_t cmd = s_pendingCommand;
  if (cmd != 0) {
    s_pendingCommand = 0;
    switch (cmd) {
      case 0x01:  // 即時送信
        Serial.println(F("[BLE] コマンド: 即時送信"));
        flushRecords();
        lastSend = millis();
        break;
      case 0x02:  // ネットワーク再登録
        Serial.println(F("[BLE] コマンド: ネットワーク再登録"));
        s_lastNetOk = initNetwork();
        break;
      case 0x03:  // 起動確認送信
        Serial.println(F("[BLE] コマンド: 起動確認送信"));
        postBootInfoRow();
        break;
      case 0xFF:  // フルリセット（NVIC_SystemReset → setup() から再初期化）
        Serial.println(F("[BLE] コマンド: フルリセット"));
        delay(100);  // ログ送出とBLE応答の猶予
        NVIC_SystemReset();
        break;
      default:
        Serial.print(F("[BLE] 未知コマンド: 0x")); Serial.println(cmd, HEX);
        break;
    }
    updateStatusChar();
  }
}
#endif  // COMM_MODE_LORA

// ══════════════════════════════════════════════
// setup
// ══════════════════════════════════════════════
void setup() {
  wdtInit(WDT_TIMEOUT_MS);  // 無人運用の安全網。以降 120 秒キックが無ければ自動リセット
  lastGasSuccessMs = millis();  // アプリ層WDTの起点。以降 APP_WDT_NO_SEND_RESET_MS 内に送信成功が無ければ再起動

  Wire.begin();
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);  // SDカード非選択で起動
#ifdef COMM_MODE_LORA
  pinMode(LORA_M0M1_PIN, OUTPUT);
  digitalWrite(LORA_M0M1_PIN, LOW);  // Normalモードで起動
#endif

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
      Serial.println(F("△ RTC が電源喪失 → ネットワーク接続後に網時刻(AT+CCLK)で自動設定します"));
      s_rtcNeedsTimeSet = true;
    }
    Serial.println(F("✓ DS3231 初期化完了"));
    Serial.print(F("  現在時刻: ")); Serial.println(getTimestamp());
  } else {
    Serial.println(F("✗ DS3231 が見つかりません（タイムスタンプは millis 基準）"));
  }

  // SD カード初期化（CS直結）
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

  recordMutex = xSemaphoreCreateMutex();

#ifdef COMM_MODE_BLE
  // BLE 初期化
  Bluefruit.begin(0, 1);  // 0 peripheral, 1 central
  Bluefruit.setName("MonitaGateway");
  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.useActiveScan(false);
  Bluefruit.Scanner.setInterval(
    (uint16_t)(SCAN_INTERVAL_MS * 1000 / 625),
    (uint16_t)(SCAN_WINDOW_MS   * 1000 / 625)
  );
  Serial.println(F("✓ BLE 初期化完了"));
#endif

#ifdef COMM_MODE_LORA
  // 保存済み送信間隔を内蔵フラッシュから復元（コントローラーで変更した値の永続化）
  loadConfig();

  // コントローラー連携BLE（GATTサーバ）初期化・広告開始
  bleControllerBegin();

  // LoRa（E220）初期化
  loraSerial.begin(9600);  // E220-900T22S(JP) デフォルト（要データシート確認）
  delay(500);              // E220 起動待ち（暫定値、要実測）
  loraModeNormal();
  if (!loraCheckAndConfigure()) {
    Serial.println(F("✗ LoRa設定確認に失敗（配線・電源を確認してください）"));
  }
  Serial.println(F("✓ LoRa 初期化完了"));
#endif

#if !LTEM_SEND_ENABLED
  // ★2026-07-23: LTE-M送信無効（SD記録のみモード）。SIM7080Gの初期化・ネットワーク接続は
  // 一切行わず、BLE（またはLoRa）受信データを updateRecordFromPayload() 内で直接SDへ記録する。
  Serial.println(F("\n△ LTEM_SEND_ENABLED=false: SIM7080G初期化・LTE-M送信をスキップ（SD記録のみモード）"));
#ifdef COMM_MODE_BLE
  Bluefruit.Scanner.start(0);
  Serial.println(F("✓ BLE スキャン開始（SD記録のみモード）"));
#endif
#else
  // ── SIM7080G 初期化シーケンス ──────────────────
  // SIM7080GはXIAOの5Vに常時直結（AC電源、v1.0と同じ方式）のため、
  // ソフトウェアでの電源投入シーケンスは不要。
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

#ifdef COMM_MODE_BLE
  // SIM7080G 起動待ちが終わった時点で BLE スキャンを開始する。
  // UART（Serial1, SIM7080G通信）と BLE 無線はハードウェア的に独立しているため、
  // STAGE3〜6（AT疎通確認〜ネットワーク接続）の間もスキャンを継続してよい。
  // これにより起動確認送信までに子機データがより多く貯まる。
  Bluefruit.Scanner.start(0);
  Serial.println(F("✓ BLE スキャン開始"));
#endif

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
  s_lastNetOk = netOk;  // コントローラー向けステータス用にキャッシュ
  simStage("STAGE6: ネットワーク接続", netOk);
  if (!netOk) {
    Serial.println(F("  → APN 設定・SIM 契約・電波状況を確認してください"));
    Serial.println(F("  → BLE スキャンは継続します（LTE-M なしで SD 保存のみ）"));
  } else if (rtcAvailable && s_rtcNeedsTimeSet) {
    if (syncRtcFromNetworkTime()) s_rtcNeedsTimeSet = false;
    else Serial.println(F("△ 網時刻の取得に失敗（次回起動時に再試行）"));
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
//
// AC電源（XIAO Type-C給電）での常時稼働モデル（v1.0と同じ）。
// BLE常時スキャン（またはLoRa常時待ち受け）＋ SEND_INTERVAL_MS ごとに定期送信。
// バッテリー駆動用のディープスリープ間欠動作は不要になったため未実装
// （旧設計は gateway_v1.10_ARCHIVE_battery_TCA9534_design.md 参照）。
// ══════════════════════════════════════════════
static uint32_t lastHeartbeat = 0;

void loop() {
  wdtFeed();          // BLE スキャン/LoRa待ち受けのみで sendAT が呼ばれない期間もハング扱いされないよう給餌
#if LTEM_SEND_ENABLED
  appWatchdogCheck();  // 一定時間 GAS 送信成功が無ければ強制再起動（ソフトハング対策）
#endif
  uint32_t now = millis();

#ifdef COMM_MODE_LORA
  loraPoll();                 // 受信バッファを読み切り、フレームが完成していればレコードへ反映
  handlePendingBleCommands(); // コントローラーからのBLE設定/コマンド要求を安全なタイミングで実行
#endif

  // デバッグ心拍: 10秒ごとに次回送信までの残り時間を表示
  if (now - lastHeartbeat >= 10000) {
    lastHeartbeat = now;
    Serial.print(F("[HB] now=")); Serial.print(now);
    Serial.print(F(" lastSend=")); Serial.print(lastSend);
    Serial.print(F(" 残り=")); Serial.print((long)(sendIntervalMs - (now - lastSend)));
    Serial.print(F("ms 受信台数=")); Serial.println(recordCount);
#ifdef COMM_MODE_LORA
    updateStatusChar();  // コントローラーへ最新状態を反映（接続中はnotify）
#endif
  }

#if LTEM_SEND_ENABLED
  if (now - lastSend >= sendIntervalMs) {
    lastSend = now;
    Serial.println(F("\n=== 定期送信 ==="));
    Serial.print(F("時刻: ")); Serial.println(getTimestamp());
    Serial.print(F("受信済み Flex 台数: ")); Serial.println(recordCount);

#ifdef COMM_MODE_BLE
    // 送信中は BLE スキャンを停止（LTE-M 通信中の割り込み負荷を減らす）
    Bluefruit.Scanner.stop();
#endif
    flushRecords();
#ifdef COMM_MODE_BLE
    Bluefruit.Scanner.start(0);
#endif
  }

  // 手動 AT コマンドモード（シリアルから入力）
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) sendAT(line);
  }
  while (Serial1.available()) Serial.write(Serial1.read());
#endif  // LTEM_SEND_ENABLED
}
