#define DEBUG 
#include <Arduino.h>// board maneger 2.0.17
#include <lvgl.h> //8.4.0
#include <TFT_eSPI.h> //2.5.43
#include "src/ui/ui.h"
#include <NimBLEDevice.h> //1.4.2
#include <vector>//1.2.2
#include <nvs_flash.h>
#include <cstring> 
#include <algorithm>

#include "esp_heap_caps.h"
#include "esp_system.h"

const char* VERSION = "1.41"; //FWバージョン
const char* BUILD_DATETIME = __DATE__ " " __TIME__;
#define AVLC_MAGIC "AVLC" //ヘッダー名称
#define AVLC_FORMAT_VERSION 1 //AVLCフォーマットバージョン

#include "time.h"
struct tm userTime;
//bool rtc_sync = false ;

//SD
#include "FS.h"
#include <FFat.h>
#include <SPI.h>
#include "SdFat.h"

#include <atomic>
using std::atomic;

// 先頭の include 群に追加
#if defined(MBEDTLS_CHACHAPOLY_C)
  #include "mbedtls/chachapoly.h"
#endif
#include "mbedtls/gcm.h"

//バッテリーゲージ
#include <MAX17048.h>
MAX17048 pwr_mgmt;

//IO設定
#define switch_a 34 //スイッチA
#define switch_b 35 //スイッチB
#define Sensor_VP 36 //S_VP
#define Sensor_VN 39 //S_VN
#define sw_off 25   //電源
#define sw_led 4   //LED
#define BL_PIN 14 //LCDのバックライト

// ===== Log mode switch =====
enum LogMode : uint8_t {
  LOG_CSV       = 0,  // 既存のCSV (既定)
  LOG_BIN       = 1,  // バイナリ(平文)
  LOG_BIN_AEAD  = 2   // バイナリ(暗号+完全性/AEAD)
};
static LogMode g_log_mode = LOG_BIN_AEAD;   // ← 必要に応じて切替
int SN_Limit=30000;//１ファイルの最大行数

// ====== 共通バイナリ定義 ======
#pragma pack(push,1)
struct AvlcFileHeader {
  char     magic[4];       // "AVLC"
  uint8_t  ver;            // AVLCフォーマットバージョン
  uint8_t  flags;          // bit0:encrypted(1)/plain(0)
  uint8_t  aead_algo;      // 1=ChaCha20-Poly1305, 2=AES-GCM
  uint8_t  key_id;         // 将来ローテ用
  uint8_t  log_mode;       // 0=CSV, 1=BIN, 2=AEAD
  char     fw_version[8];  // "1.39" など
  uint8_t  dev_mac[6];     // 識別用（任意）
  uint32_t start_unix;     // 開始UNIX秒
  uint64_t file_id;        // 乱数(ノンス用)
  uint32_t rec_counter0;   // 初期レコード番号（0推奨）
  uint32_t header_crc32;   // 上記のCRC32（magic〜rec_counter0）
};
static constexpr size_t AVLC_HEADER_BYTES = sizeof(AvlcFileHeader);

struct AvlcRecordHeader {
  uint8_t  type;         // 1=通常行, 2=メタ, 3=イニシャル(GN/VV/MX22)
  uint32_t ts_ms;        // 相対ms or 絶対ms
  uint16_t len;          // 平文payload長
  uint32_t idx;          // レコード連番
};

#pragma pack(pop)
// ====== AEADキー（NVSから取得するならここで読込） ======
static uint8_t  g_aead_key[32] = {0};   // 32B鍵（仮：ゼロ。量産でNVSから）
static uint8_t  g_aead_algo     = 2;    // 1=ChaCha20-Poly1305 (推奨), 2=AES-GCM
static uint8_t  g_key_id        = 0;

// 既存のキー定義の下あたり
static bool load_aead_key_from_nvs() {
  nvs_handle h;
  if (nvs_open("storage", NVS_READONLY, &h) != ESP_OK) {
    Serial.println("[AEAD] NVS open failed -> using TEST key");
    memset(g_aead_key, 0x11, sizeof(g_aead_key));  // テスト用
    g_key_id = 1;
    return false;
  }
  size_t sz = sizeof(g_aead_key);
  esp_err_t err = nvs_get_blob(h, "aead_key", g_aead_key, &sz);
  if (err != ESP_OK || sz != sizeof(g_aead_key)) {
    Serial.println("[AEAD] no key in NVS -> using TEST key");
    memset(g_aead_key, 0x11, sizeof(g_aead_key));  // テスト用
    g_key_id = 1;
    nvs_close(h);
    return false;
  }
  // 任意: key_id も保存してあれば読む
  uint8_t kid = 0;
  sz = sizeof(kid);
  if (nvs_get_blob(h, "aead_key_id", &kid, &sz) == ESP_OK && sz == 1) g_key_id = kid;
  nvs_close(h);
  return true;
}

SdFat32 SD;
File32 SD_File;
//int file_count =0;
char filename[16];//ファイル名
const int QUEUE_THRESHOLD = 10;    // Queue(Buffer)保存するデータ数の閾値
const int TIME_THRESHOLD = 10000;  // SDに保存する時間間隔 (ミリ秒)
unsigned long lastSaveTime = 0;   // 最後に保存した時間

#define SD_CS_PIN 5    // SDカードのチップセレクトピン
#define SD_MISO 19   // SPIのMISOピン
#define SD_MOSI 23   // SPIのMOSIピン
#define SD_SCK  18   // SPIのクロックピン
//#define SD_CONFIG SdSpiConfig(SD_CS_PIN, DEDICATED_SPI, SD_SCK_MHZ(24))
#define SD_CONFIG SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(12))

// ====== 追加：FFat(内蔵Flash) 用のログファイル管理 ======
static fs::File ffatLog;                 // 現行FFatログ（CSVで追記）
static const char* SD_LOG_DIR = "/AVLC";  // サブディレクトリ名（先頭スラッシュ必須）
static const char* FLASH_BOOT_CSV = "/log_boot.csv";
static const char* FLASH_BOOT_BIN = "/log_boot.bin";
static char flash_cur_path[48] = "/log_boot.csv";
// 追加: 今のモードの拡張子とBootパスを返すヘルパ
static inline const char* log_ext_for_mode() {
  return (g_log_mode == LOG_CSV) ? ".csv" : ".bin";
}
static inline const char* boot_path_for_mode() {
  return (g_log_mode == LOG_CSV) ? FLASH_BOOT_CSV : FLASH_BOOT_BIN;
}
 
//FFAT
static bool flash_ok = false ; 
static const char* FLASH_LOG_PREFIX = "/log_";

char g_dump_name[20] = {0};

//-----------
#include <XPT2046_Touchscreen.h>
// Touchscreen pins
#define XPT2046_IRQ 13   // T_IRQ
#define XPT2046_MOSI 23  // T_DIN
#define XPT2046_MISO 19  // T_OUT
#define XPT2046_CLK 18  // T_CLK
#define XPT2046_CS 33    // T_CS

//SPIClass touchscreenSPI = SPIClass(HSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
//-----------


// Nordic UART Service UUIDs
static BLEUUID serviceUUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
static BLEUUID charUUID_RX("6e400002-b5a3-f393-e0a9-e50e24dcca9e");
static BLEUUID charUUID_TX("6e400003-b5a3-f393-e0a9-e50e24dcca9e");

// === NVS utils forward declarations (先に宣言しておく) ===
static bool nvs_write_last_addr(const char* addr);
static bool nvs_read_last_addr(String &out);
static bool nvs_erase_last_addr();
// ===== Atomic shared flags & counters (with function comments) =====

// --- 時刻同期・初期化シーケンス ---
atomic<bool> rtc_sync{false};        // RTCが同期済みか      (true=時刻設定完了, false=未設定)
atomic<bool> id_get{false};          // デバイスID取得完了    (true=GN応答取得済み)
atomic<bool> ver_get{false};         // ファームVer取得完了   (true=VV応答取得済み)
atomic<bool> mx22_done{false};       // 初期化コマンド完了    (true=MX22応答完了)
atomic<bool> needHeader{false};      // 新ログ開始フラグ      (true=GN/VV再送必要, false=完了)
atomic<bool> rollPending{false};     // ファイルローテ準備     (true=次コマンド待ちで新ファイル準備)
atomic<bool> cmd_just_finished{false}; // 直近でCommand受信完了 (true=Command:受信直後)
atomic<bool> ui_auto_to_menu{false}; // 初期化完了後自動遷移   (true=自動でメニュー画面へ戻す)
//atomic<bool> nav_to_menu_after_cmd{false}; // ui_vars.cで宣言、コマンド完了後にメニュー戻す (true=戻す) 
atomic<bool> g_time_named{false};    // Flashログ名が時刻ベース (true=/log_YYMMDDhhmmss.csv化済み)

// --- BLE接続／通信制御 ---
atomic<bool> doConnect{false};       // BLE接続要求フラグ     (true=接続開始を指示)
atomic<bool> connected{false};       // GATT接続状態          (true=接続中, false=未接続)
atomic<bool> doScan{true};           // スキャン要求フラグ    (true=スキャン中/要求, false=停止)
atomic<bool> GATT_command_rep{false};     // BLE応答検出フラグ    (true=期待応答を受信済み)
//atomic<bool> GATT_command_receive{false}; // ui_vars.cで宣言、BLE通信ビジー状態    (true=コマンド処理中, false=待機可)
atomic<bool> re_try{false};               // 再送中フラグ         (true=再送リトライ中)
atomic<bool> unint_disconnect{false};     // 予期せぬ切断検出     (true=意図しない切断発生)

// --- 計測／UI制御 ---
//atomic<bool> is_measuring{false};    // ui_vars.cで宣言、計測中フラグ          (true=連続測定中)
atomic<bool> isWriting{false};       // チャート更新要求      (true=新データあり再描画必要)
//atomic<bool> measurment_mode{true};  // ui_vars.cで宣言、計測モード選択        (true=Fast(MSC), false=Slow(MX2ES))

// --- ストレージ／ログ管理 ---
atomic<bool> g_dump_request{false};       // ダンプ要求フラグ      (true=Flash→SD転送要求)
atomic<bool> g_dump_in_progress{false};   // ダンプ実行中          (true=転送処理中)
atomic<bool> g_post_cleanup_pending{false}; // ダンプ後後片付け待ち (true=リセット処理待機)
static uint64_t s_file_id = 0;            // ファイル先頭以外からも参照したいのでファイルスコープ static を使う
static std::atomic<bool> log_ready{false};            // 初回ログ準備が済んだら true
static std::atomic<bool> first_file_prepared{false};  // 初回ファイル生成を一度だけ
static std::atomic<bool> g_sd_absent{false}; //SDカードが入っているか、いないか。
static std::atomic<bool> g_sd_status_known{false};  // ★追加：SD判定済みか
static uint32_t g_sd_last_try_ms = 0; //前回リトライした時刻
static const uint32_t SD_RETRY_MS = 30000; // 30秒毎くらいに再挑戦
static uint32_t g_last_rotate_fail_ms = 0;
static uint32_t g_last_cooldown_log_ms = 0;   // ← 使うなら（クールダウンのログ間引き用）
static const uint32_t ROTATE_RETRY_MS = 5000; // 5秒おきに再挑戦
// アイドルDump用の待機時間/クールダウン
static const uint32_t IDLE_DUMP_AFTER_MS     = 3000;  // 最終書込から3秒静かならOK
static const uint32_t IDLE_DUMP_COOLDOWN_MS  = 10000; // 連発防止のクールダウン
static uint32_t g_last_idle_dump_ms = 0;

//POWOFF　管理
atomic<bool> g_poweroff_request{false};
atomic<bool> g_poweroff_in_progress{false};

static void (*const kInit[7])(void) = {
  ui_Screen1_screen_init,
  ui_Screen2_screen_init,
  ui_Screen3_screen_init,
  ui_Screen4_screen_init,
  ui_Screen5_screen_init,
  ui_Screen6_screen_init,
  ui_Screen7_screen_init
};

static inline bool initial_ready() {
  return rtc_sync.load() && id_get.load() && ver_get.load() && mx22_done.load();
}

static void ensure_sd_status_known_now(bool force_probe = false) {
  uint32_t now = millis();

  // ★ クールダウン（叩き過ぎ防止）
  if (!force_probe && (now - g_sd_last_try_ms) < SD_RETRY_MS) return;
  if ((now - g_sd_last_try_ms) < 1000) return; // 1秒未満の連打も抑制
  g_sd_last_try_ms = now;

  if (SD.begin(SD_CONFIG)) {
    g_sd_absent.store(false);
  } else {
    g_sd_absent.store(true);
  }
  g_sd_status_known.store(true); // 「判定はした」印。以後もクールダウン後に再判定可。
}

// PATCH B: State machine
enum class SystemState : uint8_t {
  NORMAL,
  SOFT_MARGIN,        // 3MB未満（SDあり時のみ defer_dump 有効）
  EMERGENCY,          // 1MB未満（即停止→Dump）
  WAIT_SD,            // EMERGENCY中にSD不在/失敗
  DUMPING,            // Dump中
  POST_DUMP_ROTATE    // Dump成功後のローテ処理
};
static std::atomic<SystemState> g_state{SystemState::NORMAL};

static inline bool can_write_now() {
  auto s = g_state.load();
  return (s == SystemState::NORMAL || s == SystemState::SOFT_MARGIN);
}

// ====== Free space guard (mode-aware) ======
struct RecSizeEstimator {
  // 単純移動平均でもOKやけど、コスト低く反応良いEWMAを採用
  // alpha大：新しい値を重視。0.05〜0.2くらいが扱いやすい。
  float ewma = 0.0f;
  uint32_t n = 0;
  void update(size_t bytes) {
    const float alpha = 0.12f;   // 調整可
    if (n == 0) { ewma = (float)bytes; n = 1; return; }
    ewma = (1.0f - alpha) * ewma + alpha * (float)bytes;
    if (n < 0xFFFFFFFF) n++;
  }
  size_t avg() const { return (size_t) (ewma > 1.0f ? ewma : 0); }
  bool ready() const { return n >= 8; } // 最初の数サンプルは固定値で保守
};

// モード別の推定器
static RecSizeEstimator g_est_csv;
static RecSizeEstimator g_est_bin;
static RecSizeEstimator g_est_aead;

// 初期の保守固定（最低ライン）— あとで実測が上書き
static const size_t CSV_REC_BYTES_BASE  =  60;      // 目安：行の平均（タグ込み）
static const size_t BIN_REC_BYTES_BASE  =  32 + 24; // AvlcRecordHeader(12) + 平文payload目安 + 余裕
static const size_t AEAD_REC_BYTES_BASE =  32 + 16 + 24; // payload目安 + Tag16 + 余裕

// PATCH A: thresholds (512KB hard-stop 廃止)
static const size_t MIN_MARGIN_BYTES_SOFT = 3 * 1024 * 1024;  // 3MB: 次ローテでDump
static const size_t EMERGENCY_BYTES       = 256 * 1024;  // 1MB: 即停止→Dump→ローテ
// static const size_t HARD_STOP_MIN = 512 * 1024; // 廃止

// ユーザー指定：何本分を安全確保するか（例：2本）
static std::atomic<uint32_t> g_margin_files{1};
//static std::atomic<uint32_t> g_margin_files{1};

// しきい値を越えたが、“途中切れ防止”のため Dump は次ローテで実施する予約フラグ
static std::atomic<bool> g_defer_dump_until_rotate{false};

// 任意：ログに現在の推定を出す/抑制
static bool g_est_debug = true;

static inline RecSizeEstimator& cur_estimator() {
  switch (g_log_mode) {
    case LOG_CSV:      return g_est_csv;
    case LOG_BIN:      return g_est_bin;
    case LOG_BIN_AEAD: return g_est_aead;
    default:           return g_est_csv;
  }
}

static inline size_t base_rec_bytes_for_mode() {
  switch (g_log_mode) {
    case LOG_CSV:      return CSV_REC_BYTES_BASE;
    case LOG_BIN:      return BIN_REC_BYTES_BASE;
    case LOG_BIN_AEAD: return AEAD_REC_BYTES_BASE;
    default:           return CSV_REC_BYTES_BASE;
  }
}

// 1ファイルの“実サイズ”見積り（平均×行数、足切りあり）
static inline size_t estimate_one_file_bytes() {
  auto &est = cur_estimator();
  size_t avg = est.ready() ? est.avg() : base_rec_bytes_for_mode();
  // 1行が極端に短く見積もられたときのための保険
  if (avg < 16) avg = 16;
  size_t est_file = (size_t)avg * (size_t)SN_Limit;
  // ファイルヘッダなどの固定オーバーヘッドをざっくり上乗せ
  if (g_log_mode != LOG_CSV) est_file += 128;
  return est_file;
}

// 現在のモードに応じた margin_bytes を返す
static inline size_t calc_margin_bytes() {
  uint32_t mfiles = g_margin_files.load();
  if (mfiles == 0) mfiles = 1; // 最低でも1本分
  size_t est_file = estimate_one_file_bytes();
  size_t margin   = est_file * (size_t)mfiles;
  if (margin < MIN_MARGIN_BYTES_SOFT) margin = MIN_MARGIN_BYTES_SOFT; // “最低ライン”
  return margin;
}

// PATCH C: 状態機械へ統合済。呼び出し側から参照削除するためダミーに。
static inline bool hard_stop_active() {
  return false;
}

// PATCH B2: 空き容量→状態の評価（save系関数の先頭で呼ぶ）
static void evaluate_space_and_state_for_write() {
  const size_t freeb = (size_t)FFat.totalBytes() - (size_t)FFat.usedBytes();

  if (freeb < EMERGENCY_BYTES) {
      g_state.store(SystemState::EMERGENCY);
      Serial.printf("[Guard] EMERGENCY triggered! free=%u margin=%u (state=%d)\n",
                    (unsigned)freeb,
                    (unsigned)calc_margin_bytes(),
                    (int)g_state.load());
      return;
  }

  size_t margin = calc_margin_bytes();
  if (freeb < margin) {
    // 状態は SD の有無に関係なく SOFT_MARGIN として扱う
    g_state.store(SystemState::SOFT_MARGIN);

    // SD があるときだけ「Dump予約」と「ローテ要求」を入れる
    ensure_sd_status_known_now(true);
    if (!g_sd_absent.load()) {
      g_defer_dump_until_rotate.store(true);
      if (!rollPending.load()) rollPending.store(true);
    }

    return;
  }

  g_state.store(SystemState::NORMAL);
}

#ifdef DEBUG
// ===== DEBUG: FFatダミー埋め／削除 =====
static void fill_ffat_with_dummy(size_t targetFillBytes) {
  Serial.printf("[DBG] fill_ffat_with_dummy: %u bytes\n", (unsigned)targetFillBytes);
  if (!flash_ok) { Serial.println("[DBG] FFat not ready"); return; }

  const char* name = "/dbg_fill.bin";
  fs::File f = FFat.open(name, FILE_WRITE);   // truncate
  if (!f) { Serial.println("[DBG] open failed"); return; }

  const size_t CHUNK = 4096;
  uint8_t *buf = (uint8_t*)malloc(CHUNK);
  if (!buf) { Serial.println("[DBG] malloc failed"); f.close(); return; }
  memset(buf, 0xA5, CHUNK);

  // 安全側：現在のfreeを超えて書き過ぎない
  size_t total = FFat.totalBytes(), used = FFat.usedBytes();
  size_t freeb = (total > used) ? (total - used) : 0;
  size_t safe_max = freeb > 1024 ? (freeb - 1024) : freeb;
  if (targetFillBytes > safe_max) {
    Serial.printf("[DBG] request %u > safe_max %u, clipping\n",(unsigned)targetFillBytes,(unsigned)safe_max);
    targetFillBytes = safe_max;
  }

  size_t written = 0;
  while (written < targetFillBytes) {
    size_t w = CHUNK;
    if (targetFillBytes - written < CHUNK) w = targetFillBytes - written;
    int r = f.write(buf, w);
    if (r != (int)w) { Serial.printf("[DBG] write failed at %u (wrote %d)\n",(unsigned)written,r); break; }
    written += w;
    vTaskDelay(pdMS_TO_TICKS(1)); // WDT/他タスク配慮
  }
  f.flush(); f.close(); free(buf);
  Serial.printf("[DBG] done. filled=%u bytes -> /dbg_fill.bin\n", (unsigned)written);
}

static void remove_ffat_dummy() {
  const char* name = "/dbg_fill.bin";
  if (!flash_ok) { Serial.println("[DBG] FFat not ready"); return; }
  if (FFat.exists(name)) {
    if (FFat.remove(name)) Serial.println("[DBG] removed /dbg_fill.bin");
    else                   Serial.println("[DBG] remove failed");
  } else {
    Serial.println("[DBG] /dbg_fill.bin not exists");
  }
}

static void make_ffat_log_dummy(size_t targetBytes) {
  Serial.printf("[DBG] make_ffat_log_dummy: %u bytes\n", (unsigned)targetBytes);
  if (!flash_ok) { Serial.println("[DBG] FFat not ready"); return; }

  // 安全側：現在のfreeを超えて書き過ぎない（1KBだけ余裕）
  size_t total = FFat.totalBytes();
  size_t used  = FFat.usedBytes();
  size_t freeb = (total > used) ? (total - used) : 0;
  if (freeb <= 1024) {
    Serial.println("[DBG] no free space for dummy log");
    return;
  }
  size_t safe_max = freeb - 1024;
  if (targetBytes > safe_max) {
    Serial.printf("[DBG] request %u > safe_max %u, clipping\n",
                  (unsigned)targetBytes, (unsigned)safe_max);
    targetBytes = safe_max;
  }

  // ファイル名を /log_dummy000.bin, /log_dummy001.bin... のように決める
  char name[32];
  int  idx = 0;
  for (; idx < 1000; ++idx) {
    snprintf(name, sizeof(name), "/log_dummy%03d.bin", idx);
    if (!FFat.exists(name)) break;
  }
  if (idx == 1000) {
    Serial.println("[DBG] too many dummy log files");
    return;
  }

  fs::File f = FFat.open(name, FILE_WRITE);
  if (!f) {
    Serial.printf("[DBG] open failed: %s\n", name);
    return;
  }

  // とりあえずヘッダ無しのゼロ埋めでも DumpAll 的には問題ない。
  // （Avlcパーサで読むことは想定していないテスト用）
  const size_t CHUNK = 4096;
  uint8_t *buf = (uint8_t*)malloc(CHUNK);
  if (!buf) {
    Serial.println("[DBG] malloc failed");
    f.close();
    return;
  }
  memset(buf, 0xAA, CHUNK);  // 適当なパターン

  size_t written = 0;
  while (written < targetBytes) {
    size_t w = CHUNK;
    if (targetBytes - written < CHUNK) w = targetBytes - written;
    int r = f.write(buf, w);
    if (r != (int)w) {
      Serial.printf("[DBG] write failed at %u (wrote %d)\n",
                    (unsigned)written, r);
      break;
    }
    written += w;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  f.flush();
  f.close();
  free(buf);

  Serial.printf("[DBG] dummy log created: %s (size≈%u)\n",
                name, (unsigned)written);
}

// dummylog(xxxx) dummy(XXXX) / dummy(0) を解釈して実行
static bool processDebugSerialCommand(const String& raw) {
  String s = raw; s.trim();

  // 1) dummylog(N) → Dump対象のダミーログを作る
  int p_log = s.indexOf("dummylog(");
  if (p_log >= 0) {
    int closep = s.indexOf(')', p_log + 9);
    if (closep < 0) {
      Serial.println("[DBG] dummylog(): missing ')'");
      return true;  // コマンドとしては消費した扱い
    }
    String num = s.substring(p_log + 9, closep);
    num.trim();
    long n = num.toInt();  // バイト数
    if (n <= 0) {
      Serial.println("[DBG] dummylog(): size must be >0");
    } else {
      make_ffat_log_dummy((size_t)n);
    }
    return true;
  }

  // 2) dummy(N) → /dbg_fill.bin による単純な埋め＆削除
  int p = s.indexOf("dummy(");
  if (p < 0) return false;

  int closep = s.indexOf(')', p + 6);
  if (closep < 0) {
    Serial.println("[DBG] dummy(): missing ')'");
    return true;
  }

  String num = s.substring(p + 6, closep);
  num.trim();
  long n = num.toInt();  // バイト数
  if (n <= 0) {
    // 0以下は削除扱い
    remove_ffat_dummy();
  } else {
    fill_ffat_with_dummy((size_t)n);
  }
  return true;
}
#endif // DEBUG

// --- BLE受信再構成ステート ---
atomic<bool> isCombining{false};          // データ分割受信中      (true=XY/XXパケット連結中)
atomic<unsigned long> lastPacketTime{0};  // 最後のBLE受信時刻(ms)

// --- ログ関連カウンタ ---
atomic<int> file_count{0};                // 現行ログ行数           (SN_Limit到達でローテ)
static std::atomic<uint32_t> g_rec_idx{0};

// デバック用　グローバルに一行追加（テスト後は削除OK）
static std::atomic<int> g_disconnect_calls{0};

NimBLEClient* pClient;
NimBLEScan* pScan;
NimBLEAdvertisedDevice* myDevice;
NimBLERemoteCharacteristic* pRxCharacteristic = nullptr;
NimBLEAddress lastConnectedAddress;

std::vector<NimBLEAdvertisedDevice*> foundDevices;
std::vector<uint8_t> dataBuffer;
std::vector<uint8_t> combinedData;
std::string expectedReply;

static const unsigned long timeoutInterval = 12000; // unit milli seconds
static const int scanInterval = 3; // unit milli seconds
//-----------

/*Don't forget to set Sketchbook location in File/Preferences to the path of your UI project (the parent foder of this INO file)*/

/*Change to your screen resolution*/
static const uint16_t screenWidth  = 320;
static const uint16_t screenHeight = 240;

static lv_disp_draw_buf_t draw_buf;
//static lv_color_t buf[screenWidth * 10]; // Buffer size adjusted
static lv_color_t buf1[screenWidth * 10];
static lv_color_t buf2[screenWidth * 10];

TFT_eSPI tft = TFT_eSPI(screenWidth, screenHeight); /* TFT instance */

// スクリーンLED
lv_obj_t* led_indicator;

// ★ ステータスバー関係
static lv_obj_t* status_bar = nullptr;
static lv_obj_t* icon_batt  = nullptr;
static lv_obj_t* icon_ffat  = nullptr;
static lv_obj_t* icon_sd    = nullptr;
static lv_obj_t* batt_bar[4];
static lv_obj_t* batt_charge_icon = nullptr;

enum IconState : uint8_t {
    ICON_UNKNOWN = 0,
    ICON_OK,
    ICON_WARN,
    ICON_NG
};

// チャート
lv_chart_series_t* ui_Chart1_series_1;
// チャートの配列サイズ
const int CHART_POINTS = 10;
float ui_Chart1_series_1_array[CHART_POINTS];

//Queue　Logデータ
QueueHandle_t dataQueue;
#define QUEUE1_LENGTH 100
static const int ble_reply = 126;

//Queue　画面切り替え
typedef struct {
    uint8_t             index;   // 0-5
    lv_scr_load_anim_t  anim;    // アニメーション種別
} ScreenMsg;
QueueHandle_t screenQueue;

void requestScreenChange(uint8_t idx, lv_scr_load_anim_t anim)
{
    if (idx >= 7) {
        Serial.printf("[UI] requestScreenChange: bad idx=%u\n", idx);
        return;
    }
    ScreenMsg msg = {idx, anim};
    xQueueSend(screenQueue, &msg, 0);
}


// プロトタイプ宣言
void update_chart(float new_value); // チャート配列に新しい測定値を追加し、表示データを更新
void set_dropdown_options(const char* options); // ドロップダウンリストの選択肢を動的に設定
void setCheckboxChecked(lv_obj_t *checkbox, bool checked); // チェックボックスのON/OFF状態を設定
void dateTime(uint16_t* date, uint16_t* time); // FATファイル形式の日付・時刻値をRTCから生成
void processData(const std::string& data); // BLE受信データを解析し、ログ処理やUI更新を行う
void my_print(const char * buf); // LVGLログ出力をSerialへ転送
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p); // LVGL描画バッファをTFTに転送
void debug_print(const char* message); // 簡易デバッグ出力（Serial.println）
void redraw_chart(lv_obj_t* chart, lv_chart_series_t* series); // チャート描画範囲・ラベル・データを再描画
void handleConnection(); // BLEデバイスとの接続処理（UUID探索・Notify購読など）
void handleScanning(); // BLEスキャン実行とデバイスリスト生成
void handleSerialInput(String& input, bool& uart_send); // シリアル入力文字列を受け取り解析
void handleDeviceSelection(String& input); // シリアル入力番号から接続対象デバイスを選択
void handleUartCommunication(String& input); // BLE経由コマンド送信と応答リトライ処理
void checkPacketTimeout(unsigned long currentTime); // BLEパケット結合処理のタイムアウト監視
void eraseNVSKey(); // NVS上の保存アドレス情報を削除（最後の接続MAC消去）
void disconnect(); // BLE切断＋FFatログ保存要求＋後片付けフラグ設定
bool inputTimeFromConsole(const String& input); // コンソール入力から時刻文字列を解析し構造体へ格納
void setRTCFromUserTime(); // userTime構造体からESP32内部RTCへ時刻設定
bool printLocalTime(); // 現在のRTC時刻を取得し表示＋ファイル名生成
void ble_task(void *pvParameters); // BLE通信メインタスク（接続・送信・状態管理ループ）
void ui_task(void *pvParameters); // UI描画・ログ保存・画面切替を処理するタスク
static void ensure_touch_calibrated(); // タッチパネルのキャリブレーション確認と実行
static void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data); // タッチ入力座標を取得してLVGLに渡す
static void wait_for_release_and_debounce(uint16_t release_ms = 80, uint16_t debounce_ms = 200); // タッチリリースとデバウンス待機
static bool openFlashCsvForAppend(); // FFat上のCSV/BINログファイルを追記モードで開く
void saveToFlashCsv(); // FFatへCSV形式でキューデータを追記保存
static bool dumpFlashCsvToSD(const char* sd_csv_name); // FFatログをSDカードへコピー（旧形式）
static bool flash_rotate_to_new_file(); // FFatログファイルを新規ファイルへローテーション
static bool dumpAllFlashCsvToSD_AndPurge(); // すべてのFFatログをSDへ移動し削除
static void ffat_debug_list(); // FFat内ファイル一覧と容量をシリアル出力
static void ensure_log_path_ready_for_write(); // ログファイル名を時刻ベースに更新して書き込み準備
static inline void saveToFlash(); // 現在のモードに応じてCSV/BIN保存処理を呼び分け
void saveToFlashBin(); // FFatへAvlcバイナリ形式でデータを保存
static bool ensureBinFileHeader(); // 新規BINファイル作成時にヘッダを1回だけ書き込み
static bool aead_encrypt(const AvlcRecordHeader& rh, const uint8_t* pt, size_t pt_len, uint8_t* ct, size_t& ct_len); // AEAD暗号化処理（未実装）
static uint32_t crc32_le(uint32_t crc, const uint8_t *buf, size_t len); // Little Endian形式のCRC32計算
static bool writeAvlcHeaderInto(fs::File &f); // AvlcFileHeader構造体をファイルに書き込む
static bool readAvlcHeaderAndLatch(fs::File &f);// 既存BINのヘッダを読み取り、s_file_id を復旧する（成功なら true）
static void statusbar_create();
static void statusbar_update();
static bool is_charging();
static const char* log_mode_to_str(LogMode mode);
static void buildCsvHeader(char* buf, size_t bufsize);
static void perform_safe_poweroff();
static void flush_queue_to_flash_for_poweroff();
static void show_measuring_block_dialog();
static inline bool avl_busy();

// ------- Monitor helper (1 place to rule them all) -------
static inline const char* log_mode_str() {
  switch (g_log_mode) {
    case LOG_CSV:      return "CSV";
    case LOG_BIN:      return "BIN";
    case LOG_BIN_AEAD: return "AEAD";
    default:           return "?";
  }
}

static void set_icon_state(lv_obj_t* obj, IconState st)
{
    if (!obj) return;

    lv_color_t col;
    switch (st) {
        case ICON_OK:
            col = lv_color_make(0x00, 0xC8, 0x00); // 緑
            break;
        case ICON_WARN:
            col = lv_color_make(0xE0, 0xE0, 0x00); // 黄
            break;
        case ICON_NG:
            col = lv_color_make(0xE0, 0x00, 0x00); // 赤
            break;
        case ICON_UNKNOWN:
        default:
            col = lv_color_make(0x80, 0x80, 0x80); // グレー
            break;
    }

    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, col, 0);
}

// ダイアログのLVGLオブジェクト
static lv_obj_t* g_poweroff_mbox = nullptr;
// しきい値を超えた状態を保持（立ち上がりだけでダイアログを出す）
static bool g_sensor_vp_high = false;

// 「はい / いいえ / ✗」共通のイベントハンドラ
static void poweroff_mbox_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        // ★ ここが重要：current_target が msgbox、本体の方
        lv_obj_t * mbox = lv_event_get_current_target(e);

        const char * txt = lv_msgbox_get_active_btn_text(mbox);
        if (txt != nullptr) {
          if (strcmp(txt, "はい") == 0) {
              if (avl_busy()) {
                  Serial.println("[POW] poweroff blocked at confirm: measuring");
                  show_measuring_block_dialog();
              } else {
                  g_poweroff_request = true;
              }
          }
            // 「いいえ」でも閉じる
        }

        // msgbox を閉じる（これで LV_EVENT_DELETE も飛んでくる）
        lv_msgbox_close(mbox);
    }
    else if (code == LV_EVENT_DELETE) {
        // ✗ で閉じた場合や、close() 呼び出し後に必ず通る
        g_poweroff_mbox = nullptr;
        g_sensor_vp_high = false;   // 次のループでまたトリガ可能にする
    }
}

// ダイアログ生成関数
static void show_poweroff_dialog() {
    if (g_poweroff_mbox != nullptr) return;

    static const char * btns[] = { "はい", "いいえ", "" };

    g_poweroff_mbox = lv_msgbox_create(
        NULL,
        "電源OFF",
        "電源を切りますか？",
        btns,
        true  // ✗ボタン付き
    );

    lv_obj_center(g_poweroff_mbox);

    // VALUE_CHANGED（Yes/No）
    lv_obj_add_event_cb(g_poweroff_mbox, poweroff_mbox_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    // DELETE（✗や close() 後）
    lv_obj_add_event_cb(g_poweroff_mbox, poweroff_mbox_event_cb,
                        LV_EVENT_DELETE, NULL);
}


static void monitor_status() {

  // ★ Dump中・WAIT_SD中は状態をいじらない（ループ防止）
  SystemState st = g_state.load();
  if (!g_dump_in_progress.load() &&
      st != SystemState::DUMPING &&
      st != SystemState::WAIT_SD) {

    // 10秒ごとに FFat の状態を再評価（書き込み停止中でもOK）
    evaluate_space_and_state_for_write();

    // SDの有無も「様子見」だけする（force_probe=false）
    ensure_sd_status_known_now(false);
  }
  
  const uint32_t used  = (uint32_t)FFat.usedBytes();
  const uint32_t total = (uint32_t)FFat.totalBytes();
  const uint32_t freeb = total - used;
  const bool     open  = (bool)ffatLog;  // 現在 open(APPEND) されているか
  size_t est_file   = estimate_one_file_bytes();
  size_t margin_b   = calc_margin_bytes();

  // 現在のログパス（長すぎると崩れるので48Bに収めている想定）
  const char* path = flash_cur_path;

  // 直近保存からの経過ms（見やすさ用）
  const uint32_t since_save_ms = millis() - lastSaveTime;

  Serial.printf(
    "[Monitor] FFat %u/%u (free=%u) | "
    "heap=%u minHeap=%u largest=%u | "
    "reset=%d | "
    "can_write_now=%s | mode=%s | path=%s | open=%s | tnamed=%s | rtc=%s | "
    "roll=%s needHdr=%s fileCnt=%d recIdx=%u | sinceSave=%ums\n"
    " | estFile=%u margin=%u mfiles=%u | "
    "Batt=%.2fV %u%%\n",

    used, total, freeb,

    (unsigned)ESP.getFreeHeap(),
    (unsigned)ESP.getMinFreeHeap(),                      // ★ 追加
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), // ★ 追加

    (int)esp_reset_reason(),                             // ★ 追加

    can_write_now() ? "ON" : "OFF",
    log_mode_str(),
    path,
    open ? "YES" : "NO",
    g_time_named ? "T" : "F",
    rtc_sync ? "T" : "F",
    rollPending ? "T" : "F",
    needHeader ? "T" : "F",
    (int)file_count.load(),
    (unsigned)g_rec_idx.load(),
    (unsigned)since_save_ms,
    (unsigned)est_file,
    (unsigned)margin_b,
    (unsigned)g_margin_files.load(),
    pwr_mgmt.voltage(),
    pwr_mgmt.percent()
  );

}

void set_dropdown_options(const char* opts)
{
    /* ----- opts を UI スレッド側で使えるように複製 ----- */
    char* copy = (char*)lv_mem_alloc(strlen(opts) + 1);
    strcpy(copy, opts);

    lv_async_call(
        [](void* p){
            lv_dropdown_set_options(ui_Dropdown1, (const char*)p);
            lv_mem_free(p);                 // ← lv_mem_free を使う
        },
        copy);
}


void setCheckboxChecked(lv_obj_t *checkbox, bool checked)
{
    struct Param { lv_obj_t *obj; bool flag; };
    Param *p = (Param*)lv_mem_alloc(sizeof(Param));
    if (!p) {
        Serial.println("[UI] lv_mem_alloc failed");
        return;
    }

    p->obj  = checkbox;
    p->flag = checked;

    lv_async_call(
        [](void *ud){
            Param *pp = (Param*)ud;
            if (!pp) return;

            if (pp->obj == nullptr) {
                Serial.println("[UI] checkbox is null");
                lv_mem_free(pp);
                return;
            }

            if (!lv_obj_is_valid(pp->obj)) {
                Serial.printf("[UI] invalid checkbox ptr=%p\n", pp->obj);
                lv_mem_free(pp);
                return;
            }

            if (pp->flag) {
                lv_obj_add_state(pp->obj, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(pp->obj, LV_STATE_CHECKED);
            }

            lv_mem_free(pp);
        },
        p);
}


// タイムスタンプ用のコールバック関数
void dateTime(uint16_t* date, uint16_t* time) {
    struct tm timeinfo;

    // ESP32のRTCから現在の日時を取得
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
        return;
    }

    // FATファイルシステム形式に変換
    *date = FAT_DATE(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    *time = FAT_TIME(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

void processData(const std::string& data) {

    size_t vlgPos = data.find("VLG");
    size_t me_vPos = data.find("ME_V");
    
    if (vlgPos != std::string::npos && me_vPos != std::string::npos) {
        size_t voltageStart = me_vPos + 5; // ME_V, の後ろから開始
        size_t voltageEnd = data.find("m", voltageStart); // 'V'の位置を探す
        
        if (voltageEnd != std::string::npos) {
            std::string voltageValue = data.substr(voltageStart, voltageEnd - voltageStart);

            float voltageFloat = std::stof(voltageValue);
            //int voltageInt = static_cast<int>(voltageFloat);

            update_chart(voltageFloat);
            isWriting = true;
            //update_chart(ui_Chart1, ui_Chart1_series_1, voltageInt);


            Serial.print("Extracted Voltage: ");
            Serial.println(voltageValue.c_str());
        }
    }

    if(data == "Command:"){

    if (expectedReply == "ble_command:MX2G") {
        setCheckboxChecked(ui_Checkbox601, true);
    } else if (expectedReply == "ble_command:MX21") {
        setCheckboxChecked(ui_Checkbox602, true);
    } else if (expectedReply.find("ble_command:MX2AS") != std::string::npos || 
    expectedReply.find("ble_command:DVV") != std::string::npos) {
        setCheckboxChecked(ui_Checkbox402, true);
    }else if (expectedReply.find("ble_command:ST") != std::string::npos) {
        Serial.println("Set time.");
        std::string timestamp = expectedReply.substr(14);// "頭14文字切り取る

        if (timestamp.length() == 12) {
            std::string yy = "20" + timestamp.substr(0, 2); // Year
            std::string mm = timestamp.substr(2, 2);         // Month
            std::string dd = timestamp.substr(4, 2);         // Day
            std::string hh = timestamp.substr(6, 2);         // Hour
            std::string min = timestamp.substr(8, 2);        // Minute
            std::string ss = timestamp.substr(10, 2);        // Second

            // Format into "20yy/mm/dd hh:mm:ss"
            std::string formattedDateTime = yy + "/" + mm + "/" + dd + " " + hh + ":" + min + ":" + ss;

            // Output or use the formatted string
            //lv_textarea_set_text(ui_TextArea604, formattedDateTime.c_str());

            auto txt = (char*)lv_mem_alloc(formattedDateTime.size() + 1);
            strcpy(txt, formattedDateTime.c_str());
            
            lv_async_call([](void* p){
                lv_textarea_set_text(ui_TextArea604, (const char*)p);
                lv_mem_free(p);
            }, txt);

        }
    }else if (expectedReply.find("ble_command:VV") != std::string::npos) {
        ver_get = true;

    }else if (expectedReply.find("ble_command:MX22") != std::string::npos) { // ← 追加
        mx22_done = true;
        
        needHeader = false;   // ← 応答確認後に落とす
        file_count = 0;       // 初期化したいならここで

        if (ui_auto_to_menu) {
            requestScreenChange(2, LV_SCR_LOAD_ANIM_MOVE_LEFT);
            ui_auto_to_menu = false;
        }

    }else if (expectedReply.find("ble_command:GN") != std::string::npos) { // ← 追加
      id_get = true ;
    }

      Serial.print(expectedReply.c_str());
      Serial.println("->END");
      GATT_command_receive = false;
      cmd_just_finished = true;

    // ★ Back待機中なら、コマンド完了をトリガにメニューへ
    if (nav_to_menu_after_cmd) {
        nav_to_menu_after_cmd = false;
        requestScreenChange(2, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    }

    
      return;
    }


if (expectedReply == "ble_command:GT") {

    String s = data.c_str();
    s.trim();
    if (s.endsWith(",")) s.remove(s.length()-1);   // 末尾カンマ対策

    String low = s; low.toLowerCase();

    // 1) RTC未設定か？
    if (low == "rtc not set") {
        userTime = {};
        userTime.tm_year = 2025 - 1900;
        userTime.tm_mon  = 0;
        userTime.tm_mday = 1;
        userTime.tm_hour = 0;
        userTime.tm_min  = 0;
        userTime.tm_sec  = 0;
        setRTCFromUserTime();
        printLocalTime();

        return;  // ここで抜ける
    }

    // 2) 時刻フォーマットか？
    bool looksLikeTime = (s.length() >= 19 &&
                          isdigit(s[0]) &&
                          s.indexOf('/') > 0 &&
                          s.indexOf(':') > 0);
    if (looksLikeTime) {
        if (inputTimeFromConsole(s)) {
            setRTCFromUserTime();
            printLocalTime();

        }
        return;  // ここで抜ける
    }

    // 3) どちらでもない（=ヘッダなど）は完全にスルー
    return;
}


             // キューに送信するためにバッファを作成
    if (can_write_now()) {
    // キューの残り容量を確認
      UBaseType_t queueSpaces = uxQueueSpacesAvailable(dataQueue);

      if (queueSpaces > 0) {  // 残りがある場合のみ送信
        char queueData[ble_reply];
        snprintf(queueData, sizeof(queueData), "%s", data.c_str());

        // キューにデータを送信
        if (xQueueSend(dataQueue, queueData, 0) != pdPASS) {
            Serial.println("Failed to send data to queue");
        } else {
            //Serial.println("Data sent to queue");
        }
      } else {
        // キューがいっぱいの場合
        Serial.println("Queue is full, skipping data send");
      }
    }

    Serial.print("Processing data: ");
    Serial.println(data.c_str());
}

class BLEDataProcessor {
public:
    void notifyCallback(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
        dataBuffer.insert(dataBuffer.end(), pData, pData + length);
        //lastPacketTime = millis();

        processDataBuffer();
    }
    
    void dropBuffers() {                 // ★これを追加！
        dataBuffer.clear();
        combinedData.clear();
    }

    std::string getProcessedData() {
        return std::string(processedData.begin(), processedData.end());
    }

    void clearProcessedData() {
        processedData.clear();
    }

private:
    void processDataBuffer() {

        while (dataBuffer.size() >= 20) {
// 先頭2バイトが無いなら判定不能
          if (dataBuffer.size() < 2) return;

          if (dataBuffer[0] == 'X' && dataBuffer[1] == 'Y') {
              // --- ヘッダ到着（ここで combining 開始＆監視スタート） ---
              if (dataBuffer.size() < 3) return;                 // ヘッダ未満
              if (!isCombining) { isCombining = true; lastPacketTime = millis(); }

              uint8_t len = dataBuffer[2];
              if (dataBuffer.size() < (size_t)(3 + len)) return; // 本文未到着（監視だけ継続）

              // --- 本文そろったので取り出し ---
              combinedData.insert(combinedData.end(),
                                  dataBuffer.begin() + 3,
                                  dataBuffer.begin() + 3 + len);
              dataBuffer.erase(dataBuffer.begin(), dataBuffer.begin() + 3 + len);
          }
          else if (dataBuffer[0] == 'X' && dataBuffer[1] == 'X') {
              if (dataBuffer.size() < 3) return;
              uint8_t len = dataBuffer[2];
              if (dataBuffer.size() < (size_t)(3 + len)) return;

              combinedData.insert(combinedData.end(),
                                  dataBuffer.begin() + 3,
                                  dataBuffer.begin() + 3 + len);

              std::string combinedDataStr(combinedData.begin(), combinedData.end());
              if (expectedReply == combinedDataStr) {
                  GATT_command_rep = true;
                  Serial.println("Reply recognized.");
              } else {
                  if (GATT_command_rep) { processData(combinedDataStr); }
              }

              combinedData.clear();
              dataBuffer.erase(dataBuffer.begin(), dataBuffer.begin() + 3 + len);

              // --- 完了したのでここで解除（処理成功後！） ---
              isCombining = false;
          }
          else {
              // 先頭が 'X','Y'/'X' でなければ1バイト捨てて再同期
              dataBuffer.erase(dataBuffer.begin());
          }
        }
    }
    std::vector<uint8_t> processedData;
};
BLEDataProcessor bleProcessor;

#if LV_USE_LOG != 0
/* Serial debugging */
void my_print(const char * buf)
{
    Serial.printf("%s", buf);
    Serial.flush();
}
#endif

/* Display flushing */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(disp);
}


lv_obj_t* screens[7] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
int current_screen = 0;
unsigned long previousMillis = 0;
const long interval = 3000; // 3 seconds interval

void debug_print(const char* message) {
    Serial.println(message);
}

class MyClientCallback : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) {
        Serial.println("Connected");
        connected = true;
        doScan = false;
    }

    void onDisconnect(NimBLEClient* pClient) {
        Serial.println("Disconnected");
        connected = false;            // 最優先で切断を反映
        GATT_command_receive = false; // UI整合のため下げておく（推奨）
        unint_disconnect = true ;
        //disconnect();
    }
};

class MyAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        if (advertisedDevice->haveServiceUUID() && advertisedDevice->isAdvertisingService(serviceUUID)) {
            for (NimBLEAdvertisedDevice* device : foundDevices) {
                if (device->getAddress().equals(advertisedDevice->getAddress())) {
                    if (!device->haveServiceUUID() && advertisedDevice->haveServiceUUID()) {
                        Serial.println("Updating device information with new UUID.");
                        *device = *advertisedDevice;
                    }
                    return;
                }
            }
            NimBLEAdvertisedDevice* newDevice = new NimBLEAdvertisedDevice(*advertisedDevice);
            foundDevices.push_back(newDevice);
        }
    }
};

void setup()
{

    //I2C　バッテリーゲージ
    Wire.begin();
    pwr_mgmt.attatch(Wire);

    //IO関連
    pinMode(switch_a, INPUT);
    pinMode(switch_b, INPUT);
    pinMode(Sensor_VP,INPUT);//Sensor_VPは入力専用なのであっても、なくてもいいけど。
    pinMode(Sensor_VN,INPUT);//Sensor_VNは入力専用なのであっても、なくてもいいけど。
    analogReadResolution(12);// ESP32 ADC は 12bit（0〜4095）
    analogSetPinAttenuation(Sensor_VP, ADC_11db); // 11dB → 最大約3.1Vまで測定
    analogSetPinAttenuation(Sensor_VN, ADC_11db);

    pinMode(sw_off, INPUT);
    pinMode(sw_led, OUTPUT);
    pinMode(BL_PIN,OUTPUT);

    //digitalWrite(sw_off, HIGH); //電源保持
    digitalWrite(sw_led,HIGH); //LED点灯
    digitalWrite(BL_PIN, HIGH);//LCDのバックライト

    //HSPIを使っているデバイス
    pinMode(XPT2046_CS, OUTPUT);
    pinMode(TFT_CS, OUTPUT);
    pinMode(SD_CS_PIN, OUTPUT);
    // 初期状態ではすべて非アクティブ
    digitalWrite(XPT2046_CS, HIGH);
    digitalWrite(TFT_CS, HIGH);
    digitalWrite(SD_CS_PIN, HIGH);


    SPI.begin(18, 19, 23);
    Serial.begin(115200); /* prepare for possible serial debug */
    
    flash_ok = FFat.begin(true);
    bool ts_ok = printLocalTime();

    if (ts_ok) {
      // filename は "ddhhmmss.csv" が入っている前提なので、ベース8桁を使って拡張子つけ直し
      char base[16];
      strncpy(base, filename, 8); base[8] = '\0';
      snprintf(flash_cur_path, sizeof(flash_cur_path), "%s%s%s", FLASH_LOG_PREFIX, base, log_ext_for_mode());
      g_time_named = true;
    } else {
      strncpy(flash_cur_path, boot_path_for_mode(), sizeof(flash_cur_path));
      flash_cur_path[sizeof(flash_cur_path)-1] = '\0';
      g_time_named = false;
    }

    if (!flash_ok) {
      Serial.println("FFat mount failed");
    } else {
      Serial.printf("FFat total:%u, used:%u\n",
                    (unsigned)FFat.totalBytes(), (unsigned)FFat.usedBytes());
    }

    Serial.printf("MODE_LCD=%d\n", MODE_LCD);
    
    String LVGL_Arduino = "Hello Arduino! ";
    LVGL_Arduino += String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();

    Serial.println(LVGL_Arduino);
    Serial.println("I am LVGL_Arduino");

    lv_init();

#if LV_USE_LOG != 0
    lv_log_register_print_cb(my_print); /* register print function for debugging */
#endif

    tft.begin();          /* TFT init */
    
    #if MODE_LCD == 1
      #pragma message "Build for ESP32-2432S028R"
      tft.setRotation(3); /*ESP32-2432S028R*/
    #elif MODE_LCD == 2
      #pragma message "Build for WROOM+MSP2807"
      tft.setRotation(1); /*MSP2807*/
    #endif
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, screenWidth * 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    touchscreen.begin();
    touchscreen.setRotation(3); // Corrected to match display orientation

    // Initialize the input device driver
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_t * my_indev = lv_indev_drv_register(&indev_drv);

    //タッチパネル・イニシャル
    ensure_touch_calibrated();
    lv_obj_clean(lv_scr_act());

    ui_init();

    // デバッグメッセージで各画面の初期化を確認
    if (ui_Screen1) Serial.println("Screen1 initialized");
    if (ui_Screen2) Serial.println("Screen2 initialized");
    if (ui_Screen3) Serial.println("Screen3 initialized");
    if (ui_Screen4) Serial.println("Screen4 initialized");
    if (ui_Screen5) Serial.println("Screen5 initialized");
    if (ui_Screen6) Serial.println("Screen6 initialized");
    if (ui_Screen7) Serial.println("Screen7 initialized");

    // 配列に画面オブジェクトを設定
    screens[0] = ui_Screen1;
    screens[1] = ui_Screen2;
    screens[2] = ui_Screen3;
    screens[3] = ui_Screen4;
    screens[4] = ui_Screen5;
    screens[5] = ui_Screen6;
    screens[6] = ui_Screen7;

    // LEDオブジェクトの初期化
    led_indicator = lv_obj_create(lv_scr_act());
    lv_obj_set_size(led_indicator, 15, 15);
    lv_obj_align(led_indicator, LV_ALIGN_RIGHT_MID, -10, 100);

    // ★丸にする
    lv_obj_set_style_radius(led_indicator, LV_RADIUS_CIRCLE, 0);

    // ★枠線の基本（黒）
    lv_obj_set_style_border_width(led_indicator, 2, 0);
    lv_obj_set_style_border_color(led_indicator, lv_color_black(), 0);

    // ★デフォは塗りなし（あとで状態で変える）
    lv_obj_set_style_bg_opa(led_indicator, LV_OPA_COVER, 0);

    //ステータスバーを作成
    statusbar_create();

    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Max power　P9
    NimBLEDevice::setSecurityAuth(true, true, true);

    pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pScan->setInterval(160);
    pScan->setWindow(15);
    pScan->setActiveScan(true);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 読み出し
    {
        String last;
        if (nvs_read_last_addr(last)) {
            lastConnectedAddress = NimBLEAddress(last.c_str());
            Serial.print("Last connected device address: ");
            std::string s = lastConnectedAddress.toString();
            Serial.println(s.c_str());
        } else {
            Serial.println("No last connected device address found.");
        }
    }

    {
      size_t free_bytes   = FFat.totalBytes() - FFat.usedBytes();
      size_t margin_bytes = calc_margin_bytes();
      if (free_bytes < margin_bytes && !g_sd_status_known.load()) {
        ensure_sd_status_known_now(); // ★ 先に判定しとく
      }
    }
  
  //Queue
  dataQueue   = xQueueCreate(QUEUE1_LENGTH, sizeof(char) * ble_reply);
  screenQueue = xQueueCreate(8, sizeof(ScreenMsg));

    if (dataQueue == NULL) {
        Serial.println("Failed to create queue");
        while(1);  // キュー作成に失敗したらプログラムを停止
    } else {
        Serial.println("Queue created successfully");
    }

  //Task
  xTaskCreatePinnedToCore(ble_task, "BLE Task", 8192, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(ui_task, "UI Task", 8192, NULL, 2,NULL, 1);

  load_aead_key_from_nvs();

  Serial.println("Setup done");

    //randomSeed(analogRead(0)); // ランダムシードを設定
    // オプションの動的設定
    //set_dropdown_options("New Option 1\nNew Option 2\nNew Option 3");

}

int current_index = 0;  // 新しいデータをどこに入れるかを示すカウンター
const int INVALID_VALUE = -111;  // 無効な値として -1 を使用

void update_chart(float new_value) {
    Serial.print("chart:");
    Serial.println(new_value);

    // 初回のみ無効な値で配列を初期化
    if (current_index == 0) {
        for (int i = 0; i < CHART_POINTS; i++) {
            ui_Chart1_series_1_array[i] = INVALID_VALUE;
        }
    }

    // データを追加（FIFO）
    if (current_index < CHART_POINTS) {
        ui_Chart1_series_1_array[current_index] = new_value * 1000;
        current_index++;
    } else {
        for (int i = 0; i < CHART_POINTS - 1; i++) {
            ui_Chart1_series_1_array[i] = ui_Chart1_series_1_array[i + 1];
        }
        ui_Chart1_series_1_array[CHART_POINTS - 1] = new_value * 1000;
    }

}

void loop()
{

 delay(1);
   
}

inline bool canSend() {
    return connected && pRxCharacteristic != nullptr && 
         !GATT_command_receive && !g_dump_in_progress;
}

void ble_task(void *pvParameters){

unsigned long lastChangeTime = 0; // GATT_command_receiveがfalseに変わった時刻
//bool id_get = false ; //デバイスからIDを取得しているか、どうか。

while(1){
    vTaskDelay(pdMS_TO_TICKS(1));

    unsigned long currentTime = millis();
    bool uart_send = false;
    static String input = "";

    // --- Dump execution (disconnect / soft-margin / emergency 共通) ---
    if (g_dump_request.load() && !g_dump_in_progress.load()) {
        g_dump_in_progress.store(true);

        bool from_disconnect = g_post_cleanup_pending.load();

        // まず SD 状態を確認（ここで SD.begin + g_sd_absent 更新）
        ensure_sd_status_known_now(true);
        bool can_dump_to_sd = !g_sd_absent.load();

        // disconnect 由来 かつ SD があって Dump できる場合だけ処理中画面へ
        if (from_disconnect && can_dump_to_sd) {
            requestScreenChange(6, LV_SCR_LOAD_ANIM_MOVE_LEFT);
        }

        // Dump 前に FFat 追記を止める
        if (ffatLog) {
            ffatLog.flush();
            ffatLog.close();
        }
        doScan = false;

        bool dumped = false;
        SystemState prev_state = g_state.load();  // Dump 開始前の状態を記録

        if (can_dump_to_sd) {
            // SDあり → 本当に Dump 実行
            dumped = dumpAllFlashCsvToSD_AndPurge();
        } else {
            // SD無し → dumpAll は呼ばない（すぐ戻る）
            dumped = false;
        }

        g_dump_request.store(false);
        g_dump_in_progress.store(false);

        // ---- disconnect 以外（IdleDump / EMERGENCY）由来の Dump ----
        if (!from_disconnect) {
            if (dumped) {
                // EMERGENCY / WAIT_SD → 回復用に POST_DUMP_ROTATE
                if (prev_state == SystemState::EMERGENCY ||
                    prev_state == SystemState::WAIT_SD) {
                    g_state.store(SystemState::POST_DUMP_ROTATE);
                } else {
                    g_state.store(SystemState::NORMAL);
                }
            } else {
                // dumped == false の理由を判定
                if (g_sd_absent.load()) {
                    // ホンマに SD 無い or SD.init 失敗 → WAIT_SD（「刺さるの待ち」）
                    g_state.store(SystemState::WAIT_SD);
                } else {
                    // SD はあるけど容量不足 or write失敗 → もう自動Dumpは諦める
                    g_state.store(SystemState::NORMAL);
                    g_defer_dump_until_rotate.store(false);
                    // 必要ならここで「Dump一部失敗（容量不足など）」を Serial に出す
                    Serial.println("[DumpGuard] dump failed but SD present -> stop auto-retry (likely full)");
                }
            }
        }
        // ---- disconnect 由来の Dump ----
        else {
            if (dumped) {
                // Dump成功時だけ「セッション終了 cleanup」を実行
                is_measuring = false;
                doConnect    = false;
                connected    = false;
                doScan       = true;
                GATT_command_rep     = false;
                GATT_command_receive = false;
                re_try               = false;

                rtc_sync  = false;
                id_get    = false;
                ver_get   = false;
                mx22_done = false;
                file_count = 0;

                log_ready        = false;
                first_file_prepared = false;
                g_time_named     = false;
                s_file_id        = 0;

                // Dump成功後の状態としては NORMAL に戻す
                g_state.store(SystemState::NORMAL);

                // UI に「ホームへ戻してOK」のシグナル
                g_post_cleanup_pending.store(true);
            } else {
                // Dump失敗（SD無しなど）のとき：

                // 1) BLE / 計測状態リセット（今のままでOK）
                is_measuring = false;
                doConnect    = false;
                connected    = false;
                doScan       = true;
                GATT_command_rep     = false;
                GATT_command_receive = false;
                re_try               = false;

                // 2) 初期化フラグもリセットして「次回接続で必ず GT/GN/VV/MX22 が走る」ようにする
                rtc_sync  = false;
                id_get    = false;
                ver_get   = false;
                mx22_done = false;
                needHeader = false;
                file_count = 0;

                // 3) ログセッション状態もリセット（次セッションで BOOT から始める）
                log_ready          = false;
                first_file_prepared = false;
                g_time_named       = false;
                s_file_id          = 0;

                // 4) 状態は NORMAL に戻す（書き込み可にしておく）
                g_state.store(SystemState::NORMAL);

                // 5) UI cleanup は走らせない → g_post_cleanup_pending は立てない
                //    （画面は disconnect 時の Screen0 で止まる想定）
            }
        }
    }

    bool expected = true;
    if (unint_disconnect.compare_exchange_strong(expected, false)) {
        // ここに来た＝false→trueの立ち上がりを1回だけ検出してfalseに戻せた
        if (!g_post_cleanup_pending.load() && !g_dump_in_progress.load()) {
            int c = g_disconnect_calls.fetch_add(1) + 1;  // 実コール時だけカウント
            Serial.printf("[TEST] disconnect() called, count=%d\n", c);
            disconnect();
        } else {
            // 既に切断フロー中（ダンプ/後片付け中）なので二重実行はしない
            Serial.println("[TEST] skip disconnect(): cleanup/dump in progress");
        }
    }

    if (doConnect) {
        //Serial.println("call handleConnection start");
        handleConnection();
        //Serial.println("call handleConnection end");
    }

    if (doScan) {
        //Serial.println("call handleScanning start");
        handleScanning();
        //Serial.println("call handleScanning end");
    }

    if (Serial.available() > 0) {
       handleSerialInput(input, uart_send);
    }

    if (!connected && !doScan && uart_send) {
        handleDeviceSelection(input);
    } else if (connected && !doScan && uart_send ) {
        handleUartCommunication(input);
        uart_send = false;
    }

    if (canSend() && (cmd_just_finished || needHeader || !rtc_sync)) {
        cmd_just_finished = false;  // ★ 先に落として再入防止
        lastChangeTime = millis();  // 変化した時刻を記録
        //Serial.println("GATT_command_receive changed from T to F.");
       if (!rtc_sync) {
        String command = "GT" ;
        handleUartCommunication(command);
       }
       else if (!id_get) {
        String command1 = "GNM" ;
        handleUartCommunication(command1);
        while (GATT_command_receive) vTaskDelay(pdMS_TO_TICKS(10));
        String command2 = "GNA" ;
        handleUartCommunication(command2);
       }
       else if (!ver_get) {
        String command = "VV" ;
        handleUartCommunication(command);
       }
       else if (!mx22_done) {
        String command = "MX22" ;
        handleUartCommunication(command);
       }       


    }

    checkPacketTimeout(currentTime);

    bool rotation_window = rollPending
    && !GATT_command_receive
    && (g_state.load()==SystemState::NORMAL || g_state.load()==SystemState::SOFT_MARGIN)
    && (millis() - g_last_rotate_fail_ms >= ROTATE_RETRY_MS);
  
    if (canSend() && is_measuring
        && (millis() - lastChangeTime >= 500)
        && !rotation_window            // ← 本当にローテ行ける瞬間だけブロック
        && !needHeader && rtc_sync && ver_get && mx22_done) {
        String command;
        if (measurment_mode ) { 
            command = "MSC" ;
        }
        else if ( !measurment_mode  ) {
            command = "MX2ES" ;
        }
     
        handleUartCommunication(command);
    }
}
}

void ui_task(void *pvParameters) {

  //_ui_screen_change(&screens[0], LV_SCR_LOAD_ANIM_NONE, 0, 0, NULL); 
  requestScreenChange(0, LV_SCR_LOAD_ANIM_NONE);

  while (1) {

    // ui_task() 冒頭の while(1) 内の適当な早い位置に
    static SystemState prev_state = g_state.load();
    SystemState cur = g_state.load();

    int raw = analogRead(Sensor_VP);
    float voltage = (float)raw / 4095.0 * 3.1;
    
    if (voltage <= 1.4f) {
        if (!g_sensor_vp_high) {
            g_sensor_vp_high = true;

            if (avl_busy()) {
                Serial.println("[POW] poweroff blocked: measuring");
                show_measuring_block_dialog();
            } else {
                show_poweroff_dialog();
            }
        }
    }else if (voltage >= 1.5f) {
        // 少し下がるまでは「高レベル」のままにしてチャタリングを防止
        g_sensor_vp_high = false;
    }

    if (cur != prev_state) {
      if (cur == SystemState::EMERGENCY) {
        // 閾値割れを検知した瞬間に安全側でファイルを閉じる
        if (ffatLog) {
          ffatLog.flush();
          ffatLog.close();
          Serial.println("[Guard] EMERGENCY: closed active FFat log immediately");
        }
      }
      prev_state = cur;
    }

    if (!first_file_prepared.load() && initial_ready()) {
      s_file_id = 0;
      if (!can_write_now()) {
        // BOOT のまま追記継続。空きが戻る/SDが入るまで待つ
        g_time_named.store(false);
        log_ready.store(true);
        first_file_prepared.store(false);
        Serial.println("[FFat] skip first rotate (not writable now)");
      } else {
        bool ok = flash_rotate_to_new_file();
        if (ok) {
          g_time_named.store(true);
          log_ready.store(true);
          first_file_prepared.store(true);
          Serial.println("[FFat] first time-named log prepared.");
        } else {
          g_time_named.store(false);
          log_ready.store(true);
          first_file_prepared.store(false);
          Serial.println("[FFat] first time-named log FAILED -> keep BOOT path");
        }
      }
    }
    
      // 保存条件の判定
    if (!g_dump_in_progress.load() && can_write_now()) {
      int queue_num = uxQueueMessagesWaiting(dataQueue);
      if (queue_num >= QUEUE_THRESHOLD) {
        saveToFlash();
      } else if (queue_num >= 1 && millis() - lastSaveTime >= TIME_THRESHOLD) {
        saveToFlash();
      }
    }
    if (isWriting) {
      isWriting = false;
      redraw_chart(ui_Chart1, ui_Chart1_series_1);  // チャートの再描画
    }
      /* 画面切替要求をチェック */
        ScreenMsg msg;
        if (xQueueReceive(screenQueue, &msg, 0) == pdPASS) {
            lv_obj_t* target = (msg.index < 7) ? screens[msg.index] : nullptr;

            // 追加の保険：NULLや破棄済みを弾く
            if (target && !lv_obj_is_valid(target)) {
                Serial.printf("[UI] invalid (freed?) screen ptr at index=%u\n", msg.index);
                target = nullptr;
            }

            if (target) {
                lv_obj_t* act = lv_scr_act();
                lv_obj_t* prev = lv_disp_get_scr_prev(lv_disp_get_default()); // v8で利用可
                if (act != target && prev != target) {  // 実際に違う画面のときだけ遷移
                    _ui_screen_change(&screens[msg.index], msg.anim, 0, 0, kInit[msg.index]);
                    current_screen = msg.index;
                    Serial.printf("[UI] switched -> idx=%u\n", msg.index);
                }
            } else if (!target) {
                Serial.printf("[UI] invalid screen index=%u or NULL/invalid screen ptr\n", msg.index);
            }
        }

        //デバック　Heap FFat監視
        static unsigned long last_report = 0;
        if (millis() - last_report > 10000) {  // 10秒ごと
          monitor_status();
          last_report = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(30));
        lv_task_handler();  // 描画中でない場合にタスクを進める

        // ★ ステータスバー更新（30msごと）
        statusbar_update();

      lv_obj_set_style_border_width(led_indicator, 2, 0);
      lv_obj_set_style_border_color(led_indicator, lv_color_black(), 0);

      if(!connected){
        // ★赤は「黒枠○＋中は白（塗りなし）」
        lv_obj_set_style_bg_opa(led_indicator, LV_OPA_TRANSP, 0);  // 塗りなし
      }
      else if (re_try){
        lv_obj_set_style_bg_opa(led_indicator, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(led_indicator, lv_color_make(0xFF, 0xFF, 0x00), 0); // 黄
      }
      else if (GATT_command_receive) {
        lv_obj_set_style_bg_opa(led_indicator, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(led_indicator, lv_color_make(0x00, 0xFF, 0x00), 0); // 緑
      }
      else {
        lv_obj_set_style_bg_opa(led_indicator, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(led_indicator, lv_color_make(0x00, 0x80, 0xFF), 0); // 青
      }

    // ------------------ SD ローテーション ------------------
    static uint32_t g_last_cooldown_log_ms = 0;
    if (rollPending) {
      // ★ 直前にローテ失敗してたら、しばらく回避（スパム防止）
      if (millis() - g_last_rotate_fail_ms < ROTATE_RETRY_MS) {
        // クールダウン期間中はスキップ
        if (millis() - g_last_cooldown_log_ms > 10000) {
          Serial.println("[Guard] rotation cooldown (skip retry)");
          g_last_cooldown_log_ms = millis();
        }
      }
      else if (!GATT_command_receive
            && !needHeader
            && (g_state.load()==SystemState::NORMAL || g_state.load()==SystemState::SOFT_MARGIN))
      {
        // ★ ローテ直前：旧ファイルに残キューを書き切る
        int qn = uxQueueMessagesWaiting(dataQueue);
        if (qn > 0) saveToFlash();

        needHeader  = true; // ← 先に立てる
        rollPending = false;// ← これを後に回す

        bool ok = flash_rotate_to_new_file();
        if (ok) {
          file_count  = 0;
          id_get      = false;
          ver_get     = false;
          ui_auto_to_menu = false;
          mx22_done   = false;
          Serial.println("[FFat] rotation completed.");
          if (g_defer_dump_until_rotate.load()) {
            g_dump_request = true;
            g_defer_dump_until_rotate = false;
            Serial.println("[Guard] rotation finished → DUMP now (deferred)");
          }
        } else {
          Serial.println("[FFat] rotation FAILED → will retry later");
          g_last_rotate_fail_ms = millis();   // ★ 失敗記録
          rollPending = true;                 // 次回に再挑戦
          needHeader  = false;                // 旧ファイル継続
        }
      }
    }

    // === アイドル時の“境界作成→即Dump”ブリッジ ===============================
    bool idle_ok =
      !GATT_command_receive &&
      (millis() - lastSaveTime >= IDLE_DUMP_AFTER_MS) &&
      can_write_now();

    if (g_state.load() == SystemState::SOFT_MARGIN
    && g_defer_dump_until_rotate.load()
    && idle_ok) {
      // 連発防止
      if (millis() - g_last_idle_dump_ms >= IDLE_DUMP_COOLDOWN_MS) {

        // 残キューを吐き切ってから境界を切る（行欠け防止）
        int qn = uxQueueMessagesWaiting(dataQueue);
        if (qn > 0) saveToFlash();

        // “境界”を人工的に作る → ローテ実行
        needHeader = true;        // 新ファイル先頭でGN/VVを要求（従来と同じ流儀）
        bool ok = flash_rotate_to_new_file();
        if (ok) {
          file_count  = 0;
          id_get      = false;
          ver_get     = false;
          ui_auto_to_menu = false;
          mx22_done   = false;
          Serial.println("[IdleDump] idle boundary -> rotate OK");

          // 直ちにDumpへ（従来のローテ完了→Dumpと同じ動作に揃える）
          g_defer_dump_until_rotate = false;
          g_dump_request = true;
          g_last_idle_dump_ms = millis();
          Serial.println("[IdleDump] request DUMP (idle)");
        } else {
          // 失敗時はのちほど再挑戦（スパム防止は既存の g_last_rotate_fail_ms でOK）
          Serial.println("[IdleDump] rotate FAILED, will retry later");
          needHeader = false;  // 旧ファイル継続
          g_last_rotate_fail_ms = millis();
        }
      }
    }
    // =======================================================================

    // PATCH G: State-driven Dump / Rotate
    switch (g_state.load()) {
      case SystemState::EMERGENCY: {
        // SDがあれば即Dump、なければWAIT_SD
        ensure_sd_status_known_now(true);
        if (!g_sd_absent.load()) {
          g_state.store(SystemState::DUMPING);
          g_dump_request.store(true);
        } else {
          g_state.store(SystemState::WAIT_SD);
        }
        break;
      }
      case SystemState::WAIT_SD: {
        // クールダウンに従って再判定
        ensure_sd_status_known_now();
        if (!g_sd_absent.load()) {
          g_state.store(SystemState::DUMPING);
          g_dump_request.store(true);
        }
        break;
      }
      case SystemState::DUMPING:
        // 実処理は下の Dump 実行ブロックで行われ、成功/失敗後に状態を変える
        break;

      case SystemState::POST_DUMP_ROTATE: {
        // Dump成功後のローテ専用フェーズ（Dumpで空きは確保済みの前提）
        if (!GATT_command_receive && !needHeader) {
          // 残キューを吐く（安全）
          int qn = uxQueueMessagesWaiting(dataQueue);
          if (qn > 0 && can_write_now()) saveToFlash();

          needHeader = true;
          bool ok = flash_rotate_to_new_file();
          if (ok) {
            file_count = 0;
            id_get = ver_get = mx22_done = false;
            g_defer_dump_until_rotate.store(false);
            g_state.store(SystemState::NORMAL);
          } else {
            // 次回再試行
            if (millis() - g_last_rotate_fail_ms >= ROTATE_RETRY_MS) {
              g_last_rotate_fail_ms = millis();
            }
          }
        }
        break;
      }
      default:
        break;
    }

    // ダンプ後の一括後片付け（最終状態をここで決める）
    if (g_post_cleanup_pending && !g_dump_in_progress) {

      SystemState s = g_state.load();

      // POST_DUMP_ROTATE 中はまだローテ中なので待つ
      if (s == SystemState::POST_DUMP_ROTATE) {
        // 次のループで NORMAL / WAIT_SD になってから処理
      } else {
        // BLE側の論理cleanupは終わっている想定なので
        // ここでは画面だけ戻す
        g_post_cleanup_pending = false;
        requestScreenChange(0, LV_SCR_LOAD_ANIM_NONE);
      }
    }

    if (g_poweroff_request.load() && !g_poweroff_in_progress.load()) {
        g_poweroff_request = false;
        perform_safe_poweroff();
    } 

  }

}

void redraw_chart(lv_obj_t* chart, lv_chart_series_t* series) {
    static int offset = 0;
    static int scall = 0;
    static float scale_factor = 0.001;

    // 最小値と最大値を計算
    float min = INT_MAX;
    float max = INT_MIN;

    int num;
    for (num = 0; num < CHART_POINTS; num++) {
        if (ui_Chart1_series_1_array[num] != INVALID_VALUE) {
          if (ui_Chart1_series_1_array[num] < min) min = ui_Chart1_series_1_array[num];
          if (ui_Chart1_series_1_array[num] > max) max = ui_Chart1_series_1_array[num];
        } else {
        break;
        }
    }

    if (num > 0) {  // 配列の範囲外アクセスを防ぐ
      float voltageValue = (ui_Chart1_series_1_array[num - 1])/1000;
      char buffer[32];
      snprintf(buffer, sizeof(buffer), "%.3f",voltageValue);
      lv_textarea_set_text(ui_TextArea3, buffer);
      lv_textarea_set_text(ui_TextArea603, buffer);
    }

    // カスタムラベルの削除（古いラベルをクリア）
    static lv_obj_t* min_label = NULL;
    static lv_obj_t* max_label = NULL;
    if (min_label) lv_obj_del(min_label);
    if (max_label) lv_obj_del(max_label);
    min_label = NULL;
    max_label = NULL;

    // 範囲が狭い場合のスケーリング
    if (abs(max - min) < 10000) {
        offset = (int)(floor(min * 0.001) * 1000);
        scall = (int)(ceil(max * 0.001) * 1000);
        scale_factor = 0.1;
        lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, (scall - offset) * scale_factor);

        if(current_screen == 4){
          // `min` ラベルを作成
          min_label = lv_label_create(lv_scr_act());
          char min_text[10];
          snprintf(min_text, sizeof(min_text), "%d", (offset / 1000));
          lv_label_set_text(min_label, min_text);
          lv_obj_align_to(min_label, chart, LV_ALIGN_OUT_LEFT_BOTTOM, -10, 0);

          // `max` ラベルを作成
          max_label = lv_label_create(lv_scr_act());
          char max_text[10];
          snprintf(max_text, sizeof(max_text), "%d", (scall / 1000));
          lv_label_set_text(max_label, max_text);
          lv_obj_align_to(max_label, chart, LV_ALIGN_OUT_LEFT_TOP, -10, 0);
        }

        lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_Y, 5, 5, 2, 5, false, 70);
    } else {
        offset = 0;
        scale_factor = 0.001;
        lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, (int)min / 1000, (int)max / 1000);

        lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_Y, 5, 2, 5, 2, true, 70);
        lv_obj_set_style_text_color(chart, lv_color_make(0, 0, 0), LV_PART_TICKS | LV_STATE_DEFAULT);
    }

    // チャートデータを更新
    for (int i = 0; i < CHART_POINTS; i++) {
        if (ui_Chart1_series_1_array[i] != INVALID_VALUE) {
            lv_chart_set_value_by_id(chart, series, i, (int)(ui_Chart1_series_1_array[i] - offset) * scale_factor);
        } else {
            lv_chart_set_value_by_id(chart, series, i, LV_CHART_POINT_NONE);
        }
    }
}

void handleConnection() {
    doConnect = false;
    doScan = false;
    
    if (pClient == nullptr) {
        // 初回のみクライアントを作成
        pClient = NimBLEDevice::createClient();
        pClient->setClientCallbacks(new MyClientCallback(), false);
        Serial.println("New client created");
    } else {
        // 再利用の場合
        Serial.println("Reusing existing client");
        if (pClient->isConnected()) {
            pClient->disconnect();  // 既存の接続がある場合は切断
        }
    }

    int retryCount = 5;
    while (retryCount > 0) {
        if (pClient->connect(myDevice)) {
            Serial.println("Connected to device");

            // ★ ここでNVSに書く前後で文字列を一度保持して使う
            std::string addrStr = myDevice->getAddress().toString();
            nvs_write_last_addr(addrStr.c_str());

            // ★ RAMキャッシュも即更新（これが肝）
            lastConnectedAddress = NimBLEAddress(addrStr);
            Serial.printf("[RAM] lastConnectedAddress set to %s\n", addrStr.c_str());

            NimBLERemoteService* pService = pClient->getService(serviceUUID);
            if (pService != nullptr) {
                NimBLERemoteCharacteristic* pTxCharacteristic = pService->getCharacteristic(charUUID_TX);
                pRxCharacteristic = pService->getCharacteristic(charUUID_RX);

                if (pTxCharacteristic != nullptr && pTxCharacteristic->canNotify()) {
                    bool success = pTxCharacteristic->subscribe(true, std::bind(&BLEDataProcessor::notifyCallback, &bleProcessor, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
                    Serial.print("sucess:");
                    Serial.println(success);
                }
            }
            re_try = false;

            //_ui_screen_change(&screens[2], LV_SCR_LOAD_ANIM_MOVE_LEFT, 0, 0, NULL);
            requestScreenChange(6, LV_SCR_LOAD_ANIM_MOVE_LEFT);
            ui_auto_to_menu = true;   // ← 初回イニシャル中だけ true
            vTaskDelay(pdMS_TO_TICKS(1000)); // 1000ms待機
            String command = "SB";
            handleUartCommunication(command);
            
            break;
        } else {
            Serial.println("Retrying to connect...");
            retryCount--;
            re_try = true;

            // スリープを細かく分割して他のタスクに制御を渡す
            for (int i = 0; i < 1000; i += 10) {
                vTaskDelay(pdMS_TO_TICKS(10)); // 10ms待機
            }
        }
    }
    if (retryCount == 0) {
        Serial.print("Failed to connect after retries - Address: ");
        std::string s = myDevice->getAddress().toString();
        Serial.println(s.c_str());
        unint_disconnect = true;
    }
}

void handleScanning() {
    Serial.println("Starting scan...");
    
    for (auto* d : foundDevices) delete d;
    foundDevices.clear();
    
    pScan->start(scanInterval, false);
    doScan = false;
    doConnect = false;

    if (foundDevices.size() > 0) {
        Serial.println("Scan complete.");
        Serial.println("Found devices:");

        char options[1024] = "";  // 必要に応じてサイズを調整
        for (int i = 0; i < foundDevices.size(); i++) {
            // デバイスアドレスのみを取得
			std::string addrStr = foundDevices[i]->getAddress().toString();
			const char* address = addrStr.c_str();
			Serial.println(address);//デバック
			strncat(options, address, sizeof(options) - strlen(options) - 1);
			strncat(options, "\n", sizeof(options) - strlen(options) - 1);

            // 自動接続処理
            if (foundDevices[i]->getAddress().equals(lastConnectedAddress) && !connected) {
                Serial.println("Found a previously connected device. ");
                myDevice = foundDevices[i];
                doScan = false;
                doConnect = true;

                vTaskDelay(pdMS_TO_TICKS(1000)); // 1000ms待機
                return;
                //break;
            } else {
                Serial.print("Skip AutoConnect. connected=");
                Serial.print(connected ? "true" : "false");
                Serial.print(", pClient=");
                Serial.print(pClient ? (pClient->isConnected() ? "true" : "false") : "null");
                Serial.print(", lastConnectedAddress=");
                Serial.print(lastConnectedAddress.toString().c_str());
                Serial.print(", found=");
                Serial.println(addrStr.c_str());
            }
        }

        // 最後に改行を削除
        size_t len = strlen(options);
        if (len > 0 && options[len - 1] == '\n') {
            options[len - 1] = '\0';
        }

        set_dropdown_options(options);
        
        vTaskDelay(pdMS_TO_TICKS(1000)); // 1000ms待機
        requestScreenChange(1, LV_SCR_LOAD_ANIM_MOVE_RIGHT);


    } else {
        Serial.println("No devices found.");
        doScan = true;
    }
}

void handleSerialInput(String& input, bool& uart_send) {
    char receivedChar = Serial.read();
    input += receivedChar;

    if (receivedChar == '\n') {
        input.trim();
        uart_send = true;
    }
}

void handleDeviceSelection(String& input) {
    int deviceIndex = input.toInt();
    if (deviceIndex > 0 && deviceIndex - 1 < foundDevices.size()) {
        Serial.print("Select No: ");
        Serial.println(deviceIndex);
        myDevice = foundDevices[deviceIndex - 1];
        doScan = false;
        doConnect = true;
        input = "";
    } else {
        Serial.println("Invalid device number -> rescan");
        // ★以前は unint_disconnect = true; だったのを削除
        doScan = true;       // これだけで十分
        doConnect = false;
        input = "";
        requestScreenChange(0, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    }
}

void handleUartCommunication(String& input) {
  if (input.equals("ME")) { GATT_command_receive = false; }

  if (GATT_command_receive) {
    Serial.println("AVL is busy.");
    input = "";
    return;
  }

  // 1) disconnect は最優先で処理して抜ける
  if (input.equals("disconnect")) {
    if (pClient && pClient->isConnected()) {
      Serial.println("Disconnecting...");
      unint_disconnect = true;
      eraseNVSKey();
    } else {
      Serial.println("No device connected");
      doScan = true;
      doConnect = false;
    }
    input = "";
    return;  // ★ここで終了
  }

#ifdef DEBUG
  // 2) デバッグ: dummy(N) / dummy(0) は BLE送信より前で食う
  if (input.indexOf("dummy") >= 0) {       // もっと厳密なら startsWith("dummy(")
    if (processDebugSerialCommand(input)) {
      input = "";
      return;  // ★ここで終了（通常送信しない）
    }
    // パース失敗などなら fall-through
  }
#endif

  // 3) それ以外は通常のBLE送信
  if (pRxCharacteristic != nullptr && connected) {
    int maxRetries = 5;
    int retries = 0;
    GATT_command_rep = false;

    expectedReply = std::string("ble_command:") + std::string(input.c_str());

    unsigned long startTime = millis();
    unsigned long interval = 2000;

    pRxCharacteristic->writeValue(input.c_str());
    GATT_command_receive = true;
    Serial.print("Sent: "); Serial.println(input);

    char queueData[ble_reply];
    snprintf(queueData, sizeof(queueData), "command:%s", input.c_str());
    if (can_write_now()) {
      if (xQueueSend(dataQueue, queueData, 0) != pdPASS) {
        Serial.println("Failed to send data to queue");
      }
    }

    while (retries < maxRetries) {
      if (GATT_command_rep || !connected) break;
      if (millis() - startTime >= interval) {
        startTime = millis();
        retries++;
        re_try = true;
        Serial.println("Retrying...");
        pRxCharacteristic->writeValue(input.c_str());
        Serial.print("Sent: "); Serial.println(input);
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!GATT_command_rep) {
      Serial.println("Failed to receive expected reply after retries.");
      unint_disconnect = true;
    }

    re_try = false;
    input = "";
    return;
  }

  // 4) 未接続
  Serial.printf("Skip send (not connected): %s\n", input.c_str());
  doScan = true;
  input = "";
}

void checkPacketTimeout(unsigned long /*unused*/) {
  uint32_t now = millis();
  if (now < lastPacketTime) {          // ロールオーバ/レース
    lastPacketTime = now;
    return;
  }
  if (isCombining) {
    uint32_t delta = now - lastPacketTime;
    if (delta >= timeoutInterval) {
      Serial.printf("Packet timeout (%lu ms). Clearing buffer.\n", (unsigned long)delta);
      bleProcessor.dropBuffers();
      isCombining = false;
    }
  }
}

// 特定のキーを消去する関数
void eraseNVSKey() {
    if (nvs_erase_last_addr()) {
        // NVSから消しただけ。メモリ上のキャッシュもクリアしとく
        lastConnectedAddress = NimBLEAddress(); // クリア
        Serial.println("lastConnectedAddress cleared (RAM).");
    } else {
        Serial.println("eraseNVSKey(): failed (see logs above).");
    }
}

void disconnect(){
  // 送信中止だけ（重いI/Oはしない）
  is_measuring = false;
  
  // ダンプをUIタスクへ依頼（ここでは *doScan をいじらない*）
  printLocalTime();  // filename[] 更新
  snprintf(g_dump_name, sizeof(g_dump_name), "%s", filename);
  g_dump_request = true;
  g_post_cleanup_pending = true;

  // BLE切断だけ先に
  if (pClient && pClient->isConnected()) pClient->disconnect();

  // ※ここでは doScan=true に戻さない／各種フラグも触らない
}

bool inputTimeFromConsole(const String& input) {
  struct tm t = {};
  int yy, MM, dd, hh, mm, ss;

  // 正規化（末尾カンマ/空白を除去）
  String norm = input;
  norm.trim();
  if (norm.endsWith(",")) norm.remove(norm.length()-1);

  if (sscanf(norm.c_str(), "%d/%d/%d %d:%d:%d",
             &yy, &MM, &dd, &hh, &mm, &ss) == 6) {
    t.tm_year = yy - 1900;
    t.tm_mon  = MM - 1;
    t.tm_mday = dd;
    t.tm_hour = hh;
    t.tm_min  = mm;
    t.tm_sec  = ss;
    userTime = t;
    Serial.println("Date and time successfully set to Time struct.");
    return true;
  } else {
    Serial.println("Invalid input format.");
    return false;
  }
}

void setRTCFromUserTime() {
  time_t t = mktime(&userTime); // 構造体からUNIXタイムを生成
  struct timeval now = { .tv_sec = t };
  settimeofday(&now, NULL);    // ESP32のRTCに設定
  Serial.println("ESP32's RTC is set successfully.");
  rtc_sync = true;
}

bool printLocalTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("ESP32's RTC Failed to obtain time");
    return false;
  }
  Serial.print("ESP32 RTC:");
  Serial.println(&timeinfo, "%Y/%m/%d %H:%M:%S");  // 時刻を"YYYY/MM/DD HH:MM:SS"の形式で出力

  snprintf(filename, sizeof(filename), "%02d%02d%02d%02d.csv", 
          timeinfo.tm_mday,    // Day of the month (dd)
          timeinfo.tm_hour,    // Hour (hh)
          timeinfo.tm_min,     // Minute (mm)
          timeinfo.tm_sec);    // Second (ss)
  
  return true;
}

/************** 1) ヘッダと構造体・ユーティリティ **************/

struct TouchCal {
  float a, b, c;  // x' = a*xr + b*yr + c
  float d, e, f;  // y' = d*xr + e*yr + f
  bool  valid;
};

static TouchCal g_cal = {0,0,0,0,0,0,false};

// 画面サイズ（既存の定義を利用）
extern const uint16_t screenWidth;   // = 320
extern const uint16_t screenHeight;  // = 240

// クロスヘアを描く簡易関数（LVGL）
static lv_obj_t* draw_cross(lv_coord_t x, lv_coord_t y) {
  const int len = 10;
  lv_obj_t* line1 = lv_line_create(lv_scr_act());
  static lv_point_t pts1[2];
  pts1[0] = { (lv_coord_t)(x - len), y };
  pts1[1] = { (lv_coord_t)(x + len), y };
  lv_line_set_points(line1, pts1, 2);

  lv_obj_t* line2 = lv_line_create(lv_scr_act());
  static lv_point_t pts2[2];
  pts2[0] = { x, (lv_coord_t)(y - len) };
  pts2[1] = { x, (lv_coord_t)(y + len) };
  lv_line_set_points(line2, pts2, 2);

  // まとめて扱いたいのでコンテナに入れる
  lv_obj_t* cont = lv_obj_create(lv_scr_act());
  lv_obj_set_size(cont, 1, 1);
  lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
  lv_obj_align(cont, LV_ALIGN_TOP_LEFT, x, y);
  lv_obj_set_style_border_opa(cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_style_radius(cont, 0, 0);
  lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_parent(line1, cont);
  lv_obj_set_parent(line2, cont);
  return cont;
}

// タッチ取得（安定化：複数回読んで中央値）
static bool read_raw_touch(int &rx, int &ry, int samples = 10) {
  if (!touchscreen.touched()) return false;
  int xs[20], ys[20];
  samples = min(samples, 20);
  for (int i=0;i<samples;i++) {
    if (!touchscreen.touched()) return false;
    TS_Point p = touchscreen.getPoint();
    xs[i] = p.x;
    ys[i] = p.y;
    delay(5);
  }
  // 中央値
  auto nth = samples/2;
  std::nth_element(xs, xs+nth, xs+samples);
  std::nth_element(ys, ys+nth, ys+samples);
  rx = xs[nth];
  ry = ys[nth];
  return true;
}

/************** 2) アフィン係数の計算（最小二乗 5点） **************/
/*
  生(raw) → 画面(screen) のアフィン
  [x']   [a b c][xr]
  [y'] = [d e f][yr]
                [1 ]
*/
static bool solve_affine_least_squares(
  const int raw_x[], const int raw_y[],
  const int scr_x[], const int scr_y[],
  int n, TouchCal &out)
{
  if (n < 3) return false;

  // 正規方程式の 6x6 行列 A と 6x1 ベクトル B を構築
  double A[6][6] = {0};
  double B[6]    = {0};

  auto add_row = [&](double xr, double yr, double xs, double ys){
    double rowx[6] = { xr, yr, 1.0, 0,  0,  0 };
    double rowy[6] = { 0,  0,  0,   xr, yr, 1.0 };

    // A += rowx^T * rowx
    for (int r=0;r<6;r++){
      for(int c=0;c<6;c++){
        A[r][c] += rowx[r]*rowx[c] + rowy[r]*rowy[c];
      }
    }
    // B += rowx^T * xs + rowy^T * ys
    B[0] += xr*xs;  B[1] += yr*xs;  B[2] += 1.0*xs;
    B[3] += xr*ys;  B[4] += yr*ys;  B[5] += 1.0*ys;
  };

  for (int i=0;i<n;i++){
    add_row((double)raw_x[i], (double)raw_y[i],
            (double)scr_x[i], (double)scr_y[i]);
  }

  // ガウス消去で A X = B を解く（Xは[a b c d e f]）
  // シンプルな実装（6x6固定）
  const int N = 6;
  double M[N][N+1]; // 拡大係数行列 [A|B]
  for (int r=0;r<N;r++){
    for (int c=0;c<N;c++) M[r][c] = A[r][c];
    M[r][N] = B[r];
  }

  for (int col=0; col<N; col++) {
    // pivot
    int piv = col;
    for (int r=col+1; r<N; r++) {
      if (fabs(M[r][col]) > fabs(M[piv][col])) piv = r;
    }
    if (fabs(M[piv][col]) < 1e-9) return false;
    if (piv != col) {
      for (int c=col; c<=N; c++) std::swap(M[piv][c], M[col][c]);
    }
    // normalize
    double div = M[col][col];
    for (int c=col; c<=N; c++) M[col][c] /= div;
    // eliminate
    for (int r=0; r<N; r++){
      if (r==col) continue;
      double f = M[r][col];
      for (int c=col; c<=N; c++) M[r][c] -= f*M[col][c];
    }
  }

  out.a = M[0][N];
  out.b = M[1][N];
  out.c = M[2][N];
  out.d = M[3][N];
  out.e = M[4][N];
  out.f = M[5][N];
  out.valid = true;
  return true;
}

/************** 3) NVS 保存・読込 **************/
static bool save_touch_calibration(const TouchCal &cal){
  nvs_handle h;
  if (nvs_open("storage", NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_blob(h, "touch_cal", &cal, sizeof(cal));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

static bool load_touch_calibration(TouchCal &cal){
  nvs_handle h;
  if (nvs_open("storage", NVS_READONLY, &h) != ESP_OK) return false;
  size_t sz = sizeof(cal);
  esp_err_t err = nvs_get_blob(h, "touch_cal", &cal, &sz);
  nvs_close(h);
  if (err == ESP_OK && sz == sizeof(cal) && cal.valid) return true;
  cal.valid = false;
  return false;
}

/************** 4) 校正UI（5点タップ） **************/
static bool run_touch_calibration_ui() {
  // ターゲット（画面上の既知座標）
  const int TX = 20, TY = 20;
  const int P = 20;
  const int scr_x[5] = { TX,              (int)screenWidth-1-TX, (int)screenWidth-1-TX, TX,              (int)screenWidth/2 };
  const int scr_y[5] = { TY,              TY,                    (int)screenHeight-1-TY,(int)screenHeight-1-TY,(int)screenHeight/2 };

  int raw_x[5], raw_y[5];

  lv_obj_t* label = lv_label_create(lv_scr_act());
  lv_label_set_text(label, "Touch calibration: Tap the dots in order");
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 8);

  for (int i=0;i<5;i++){
    lv_obj_t* cross = draw_cross(scr_x[i], scr_y[i]);
    lv_obj_t* dot = lv_obj_create(lv_scr_act());
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x000000), 0);
    lv_obj_align(dot, LV_ALIGN_TOP_LEFT, scr_x[i]-4, scr_y[i]-4);

    // 安定タップを待つ
    int rx=0, ry=0, stable=0;
    uint32_t t0 = millis();
    while (millis() - t0 < 60000) {
      lv_task_handler();
      if (touchscreen.touched()) {
        int tr_x, tr_y;
        if (read_raw_touch(tr_x, tr_y, 5)) {
          rx = tr_x; ry = tr_y; stable++;
          if (stable > 4) break;
        }
      }
      delay(10);
    }
    lv_obj_del(cross);
    lv_obj_del(dot);
    if (stable <= 4){
      lv_label_set_text(label, "Calibration aborted: timeout");
      return false;
    }
    raw_x[i] = rx; raw_y[i] = ry;

    // ★ここで追加（次ポイントへの誤反応を防ぐ）
    wait_for_release_and_debounce(80,200);
  }

  TouchCal cal;
  if (!solve_affine_least_squares(raw_x, raw_y, scr_x, scr_y, 5, cal)){
    lv_label_set_text(label, "Failed to calculate coefficients");
    return false;
  }
  save_touch_calibration(cal);
  g_cal = cal;

  lv_label_set_text(label, "Calibration complete! Saved to NVS.");
  return true;
}

/************** 5) 起動時のロードとフォールバック **************/
static void ensure_touch_calibrated() {
  if (!load_touch_calibration(g_cal)) {
    // まだ未校正ならUIを走らせる
    run_touch_calibration_ui();

    // 一定時間は必ず画面に残す
    uint32_t t0 = millis();
    while (millis() - t0 < 3000) {
      lv_timer_handler();  // v8なら lv_timer_handler()
      delay(10);           // WDTケア
    }
  }
}

/************** 6) 既存の my_touchpad_read を置換（補正適用版） **************/
static void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
  if (touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();
    int rx = p.x, ry = p.y;

    int x = 0, y = 0;
    if (g_cal.valid) {
      // アフィン変換で生値→画面座標
      float xf = g_cal.a * rx + g_cal.b * ry + g_cal.c;
      float yf = g_cal.d * rx + g_cal.e * ry + g_cal.f;
      x = (int)lrintf(xf);
      y = (int)lrintf(yf);
    } else {
      // 未校正フォールバック（従来のmap）※とりあえずの仮実装
      x = map(rx, 300, 3900, 0, screenWidth );
      y = map(ry, 200, 3800, 0, screenHeight);
    }

    // 画面内にクリップ
    if (x < 0) x = 0; if (x >= screenWidth ) x = screenWidth -1;
    if (y < 0) y = 0; if (y >= screenHeight) y = screenHeight-1;

    data->state   = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// 指を離すまで待つ → さらに無入力のデバウンス時間を設ける
static void wait_for_release_and_debounce(uint16_t release_ms, uint16_t debounce_ms) {
  // まず完全リリースを待つ
  uint32_t t0 = millis();
  while (touchscreen.touched()) {
    lv_task_handler();
    delay(10);
    t0 = millis(); // 押されている間はタイマ延長
  }
  // リリース後、一定時間は無入力を確認（同一点の連続判定を防ぐ）
  uint32_t t1 = millis();
  while (millis() - t1 < debounce_ms) {
    lv_task_handler();
    delay(10);
    if (touchscreen.touched()) { // 触られたらやり直し
      // 再び離れるまで待ち直し
      while (touchscreen.touched()) { lv_task_handler(); delay(10); }
      t1 = millis(); // デバウンスタイマをリセット
    }
  }
}

// FFat 
static bool openFlashCsvForAppend() {
  if (ffatLog) return true;

  bool newlyCreated = false;

  // 現在パスが存在するか
  bool existed = FFat.exists(flash_cur_path);

  // 既存がなければ新規作成→BINならヘッダを書く
  if (!existed) {
    fs::File f = FFat.open(flash_cur_path, FILE_WRITE);
    if (!f) {
      Serial.printf("[FFat] create failed: %s\n", flash_cur_path);
      return false;
    }

    if (g_log_mode == LOG_CSV) {
      char csv_header[128];
      buildCsvHeader(csv_header, sizeof(csv_header));

      size_t hdr_len = strlen(csv_header);
      size_t wn = f.write((const uint8_t*)csv_header, hdr_len);
      if (wn != hdr_len) {
        Serial.println("[CSV] header write failed at create");
        f.close();
        FFat.remove(flash_cur_path);
        return false;
      }
      Serial.println("[CSV] header written (create)");
    } else {
      if (!writeAvlcHeaderInto(f)) {
        Serial.println("[BIN] header write failed at create");
        f.close();
        FFat.remove(flash_cur_path);
        return false;
      }
      g_rec_idx.store(0);
      Serial.println("[BIN] header written (create)");
    }

    f.flush();
    f.close();
    newlyCreated = true;
  }

  // APPENDで開く
  ffatLog = FFat.open(flash_cur_path, FILE_APPEND);
  if (!ffatLog) {
    Serial.printf("[FFat] open(APPEND) failed: %s\n", flash_cur_path);
    return false;
  }

  // CSVならここで終わり
  if (g_log_mode == LOG_CSV) {
    if (ffatLog.size() == 0) {
      char csv_header[128];
      buildCsvHeader(csv_header, sizeof(csv_header));

      size_t hdr_len = strlen(csv_header);
      size_t wn = ffatLog.write((const uint8_t*)csv_header, hdr_len);
      if (wn != hdr_len) {
        Serial.println("[CSV] header write failed (append/empty)");
        ffatLog.close();
        return false;
      }
      ffatLog.flush();
      Serial.println("[CSV] header written (append/empty)");
    }

    Serial.printf("[FFat] open: %s (size=%u)\n", flash_cur_path, (unsigned)ffatLog.size());
    return true;
  }

  // BIN/AEAD：新規 or 既存の扱い
  if (newlyCreated) {
    // 新規はヘッダ済み
    Serial.printf("[FFat] open: %s (size=%u)\n", flash_cur_path, (unsigned)ffatLog.size());
    return true;
  }

  // 既存BINを開いた場合
  if (ffatLog.size() == 0) {
    // 既存なのに空：ヘッダを書き込む
    if (!writeAvlcHeaderInto(ffatLog)) {
      Serial.println("[BIN] header write failed (append/empty)");
      ffatLog.close();
      return false;
    }
    ffatLog.flush();
    Serial.println("[BIN] header written (append/empty)");
  } else {
    // ヘッダから s_file_id を復旧（未設定なら）
    if (s_file_id == 0) {
      if (!readAvlcHeaderAndLatch(ffatLog)) {
        Serial.println("[BIN] existing header invalid -> recreate");
        // 壊れている：削除→新規作成→ヘッダ→開き直し
        String bad = ffatLog.name();
        ffatLog.close();
        FFat.remove(bad);

        fs::File f = FFat.open(bad, FILE_WRITE);
        if (!f || !writeAvlcHeaderInto(f)) {
          if (f) f.close();
          return false;
        }
        f.flush();
        f.close();

        ffatLog = FFat.open(bad, FILE_APPEND);
        if (!ffatLog) return false;
      }
    }
  }

  Serial.printf("[FFat] open: %s (size=%u)\n", flash_cur_path, (unsigned)ffatLog.size());
  return true;
}

// ヘッダ書き込み専用の小関数（ensureではなく“必ず書く”）
static bool writeAvlcHeaderInto(fs::File &f) {
  AvlcFileHeader fh{};
  memcpy(fh.magic, AVLC_MAGIC, 4);
  fh.ver       = AVLC_FORMAT_VERSION;
  fh.flags     = (g_log_mode == LOG_BIN_AEAD) ? 0x01 : 0x00;
  fh.aead_algo = g_aead_algo;
  fh.key_id    = g_key_id;
  fh.log_mode  = (uint8_t)g_log_mode;

  strncpy(fh.fw_version, VERSION, sizeof(fh.fw_version) - 1);
  fh.fw_version[sizeof(fh.fw_version) - 1] = '\0';

  time_t now = 0;
  time(&now);
  fh.start_unix = (uint32_t)now;

  uint64_t rid = 0;
  esp_fill_random(&rid, sizeof(rid));
  fh.file_id = rid;

  fh.rec_counter0 = 0;

  uint32_t crc = 0xFFFFFFFF;
  crc = crc32_le(crc, (const uint8_t*)&fh, offsetof(AvlcFileHeader, header_crc32));
  fh.header_crc32 = crc;

  size_t n = f.write((const uint8_t*)&fh, sizeof(fh));

  if (n == sizeof(fh)) {
    s_file_id = fh.file_id;
    return true;
  } else {
    Serial.println("[BIN] header write failed");
    return false;
  }
}

// キューから取り出してFFatへ追記保存（CSVのまま）
void saveToFlashCsv() {

  if (!log_ready.load()) return;

  // PATCH D: 状態評価（ここで3MB/1MBの判断を集約）
  evaluate_space_and_state_for_write();

  // 1MB未満等で停止が必要なら書かない
  if (!can_write_now()) return;

  if (!openFlashCsvForAppend()) {
    g_dump_request = true;   // 環境回復狙い
    return;
  }

  char receivedData[ble_reply];
  int wrote_lines = 0;

  while (xQueueReceive(dataQueue, &receivedData, 0) == pdPASS) {
    const char* line = receivedData;
    size_t need = strlen(line);

    // 本文＋改行の“完全成功”確認
    size_t w1 = ffatLog.write((const uint8_t*)line, need);
    size_t w2 = ffatLog.write((const uint8_t*)"\n", 1);

    if (w1 != need || w2 != 1) {
      // 書き込み失敗：行をキュー先頭に戻してダンプ要求
      Serial.println("[FFat] write failed (full?) → dump request");
      xQueueSendToFront(dataQueue, receivedData, 0);
      ffatLog.flush();
      g_dump_request = true;
      break;
    } else {
      wrote_lines++;
      file_count++;
      
      // ★ 推定器を更新（line + '\n'）
      size_t bytes_written = need + 1;
      g_est_csv.update(bytes_written);
      if (g_est_debug && (wrote_lines % 100 == 0)) {
        Serial.printf("[EST][CSV] avg_rec=~%uB (n=%u)\n",
                      (unsigned)g_est_csv.avg(), (unsigned)g_est_csv.n);
      }

      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  if (wrote_lines > 0) {
    ffatLog.flush();
    Serial.printf("[FFat] wrote %d lines\n", wrote_lines);
  }

  // ローテーション閾値
  if (file_count >= SN_Limit) {
    rollPending = true;
  }

  lastSaveTime = millis();
}

// FFat→SD へ CSVダンプ（disconnectタイミング用）
// 旧API互換ラッパー：呼ばれたら新API（全ダンプ＋パージ）を実行
static bool dumpFlashCsvToSD(const char* /*sd_csv_name*/) {
  return dumpAllFlashCsvToSD_AndPurge();
}

static const size_t ROTATE_HEADROOM = 64 * 1024; // 128KB 目安
static bool flash_rotate_to_new_file() {

  // ★ ここでローテの“最低余裕”を確認（足りなければ即やめる）
  size_t freeb = (size_t)FFat.totalBytes() - (size_t)FFat.usedBytes();
  if (freeb < ROTATE_HEADROOM) {
    Serial.printf("[FFat] rotate: free=%u < headroom=%u -> skip\n",
                  (unsigned)freeb, (unsigned)ROTATE_HEADROOM);
    return false;
  }

  if (ffatLog) { ffatLog.flush(); ffatLog.close(); }
  s_file_id = 0;

  char newpath[64];
  if (rtc_sync && printLocalTime()) {
    char base[16];
    strncpy(base, filename, 8); 
    base[8] = '\0';
    snprintf(newpath, sizeof(newpath), "%s%s%s", FLASH_LOG_PREFIX, base, log_ext_for_mode());
  } else {
    snprintf(newpath, sizeof(newpath), "/log_boot_%lu%s",
             (unsigned long)(millis()/1000), log_ext_for_mode());
  }

  // まず“新パスで作成テスト”だけする（flash_cur_path はまだ触らない）
  fs::File f = FFat.open(newpath, FILE_WRITE);
  if (!f) {
    Serial.printf("[FFat] rotate: open failed: %s\n", newpath);
    return false;
  }

  if (g_log_mode == LOG_CSV) {
    char csv_header[128];
    buildCsvHeader(csv_header, sizeof(csv_header));
    size_t hdr_len = strlen(csv_header);
    size_t wn = f.write((const uint8_t*)csv_header, hdr_len);
    if (wn != hdr_len) {
      Serial.println("[CSV] header write failed at rotate");
      f.close();
      FFat.remove(newpath);
      return false;
    }
  } else {
    if (!writeAvlcHeaderInto(f)) {
      Serial.println("[BIN] header write failed at rotate");
      f.close();
      FFat.remove(newpath);
      return false;
    }
    f.flush();
  }
  f.close();

  // ここまで来て“初めて” flash_cur_path を切り替える
  strncpy(flash_cur_path, newpath, sizeof(flash_cur_path));
  flash_cur_path[sizeof(flash_cur_path)-1] = '\0';

  Serial.printf("[FFat] rotated & header set: %s\n", flash_cur_path);
  return true;
}

static bool dumpAllFlashCsvToSD_AndPurge() {
  // FAT 日時属性を付ける（SdFat）
  FsDateTime::setCallback(dateTime);

  // ★ SDの再挑戦レート制御（このあたりは元のまま）
  uint32_t now = millis();
  if (g_sd_absent.load() && (now - g_sd_last_try_ms < SD_RETRY_MS)) {
    Serial.println("[DumpAll] skip SD.begin (cooldown)");
    g_sd_status_known.store(true);
    return false;
  }
  g_sd_last_try_ms = now;

  if (!SD.begin(SD_CONFIG)) {
    Serial.println("[DumpAll] SD init failed → defer (no purge)");
    g_sd_absent.store(true);
    g_sd_status_known.store(true);
    return false;
  }
  g_sd_absent.store(false);
  g_sd_status_known.store(true);

  // ★ 現行アクティブファイルのパスをスナップショット
  char cur_active_path[64];
  strncpy(cur_active_path, flash_cur_path, sizeof(cur_active_path));
  cur_active_path[sizeof(cur_active_path)-1] = '\0';

  fs::File root = FFat.open("/");
  if (!root) {
    Serial.println("[DumpAll] open root failed");
    return false;
  }

  int moved = 0, failed = 0;
  const char* cur_ext = log_ext_for_mode();   // 現行モードの拡張子(.csv / .bin)

  const uint32_t SD_SAFETY_MARGIN = 4096; // 4KBくらい余裕をみておく

  while (true) {
    fs::File f = root.openNextFile();
    if (!f) break;
    if (f.isDirectory()) { f.close(); continue; }

    String p = f.path();
    if (!p.startsWith(FLASH_LOG_PREFIX)) { f.close(); continue; }

    bool is_csv  = p.endsWith(".csv");
    bool is_bin  = p.endsWith(".bin");
    bool is_boot = p.endsWith("_boot.csv") || p.endsWith("_boot.bin");
    if (!(is_csv || is_bin || is_boot)) { f.close(); continue; }

    bool is_current_active = (strcmp(p.c_str(), cur_active_path) == 0);
    size_t sz = f.size();

    // CSV: サイズ 0 を“空”扱い
    // BIN/AEAD: ヘッダだけ（<= ヘッダサイズ）を“空”扱い
    bool effectively_empty =
        (is_csv && sz == 0) ||
        (is_bin && sz <= AVLC_HEADER_BYTES);

    if (is_current_active && effectively_empty) {
      Serial.printf("[DumpAll] skip current empty file: %s (size=%u)\n",
                    p.c_str(), (unsigned)sz);
      f.close();
      continue;
    }

    // 非現行＆実質空ならゴミ掃除だけして終了
    if (!is_current_active && effectively_empty) {
      Serial.printf("[DumpAll] purge empty stub: %s (size=%u)\n",
                    p.c_str(), (unsigned)sz);
      f.close();
      FFat.remove(p.c_str());
      continue;
    }

    // SD側の出力ファイル名（ベース名をそのまま・拡張子は現行モードに統一）
    const char* base = strrchr(p.c_str(), '/');
    base = base ? base + 1 : p.c_str();

    if (!SD.exists(SD_LOG_DIR)) {
      if (!SD.mkdir(SD_LOG_DIR)) {
        Serial.printf("[DumpAll] failed to create directory: %s\n", SD_LOG_DIR);
        f.close();
        failed++;
        continue;
      }
    }

    char base_name[64];
    snprintf(base_name, sizeof(base_name), "%s/%.*s%s",
             SD_LOG_DIR,
             (int)(strlen(base) - (is_csv || is_bin ? 4 : 0)),
             base, cur_ext);

    File32 out;
    if (!out.open(base_name, O_WRONLY | O_CREAT | O_TRUNC)) {
      Serial.printf("[DumpAll] open SD failed: %s\n", base_name);
      f.close();
      failed++;
      continue;
    }

    const size_t BUFSZ = 4096;
    uint8_t* buf = (uint8_t*) malloc(BUFSZ);
    if (!buf) {
      Serial.println("[DumpAll] malloc failed");
      out.close();
      f.close();
      failed++;
      continue;
    }

    // ---------- ここから「完全コピー確認」 ----------
    size_t totalWritten = 0;
    bool   copy_ok = true;

    while (true) {
      int n = f.read(buf, BUFSZ);
      if (n <= 0) break;

      int w = out.write(buf, n);
      if (w != n) {
        Serial.printf("[DumpAll] write failed mid-file: %s n=%d w=%d\n",p.c_str(), n, w);
        if (out.getWriteError()) {
            Serial.printf("[DumpAll] writeError=%d\n", out.getWriteError());
        }
        // 可能なら SD.sdErrorCode() / sdErrorData() も表示
        copy_ok = false;
        break;
      }
      totalWritten += w;
      vTaskDelay(1);
    }

    free(buf);
    out.close();
    f.close();

    if (!copy_ok || totalWritten != sz) {
      Serial.printf("[DumpAll] copy incomplete: %s src=%u written=%u\n",
                    p.c_str(), (unsigned)sz, (unsigned)totalWritten);

      // ★ここを追加：中途半端なSD側のファイルを削除しておく
      if (!SD.remove(base_name)) {
        Serial.printf("[DumpAll] remove incomplete SD file failed: %s\n", base_name);
      }

      failed++;
      continue;
    }


    // ---------- ここまで来て初めて FFat を削除 ----------
    if (FFat.remove(p.c_str())) {
      moved++;
      Serial.printf("[DumpAll] moved & purged: %s -> %s\n",
                    p.c_str(), base_name);
    } else {
      Serial.printf("[DumpAll] remove failed: %s\n", p.c_str());
      failed++;
    }

    vTaskDelay(1);
  }

  root.close();
  Serial.printf("[DumpAll] done. moved=%d failed=%d\n", moved, failed);
  ffat_debug_list();
  return (moved > 0 && failed == 0);
}

static void ffat_debug_list() {
  fs::File root = FFat.open("/");
  if (!root) { Serial.println("[FFat] open root failed"); return; }
  size_t sum=0; int n=0;
  while (true) {
    fs::File f = root.openNextFile();
    if (!f) break;
    if (f.isDirectory()) { f.close(); continue; }
    String p = f.path();
    size_t sz = f.size();
    Serial.printf("[FFat] file: %s size=%u\n", p.c_str(), (unsigned)sz);
    sum += sz; n++; f.close();
  }
  root.close();
  Serial.printf("[FFat] files=%d, sum=%u, FS used=%u\n",
                n, (unsigned)sum, (unsigned)FFat.usedBytes());
}
static void ensure_log_path_ready_for_write() {
  // 途中改名はしない方針。RTC未同期なら BOOT を指しているか整えるだけ。
  if (!rtc_sync) {
    const char* boot = boot_path_for_mode();
    if (strcmp(flash_cur_path, boot) != 0) {
      if (ffatLog) { ffatLog.flush(); ffatLog.close(); }
      strncpy(flash_cur_path, boot, sizeof(flash_cur_path));
      flash_cur_path[sizeof(flash_cur_path)-1] = '\0';
    }
    g_time_named = false;
  }
  // rtc_sync==true でも改名操作はしない
  return;
}

// 追加：共通のディスパッチ
static inline void saveToFlash() {
  if (g_log_mode == LOG_CSV) {
    saveToFlashCsv();
  } else {
    saveToFlashBin();   // ← 新規実装（下にパッチあり）
  }
}

void saveToFlashBin() {
  if (!log_ready.load()) return;

  // PATCH E: 状態評価
  evaluate_space_and_state_for_write();
  if (!can_write_now()) return;

  if (!openFlashCsvForAppend()) {
    g_dump_request = true;
    return;
  }

  // --- ここからが肝：AEAD用の file_id を必ず用意する ---
  if (g_log_mode != LOG_CSV && s_file_id == 0) {
    // まず、既存ヘッダの書き込み保証（サイズ0なら新規作成）
    if (!ensureBinFileHeader()) {
      String bad = ffatLog.name();
      ffatLog.close();
      FFat.remove(bad);
      fs::File f = FFat.open(bad, FILE_WRITE);
      if (!f || !writeAvlcHeaderInto(f)) {
        if (f) f.close();
        g_dump_request = true;
        return;
      }
      f.flush();
      f.close();
      ffatLog = FFat.open(bad, FILE_APPEND);
      if (!ffatLog) {
        g_dump_request = true;
        return;
      }
    }

    // それでも s_file_id が0（ヘッダはあるが未ラッチ）の場合は読み出してラッチ
    if (s_file_id == 0) {
      if (!readAvlcHeaderAndLatch(ffatLog)) {
        Serial.println("[AEAD] failed to latch file_id from header (will retry later)");
        // ダンプせずに次回に回す（キューは戻すため、この時点では何も書かない）
        return;
      }
    }
  }

  char line[ble_reply];
  int wrote_lines = 0;

  while (xQueueReceive(dataQueue, &line, 0) == pdPASS) {
    const uint8_t* payload = (const uint8_t*)line;
    const size_t   plen    = strlen(line);

    AvlcRecordHeader rh{};
    rh.type  = 1;
    rh.ts_ms = millis();
    rh.len   = (uint16_t)plen;
    rh.idx   = g_rec_idx.fetch_add(1);

    if (g_log_mode == LOG_BIN_AEAD) {
      // s_file_id が万一0なら無理をしない：行を戻して抜ける（Dumpはしない）
      if (s_file_id == 0) {
        xQueueSendToFront(dataQueue, line, 0);
        break;
      }
      uint8_t  cbuf[ble_reply + 32];
      size_t   clen = sizeof(cbuf);
      if (!aead_encrypt(rh, payload, plen, cbuf, clen)) {
        // 暗号化失敗は環境一過性の可能性：行を戻して次回に回す（Dumpはしない）
        xQueueSendToFront(dataQueue, line, 0);
        ffatLog.flush();
        break;
      }
        //暗号化BIN
        size_t n1 = ffatLog.write((const uint8_t*)&rh, sizeof(rh));
        size_t n2 = ffatLog.write(cbuf, clen);

        if (n1 != sizeof(rh) || n2 != clen) {
          // 書込失敗 → キューに戻してCSVと同じ挙動で停止&ダンプ
          xQueueSendToFront(dataQueue, line, 0);
          ffatLog.flush();
          g_dump_request = true;
          break;
        }

        size_t bytes_written = sizeof(rh) + clen;
        g_est_aead.update(bytes_written);

    } else {
      // 平文BIN
      size_t n3 = ffatLog.write((const uint8_t*)&rh, sizeof(rh));
      size_t n4 = ffatLog.write(payload, plen);

      if (n3 != sizeof(rh) || n4 != plen) {
        xQueueSendToFront(dataQueue, line, 0);
        ffatLog.flush();
        g_dump_request = true;
        break;
      }

      size_t bytes_written = sizeof(rh) + plen;
      g_est_bin.update(bytes_written);

    }

    wrote_lines++;
    file_count++;
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  if (wrote_lines > 0) {
    ffatLog.flush();
    Serial.printf("[BIN] wrote %d recs\n", wrote_lines);
  }

  if (file_count >= SN_Limit) rollPending = true;
  lastSaveTime = millis();
}

static bool ensureBinFileHeader() {
  if (ffatLog && ffatLog.size() > 0) {
    Serial.println("Skip AVLC Header");
    return true;
  }
  Serial.println("Write AVLC Header (new file)");
  return writeAvlcHeaderInto(ffatLog);  // ← file_id 生成 & s_file_id 保存もここで実施
}

// ちいさめのCRC32実装が無ければESP-IDF/ROMのものを使うか簡易版を持つ
static uint32_t crc32_le(uint32_t crc, const uint8_t *buf, size_t len) {
  // 簡易ラッパ（ESP-IDFのcrc32_leが使えるならそれを）
  while (len--) {
    crc ^= *buf++;
    for (int k=0; k<8; k++) crc = (crc>>1) ^ (0xEDB88320 & -(crc & 1));
  }
  return crc ^ 0xFFFFFFFF;
}

// ---- 置き換え: グローバルの nvs_handle_t my_nvs_handle; は削除 ----

// 追加: 共通ユーティリティ（都度open/close）
static bool nvs_write_last_addr(const char* addr) {
    nvs_handle h;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        Serial.printf("[NVS] open(write) failed: %d\n", err);
        return false;
    }
    err = nvs_set_str(h, "last_addr", addr);
    if (err != ESP_OK) {
        Serial.printf("[NVS] set_str failed: %d\n", err);
        nvs_close(h);
        return false;
    }
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        Serial.printf("[NVS] commit failed: %d\n", err);
        return false;
    }
    Serial.printf("[NVS] wrote last_addr=%s\n", addr);
    return true;
}

static bool nvs_read_last_addr(String &out) {
    nvs_handle h;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &h);
    if (err != ESP_OK) {
        Serial.printf("[NVS] open(read) failed: %d\n", err);
        return false;
    }
    size_t sz = 0;
    err = nvs_get_str(h, "last_addr", nullptr, &sz);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        Serial.println("[NVS] last_addr not found.");
        nvs_close(h);
        return false;
    }
    if (err != ESP_OK) {
        Serial.printf("[NVS] get_str(size) failed: %d\n", err);
        nvs_close(h);
        return false;
    }
    std::unique_ptr<char[]> buf(new char[sz]);
    err = nvs_get_str(h, "last_addr", buf.get(), &sz);
    nvs_close(h);
    if (err != ESP_OK) {
        Serial.printf("[NVS] get_str(value) failed: %d\n", err);
        return false;
    }
    out = String(buf.get());
    Serial.printf("[NVS] read last_addr=%s\n", buf.get());
    return true;
}

static bool nvs_erase_last_addr() {
    nvs_handle h;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        Serial.printf("[NVS] open(erase) failed: %d\n", err);
        return false;
    }
    err = nvs_erase_key(h, "last_addr");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        Serial.println("[NVS] erase: key not found (no-op).");
        nvs_close(h);
        return true; // これはエラー扱いにしない
    }
    if (err != ESP_OK) {
        Serial.printf("[NVS] erase_key failed: %d\n", err);
        nvs_close(h);
        return false;
    }
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        Serial.printf("[NVS] commit after erase failed: %d\n", err);
        return false;
    }
    Serial.println("[NVS] last_addr erased.");
    return true;
}

// aead_encrypt の差し替え実装
static bool aead_encrypt(const AvlcRecordHeader& rh,
                         const uint8_t* pt, size_t pt_len,
                         uint8_t* out, size_t& out_len)
{
  uint8_t nonce[12];
  if (s_file_id == 0) { Serial.println("[AEAD] file_id not set"); return false; }
  memcpy(nonce, &s_file_id, 8);
  memcpy(nonce+8, &rh.idx, 4);

  const unsigned char* aad = reinterpret_cast<const unsigned char*>(&rh);
  const size_t aad_len = sizeof(rh);
  const size_t tag_len = 16;
  if (out_len < pt_len + tag_len) return false;

  int ret = 0;

#if defined(MBEDTLS_CHACHAPOLY_C)
  if (g_aead_algo == 1) {
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    ret = mbedtls_chachapoly_setkey(&ctx, g_aead_key);
    if (ret != 0) { mbedtls_chachapoly_free(&ctx); return false; }
    ret = mbedtls_chachapoly_starts(&ctx, nonce, MBEDTLS_CHACHAPOLY_ENCRYPT);
    if (ret != 0) { mbedtls_chachapoly_free(&ctx); return false; }
    ret = mbedtls_chachapoly_update_aad(&ctx, aad, aad_len);
    if (ret != 0) { mbedtls_chachapoly_free(&ctx); return false; }
    ret = mbedtls_chachapoly_update(&ctx, pt_len, pt, out);
    if (ret != 0) { mbedtls_chachapoly_free(&ctx); return false; }
    ret = mbedtls_chachapoly_finish(&ctx, out + pt_len);
    mbedtls_chachapoly_free(&ctx);
    if (ret != 0) return false;
    out_len = pt_len + tag_len;
    return true;
  }
#endif

  // ここに来たら GCM（既定2、または chachapoly 非対応時のフォールバック）
  {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, g_aead_key, 256);
    if (ret != 0) { mbedtls_gcm_free(&gcm); return false; }
    ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT,
                                    pt_len,
                                    nonce, sizeof(nonce),
                                    aad, aad_len,
                                    pt, out,
                                    tag_len, out + pt_len);
    mbedtls_gcm_free(&gcm);
    if (ret != 0) return false;
    out_len = pt_len + tag_len;
    return true;
  }
}

// 既存BINのヘッダを読み取り、s_file_id を復旧する（成功なら true）
static bool readAvlcHeaderAndLatch(fs::File &f) {
  if (!f) return false;
  // 先頭からヘッダ読み
  if (!f.seek(0)) return false;

  AvlcFileHeader fh{};
  size_t n = f.read((uint8_t*)&fh, sizeof(fh));
  if (n != sizeof(fh)) return false;
  if (memcmp(fh.magic, AVLC_MAGIC, 4) != 0) return false;

  // CRC検証（書き込み時と同じロジック）
  uint32_t crc = 0xFFFFFFFF;
  uint32_t calc = crc32_le(crc, (const uint8_t*)&fh, offsetof(AvlcFileHeader, header_crc32));
  if (calc != fh.header_crc32) return false;

  // OK：file_id をラッチ
  s_file_id = fh.file_id;
  return true;
}

static void statusbar_create()
{
    if (status_bar) return;

    lv_obj_t* parent = lv_layer_top();

    status_bar = lv_obj_create(parent);
    lv_obj_set_size(status_bar, 100, 20);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(status_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(status_bar, LV_ALIGN_TOP_RIGHT, -2, 2);

    // ----- レイアウト寸法 -----
    const int ICON_H        = 16;
    const int GAP           = 2;

    const int FF_W          = 20;  // F の横幅（少し短め）
    const int SD_W          = 22;  // SD の横幅（少し短め）
    const int BATT_W        = 25;  // 電池枠全体（ボディ＋頭を含む）
    const int BATT_TIP_W    = 2;   // 電池の頭
    const int BATT_BAR_W    = 4;   // 緑バー幅
    const int BATT_BAR_H    = 10;
    const int BATT_BAR_GAP  = 1;

    int x = 20;
    const int y = 2;

    // ==== FFat アイコン ====
    icon_ffat = lv_obj_create(status_bar);
    lv_obj_set_size(icon_ffat, FF_W, ICON_H);
    lv_obj_set_style_radius(icon_ffat, 3, 0);
    lv_obj_set_style_border_width(icon_ffat, 1, 0);
    lv_obj_set_style_border_color(icon_ffat, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(icon_ffat, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(icon_ffat, 0, 0);
    lv_obj_set_pos(icon_ffat, x, y);
    // ★ これを追加（スクロール無効＆スクロールバーOFF）
    lv_obj_clear_flag(icon_ffat, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(icon_ffat, LV_SCROLLBAR_MODE_OFF);
    set_icon_state(icon_ffat, ICON_UNKNOWN);

    lv_obj_t* lbl_f = lv_label_create(icon_ffat);
    lv_label_set_text(lbl_f, "F");
    lv_obj_center(lbl_f);

    // ==== SD アイコン ====
    x += FF_W + GAP;

    icon_sd = lv_obj_create(status_bar);
    lv_obj_set_size(icon_sd, SD_W, ICON_H);
    lv_obj_set_style_radius(icon_sd, 3, 0);
    lv_obj_set_style_border_width(icon_sd, 1, 0);
    lv_obj_set_style_border_color(icon_sd, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(icon_sd, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(icon_sd, 0, 0);
    lv_obj_set_pos(icon_sd, x, y);
    // ★ これを追加（スクロール無効＆スクロールバーOFF）
    lv_obj_clear_flag(icon_sd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(icon_sd, LV_SCROLLBAR_MODE_OFF);
    set_icon_state(icon_sd, ICON_UNKNOWN);

    lv_obj_t* lbl_sd = lv_label_create(icon_sd);
    lv_label_set_text(lbl_sd, "SD");
    lv_obj_center(lbl_sd);

    // ==== バッテリアイコン ====
    x += SD_W + GAP;   // ★ SD のすぐ右に電池（隙間は GAP のみ）

    icon_batt = lv_obj_create(status_bar);
    lv_obj_set_size(icon_batt, BATT_W, ICON_H);
    lv_obj_set_style_bg_opa(icon_batt, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(icon_batt, 1, 0);
    lv_obj_set_style_border_color(icon_batt, lv_color_black(), 0);
    lv_obj_set_style_pad_all(icon_batt, 1, 0);
    lv_obj_set_style_radius(icon_batt, 2, 0);
    lv_obj_set_pos(icon_batt, x, y);
    lv_obj_clear_flag(icon_batt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(icon_batt, LV_SCROLLBAR_MODE_OFF);

    // ★ 電池の頭（＋端子）は icon_batt の「中」に作る → 切れない
    lv_obj_t* batt_tip = lv_obj_create(status_bar);
    lv_obj_set_size(batt_tip, BATT_TIP_W, 8);
    lv_obj_set_style_bg_opa(batt_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(batt_tip, lv_color_black(), 0);
    lv_obj_set_style_border_opa(batt_tip, LV_OPA_TRANSP, 0);
    // ボディ部分を (BATT_W - BATT_TIP_W) と見なして、その右側に頭
    lv_obj_set_pos(batt_tip, x+BATT_W ,y+ (ICON_H - 8) / 2 );

    // 中の 4 本のバー
    for (int i = 0; i < 4; i++) {
        batt_bar[i] = lv_obj_create(icon_batt);
        lv_obj_set_size(batt_bar[i], BATT_BAR_W, BATT_BAR_H);
        lv_obj_set_style_border_opa(batt_bar[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(batt_bar[i], LV_OPA_TRANSP, 0);  // 初期は非表示
        lv_obj_set_style_bg_color(batt_bar[i], lv_color_make(0x00, 0xC8, 0x00), 0);

        // ★ 前より X,Y 共に 1px 左上へ寄せる
        int bx = 1 + i * (BATT_BAR_W + BATT_BAR_GAP); // 左から 1px
        int by = 1;                                   // 上から 1px
        lv_obj_set_pos(batt_bar[i], bx, by);
    }
    
    // ★ 追加：充電中に重ねて表示する稲妻マーク
    batt_charge_icon = lv_label_create(icon_batt);
    lv_label_set_text(batt_charge_icon, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(batt_charge_icon, &lv_font_montserrat_12, 0);
    lv_obj_center(batt_charge_icon);
    lv_obj_add_flag(batt_charge_icon, LV_OBJ_FLAG_HIDDEN);
}

static void statusbar_update()
{
    if (!status_bar) return;

    // ---- バッテリ ----
    float vbat   = pwr_mgmt.voltage();   // 例: 4.11V
    uint8_t prec = pwr_mgmt.percent();   // 0〜100

    int level = prec / 25 + 1;      // 0〜4
    if (level > 4) level = 4;

    // レベルに応じた色
    lv_color_t col;
    if (prec < 15) {
        col = lv_color_make(0xE0, 0x00, 0x00); // 赤
    } else if (prec < 40) {
        col = lv_color_make(0xE0, 0xE0, 0x00); // 黄
    } else {
        col = lv_color_make(0x00, 0xC8, 0x00); // 緑
    }

    for (int i = 0; i < 4; i++) {
        if (!batt_bar[i]) continue;
        if (i < level) {
            lv_obj_set_style_bg_opa(batt_bar[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(batt_bar[i], col, 0);
        } else {
            lv_obj_set_style_bg_opa(batt_bar[i], LV_OPA_TRANSP, 0);
        }
    }
    
    // ---- ここから：充電中(USBあり)の稲妻マーク制御 ----
    if (batt_charge_icon) {
        if (is_charging()) {
            lv_obj_clear_flag(batt_charge_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(batt_charge_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // ========= ここから下を「点滅対応版」にする =========

    // ---- FFat のベース状態 ----
    IconState ff_state = ICON_UNKNOWN;
    switch (g_state.load()) {
        case SystemState::NORMAL:
            ff_state = ICON_OK;
            break;
        case SystemState::SOFT_MARGIN:
            ff_state = ICON_WARN;   // そろそろ一杯
            break;
        case SystemState::EMERGENCY:
        case SystemState::WAIT_SD:
        case SystemState::DUMPING:
        case SystemState::POST_DUMP_ROTATE:
            ff_state = ICON_NG;
            break;
        default:
            ff_state = ICON_UNKNOWN;
            break;
    }

    // ---- SD のベース状態 ----
    IconState sd_state = ICON_UNKNOWN;
    if (!g_sd_status_known.load()) {
        sd_state = ICON_UNKNOWN;
    } else if (g_sd_absent.load()) {
        sd_state = ICON_NG;         // 未挿入 or 初期化失敗
    } else {
        sd_state = ICON_OK;
    }

    // ---- Dump中は FF/SD を点滅させる ----
    bool dump_active =
        g_dump_in_progress.load() ||
        (g_state.load() == SystemState::DUMPING);

    bool blink_on = true;
    if (dump_active) {
        // 300ms ごとに ON/OFF 切り替え
        uint32_t t = millis();
        blink_on = ((t / 300) % 2) == 0;
    }

    // 点滅OFF中はグレー表示（ICON_UNKNOWN）に落とす
    set_icon_state(icon_ffat, blink_on ? ff_state : ICON_UNKNOWN);
    set_icon_state(icon_sd,   blink_on ? sd_state : ICON_UNKNOWN);
}

static bool is_charging()
{
    static bool last = false;   // true = 充電中

    int raw = analogRead(Sensor_VN);
    float v_div = (float)raw / 4095.0f * 3.1f;

    float v_chg = v_div;   // 回路に応じて調整

    const float TH_ON  = 0.8f;  // これ未満 → 充電中
    const float TH_OFF = 1.5f;  // これ以上 → 非充電

    bool prev = last;      // ★ ここで必ず保存する

    if (last) {
        if (v_chg > TH_OFF) last = false;
    } else {
        if (v_chg < TH_ON) last = true;
    }

    return last;
}
static void buildCsvHeader(char* buf, size_t bufsize) {
  snprintf(buf, bufsize,
           "#magic=%s\n"
           "#format_version=%u\n"
           "#fw_version=%s\n"
           "#log_mode=%s\n",
           AVLC_MAGIC,
           (unsigned)AVLC_FORMAT_VERSION,
           VERSION,
           log_mode_to_str(g_log_mode));
}
static const char* log_mode_to_str(LogMode mode) {
  switch (mode) {
    case LOG_CSV:      return "CSV";
    case LOG_BIN:      return "BIN";
    case LOG_BIN_AEAD: return "AEAD";
    default:           return "?";
  }
}

static void flush_queue_to_flash_for_poweroff()
{
    if (!log_ready.load()) return;
    if (!openFlashCsvForAppend()) return;

    // --- CSV ---
    if (g_log_mode == LOG_CSV) {
        char receivedData[ble_reply];

        while (xQueueReceive(dataQueue, &receivedData, 0) == pdPASS) {
            const char* line = receivedData;
            size_t len = strlen(line);

            if (ffatLog.write((const uint8_t*)line, len) != len ||
                ffatLog.write((const uint8_t*)"\n", 1) != 1) {
                Serial.println("[POW] CSV write failed");
                break;
            }
        }
        ffatLog.flush();
    }

    // --- BIN / AEAD ---
    else {
        char line[ble_reply];

        while (xQueueReceive(dataQueue, &line, 0) == pdPASS) {

            const uint8_t* payload = (const uint8_t*)line;
            size_t plen = strlen(line);

            AvlcRecordHeader rh{};
            rh.type  = 1;
            rh.ts_ms = millis();
            rh.len   = (uint16_t)plen;
            rh.idx   = g_rec_idx.fetch_add(1);

            if (g_log_mode == LOG_BIN_AEAD) {

                uint8_t cbuf[ble_reply + 32];
                size_t clen = sizeof(cbuf);

                if (!aead_encrypt(rh, payload, plen, cbuf, clen)) {
                    Serial.println("[POW] AEAD encrypt failed");
                    break;
                }

                if (ffatLog.write((uint8_t*)&rh, sizeof(rh)) != sizeof(rh) ||
                    ffatLog.write(cbuf, clen) != clen) {
                    Serial.println("[POW] BIN(AEAD) write failed");
                    break;
                }

            } else {
                // plain BIN
                if (ffatLog.write((uint8_t*)&rh, sizeof(rh)) != sizeof(rh) ||
                    ffatLog.write(payload, plen) != plen) {
                    Serial.println("[POW] BIN write failed");
                    break;
                }
            }
        }
        ffatLog.flush();
    }
}

static void perform_safe_poweroff()
{
    if (g_poweroff_in_progress.exchange(true)) return;

    Serial.println("[POW] start safe shutdown");

    // 1) 新規処理停止
    is_measuring = false;
    doConnect = false;
    GATT_command_receive = false;
    re_try = false;

    // 2) BLE切断
    if (pClient && pClient->isConnected()) {
        pClient->disconnect();
    }
    connected = false;

    // 3) Queueを強制書き込み
    flush_queue_to_flash_for_poweroff();

    // 4) ファイルクローズ
    if (ffatLog) {
        ffatLog.flush();
        vTaskDelay(pdMS_TO_TICKS(20));
        ffatLog.close();
        Serial.println("[POW] file closed");
    }

    // 5) 少し待つ（重要）
    vTaskDelay(pdMS_TO_TICKS(100));

    // 6) 電源OFF
    Serial.println("[POW] power OFF");
    pinMode(sw_off, OUTPUT);
    digitalWrite(sw_off, LOW);
}
static void show_measuring_block_dialog() {
    static const char * btns[] = { "OK", "" };

    lv_obj_t* mbox = lv_msgbox_create(
        NULL,
        "電源OFF不可",
        "AVL動作中は電源を切れません。",
        btns,
        true
    );

    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, [](lv_event_t * e){
        lv_obj_t * m = lv_event_get_current_target(e);
        if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED ||
            lv_event_get_code(e) == LV_EVENT_DELETE) {
            lv_msgbox_close(m);
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_add_event_cb(mbox, [](lv_event_t * e){
        lv_obj_t * m = lv_event_get_current_target(e);
        if (lv_event_get_code(e) == LV_EVENT_DELETE) {
            // 何もしない
        }
    }, LV_EVENT_DELETE, NULL);
}
static inline bool avl_busy()
{
    return GATT_command_receive || re_try || is_measuring;
}