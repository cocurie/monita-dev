/**
 * 豊能町公園人流測定 — Monita Flex v3.04
 * Phase-1: BLE スキャン + SD カード記録
 *
 * ハード: Monita Flex v3.04 + Seeed XIAO nRF52840 Sense
 *
 * ─────────────────────────────────────────────
 * 起動シーケンス
 *   i2cInit() [GPIO bit-bang, Wire/TWIM 不使用]
 *   → TCA9534 初期化 (P2=HIGH → 3V3_SW ON, P3=HIGH → SD CS デアサート)
 *   → SD カード初期化 (SdFat v2 / SPI)
 *   → BLE スキャン開始
 *
 * Wire (TWIM) 非使用の理由
 *   BLE SoftDevice が Wire.begin() より前に有効化されているため、
 *   TWIM の IRQ 優先度が SoftDevice の予約帯と競合しハングする。
 *   GPIO ビットバン I2C で TCA9534 のみ制御することで回避する。
 *
 * SD CS について (v3.04 基板)
 *   PCB では SD の CS は TCA9534 P3 が能動駆動 (I2C 経由)。
 *   SdFat は GPIO CS を前提とするため、以下の方式で回避する:
 *     1. P3=HIGH でプリクロック (80 clk) → SD の電源投入シーケンスを満たす
 *     2. P3=LOW に固定 (= CS を常にアサート)
 *     3. SdFat には D9 をダミー CS として渡す (PCB 上 SD とは無接続)
 *   SPI バス上に SD のみなので CS 常時アサートは実用上問題なし。
 *   SdFat が D9 を操作しても SD の CS には影響しない。
 *
 * SPI ピン (v3.04)
 *   D10 = SCK, D1 = MOSI, D2 = MISO
 *   TCA9534 P3 → SD CS (I2C 制御)
 *
 * CSV ログ (SD /log.csv)
 *   timestamp,people,devices
 * ─────────────────────────────────────────────
 *
 * シリアルコマンド (115200bps / LF)
 *   d        ログ全件出力
 *   e        ログ削除 (次回書込時にヘッダ付き新規作成)
 *   c<N>     キャリブレーション開始 (例: c5)
 *   r        キャリブレーション途中経過表示
 *   x        キャリブレーション終了
 *
 * DS3231 時刻設定方法 (どちらか一方を使う)
 *
 * 【方法 A: フラッシュ時に自動設定 (推奨)】
 *   1. 93行目の  #define SET_RTC_ON_BOOT  0  を  1  に変更
 *   2. ビルド & フラッシュ → 起動時にビルド時刻が DS3231 に書き込まれる
 *   3. SET_RTC_ON_BOOT を 0 に戻して再フラッシュ (毎回上書きを防ぐ)
 *   ※ フラッシュ完了〜起動まで数秒ずれる場合がある
 *
 * 【方法 B: シリアルコマンドで設定】
 *   t<YYYYMMDDHHMMSS> を送信 (例: t20260701143000 → 2026-07-01 14:30:00)
 *
 *   DS3231 は電源オフ後も時刻を保持する (CR2032 バックアップ電源が必要)
 *   DS3231 未接続時は millis() 起点の経過時間をタイムスタンプとして記録する
 *
 * フェーズ別有効化
 *   Phase-2: PIR (D8) → 本ファイル末尾の PIR ブロックを参照
 *   Phase-3: 音声 (PDM) → 本ファイル末尾の AUDIO ブロックを参照
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <SPI.h>
#include <SdFat.h>
#include <bluefruit.h>
#include <math.h>
#include <string.h>

// ═══════════════════════════════════════
// ▼ 設定パラメータ
// ═══════════════════════════════════════

// ── TCA9534 ──────────────────────────
#define TCA9534_ADDR  0x20

// ── DS3231 RTC ───────────────────────
#define DS3231_ADDR   0x68

// ── DS3231 初期時刻設定 ───────────────
// 1 にしてフラッシュするとビルド時刻を DS3231 に書き込む。
// 設定後は 0 に戻して再フラッシュすること（毎回上書きを防ぐため）。
#define SET_RTC_ON_BOOT  1

// ── SD カード SPI ピン (v3.04) ─────────
#define PIN_SD_MISO       D2   // P0.28
#define PIN_SD_SCK        D10  // P1.15
#define PIN_SD_MOSI       D1   // P0.03
#define PIN_SD_CS_DUMMY   D9   // SdFat CS管理用ダミー (Sigfox RX ピン / SD 未接続)
#define SPI_SPEED_MHZ     4    // 失敗時は 1 に落とす

// ── ログ ─────────────────────────────
static bool       const ENABLE_LOGGING = true;
static char const*      LOG_FILE       = "/log.csv";

// ── スキャン / スリープ ────────────────
static uint32_t const SCAN_DURATION_MS  = 30000;  // スキャン時間 (ms)
static uint32_t const SLEEP_DURATION_MS = 90000;  // スリープ時間 (ms)

// ── BLE クラスタリングパラメータ ─────────
static int   const MIN_HITS       = 12;   // 有効デバイスのヒット最小回数
static int   const RSSI_THRESHOLD = -65;  // 採用 RSSI 下限 (dBm)
static int   const RSSI_MERGE_GAP = 3;   // 同一人物とみなす RSSI 差 (dBm)
static float const CALIBRATION    = 1.0f; // 人数補正係数

// ── BLE スキャン設定 ──────────────────
static uint16_t const SCAN_INTERVAL_MS = 150;
static uint16_t const SCAN_WINDOW_MS   = 100;

// ── LED ──────────────────────────────
static uint32_t const LED_BLINK_MS = 1000;

// ── キャリブレーション ──────────────────
#define CALIB_MIN_SAMPLES 5

// ═══════════════════════════════════════
// ▼ グローバル
// ═══════════════════════════════════════

// SPI / SD
static SPIClass SD_SPI(NRF_SPIM1, PIN_SD_MISO, PIN_SD_SCK, PIN_SD_MOSI);
static SdFat    sd;

// BLE デバイス保持
#define MAX_DEVICES 64
struct Device {
  uint8_t mac[6];
  int     count;
  int     rssi_sum;
};
static Device devices[MAX_DEVICES];
static int    deviceCount = 0;

// TCA9534 出力レジスタキャッシュ
static uint8_t s_tca9534Out = 0x00;

// SD カード初期化済みフラグ
static bool s_sdReady = false;

// キャリブレーション状態
static bool  calibMode       = false;
static int   calibActual     = 0;
static int   calibSamples    = 0;
static float calibRatioSum   = 0.0f;
static int   calibMinHitsSum = 0;

// シリアルコマンドバッファ
static char cmdBuf[16];  // 最大 t<YYYYMMDDHHMMSS> (15 chars) + null
static int  cmdLen = 0;

// 時刻 (tコマンドで設定; RTC なし・millis() オフセット方式)
static uint32_t s_rtcBase  = 0;    // 2000-01-01 00:00:00 = 0 の秒値
static uint32_t s_rtcSetMs = 0;    // 時刻設定時の millis()
static bool     s_rtcSet   = false;

// 前方宣言 (実装は eraseLog 以降の「時刻ユーティリティ」セクション)
static void getTimestamp(char *buf, size_t len);

// ═══════════════════════════════════════
// ▼ TCA9534 ソフト I2C (Wire/TWIM 不使用)
// ─ BLE SoftDevice との IRQ 競合を回避するため GPIO ビットバン実装 ─
// ═══════════════════════════════════════

#define I2C_SDA   D4   // P0.04
#define I2C_SCL   D5   // P0.05

// SDA はオープンドレイン (INPUT_PULLUP=解放 / OUTPUT+LOW=駆動)
// SCL はプッシュプル (TCA9534 はクロックストレッチなし)
static void sdaHi() { pinMode(I2C_SDA, INPUT_PULLUP); delayMicroseconds(5); }
static void sdaLo() { pinMode(I2C_SDA, OUTPUT); digitalWrite(I2C_SDA, LOW); delayMicroseconds(5); }
static void sclHi() { digitalWrite(I2C_SCL, HIGH); delayMicroseconds(5); }
static void sclLo() { digitalWrite(I2C_SCL, LOW);  delayMicroseconds(5); }

static void i2cInit() {
  pinMode(I2C_SCL, OUTPUT);
  sclHi(); sdaHi();
  delayMicroseconds(50);
}

static void i2cStart() { sdaHi(); sclHi(); sdaLo(); sclLo(); }
static void i2cStop()  { sdaLo(); sclHi(); sdaHi(); }

static bool i2cWriteByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    if ((b >> i) & 1) sdaHi(); else sdaLo();
    sclHi(); sclLo();
  }
  sdaHi();  // SDA 解放 → スレーブが ACK=LOW を返す
  sclHi();
  bool ack = (digitalRead(I2C_SDA) == LOW);
  sclLo();
  return ack;
}

static uint8_t i2cReadByte(bool sendAck) {
  uint8_t b = 0;
  sdaHi();
  for (int i = 7; i >= 0; i--) {
    sclHi();
    b = (uint8_t)((b << 1) | (digitalRead(I2C_SDA) ? 1 : 0));
    sclLo();
  }
  if (sendAck) sdaLo(); else sdaHi();
  sclHi(); sclLo(); sdaHi();
  return b;
}

static bool tca9534WriteReg(uint8_t reg, uint8_t val) {
  i2cStart();
  bool ok = i2cWriteByte((uint8_t)((TCA9534_ADDR << 1) | 0x00));
  ok &= i2cWriteByte(reg);
  ok &= i2cWriteByte(val);
  i2cStop();
  return ok;
}

/**
 * v3.04 用 TCA9534 初期化
 *   P2 = MOSFET_GATE (3V3_SW)   → OUTPUT HIGH (周辺電源 ON)
 *   P3 = SPI CS (SD カード)     → OUTPUT LOW  (CS 常時アサート)
 *   P0/P1/P4〜P7                 → INPUT
 *
 * CS を常時アサート（LOW固定）にする理由:
 *   sdCsWrite 経由の I2C トグルは1回 ~250µs かかり SdFat の
 *   CS タイミング要件を満たせない場合がある。SPI バス上に SD
 *   のみ存在するため CS 常時アサートで実用上問題なし。
 */
static bool tca9534Init() {
  // 極性レジスタ: 正論理
  if (!tca9534WriteReg(0x02, 0x00)) return false;
  // 方向レジスタ (0=OUT, 1=IN): P2/P3=OUT, 他=IN → 0b11110011 = 0xF3
  if (!tca9534WriteReg(0x03, 0xF3)) return false;
  // 出力初期値: P2=1 (3V3_SW ON), P3=0 (CS 常時アサート) → 0b00000100 = 0x04
  s_tca9534Out = 0x04;
  return tca9534WriteReg(0x01, s_tca9534Out);
}

/** ビット単位でポート出力を変更しキャッシュを更新する */
static bool tca9534SetBit(uint8_t bit, uint8_t val) {
  if (val) s_tca9534Out |=  (uint8_t)(1u << bit);
  else     s_tca9534Out &= ~(uint8_t)(1u << bit);
  return tca9534WriteReg(0x01, s_tca9534Out);
}

// ═══════════════════════════════════════
// ▼ DS3231 RTC (ビットバン I2C 共用)
// ═══════════════════════════════════════

static uint8_t bcdToDec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static uint8_t decToBcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

static bool ds3231Read(uint16_t &Y, uint8_t &Mo, uint8_t &D,
                        uint8_t &h, uint8_t &mi, uint8_t &s) {
  i2cStart();
  if (!i2cWriteByte((DS3231_ADDR << 1) | 0x00)) { i2cStop(); return false; }
  if (!i2cWriteByte(0x00))                       { i2cStop(); return false; }
  i2cStop();
  i2cStart();
  if (!i2cWriteByte((DS3231_ADDR << 1) | 0x01)) { i2cStop(); return false; }
  s   = bcdToDec(i2cReadByte(true)  & 0x7F);
  mi  = bcdToDec(i2cReadByte(true)  & 0x7F);
  h   = bcdToDec(i2cReadByte(true)  & 0x3F);
  (void)i2cReadByte(true);                       // 曜日スキップ
  D   = bcdToDec(i2cReadByte(true)  & 0x3F);
  Mo  = bcdToDec(i2cReadByte(true)  & 0x1F);
  Y   = (uint16_t)(2000 + bcdToDec(i2cReadByte(false)));
  i2cStop();
  return (Mo >= 1 && Mo <= 12 && D >= 1 && D <= 31 && h <= 23 && mi <= 59 && s <= 59);
}

static bool ds3231Write(uint16_t Y, uint8_t Mo, uint8_t D,
                         uint8_t h, uint8_t mi, uint8_t s) {
  i2cStart();
  if (!i2cWriteByte((DS3231_ADDR << 1) | 0x00)) { i2cStop(); return false; }
  if (!i2cWriteByte(0x00))                       { i2cStop(); return false; }
  i2cWriteByte(decToBcd(s));
  i2cWriteByte(decToBcd(mi));
  i2cWriteByte(decToBcd(h));
  i2cWriteByte(0x01);
  i2cWriteByte(decToBcd(D));
  i2cWriteByte(decToBcd(Mo));
  i2cWriteByte(decToBcd((uint8_t)(Y - 2000)));
  i2cStop();
  return true;
}

// ビルド時刻 (__DATE__ / __TIME__) を DS3231 に書き込む
// SET_RTC_ON_BOOT=1 のときのみ呼ばれる
static uint8_t parseMonth(const char *s) {
  const char *m = "JanFebMarAprMayJunJulAugSepOctNovDec";
  for (uint8_t i = 0; i < 12; i++)
    if (strncmp(s, m + i * 3, 3) == 0) return i + 1;
  return 1;
}

static void setRtcFromCompileTime() {
  // __DATE__ = "Jul  1 2026"  __TIME__ = "14:30:00"
  uint8_t  mo = parseMonth(__DATE__);
  uint8_t  d  = (uint8_t)atoi(__DATE__ + 4);
  uint16_t y  = (uint16_t)atoi(__DATE__ + 7);
  uint8_t  h  = (uint8_t)atoi(__TIME__);
  uint8_t  mi = (uint8_t)atoi(__TIME__ + 3);
  uint8_t  s  = (uint8_t)atoi(__TIME__ + 6);
  Serial.printf("[DS3231] SET_RTC_ON_BOOT: %04u-%02u-%02u %02u:%02u:%02u\n", y, mo, d, h, mi, s);
  Serial.println(ds3231Write(y, mo, d, h, mi, s) ? "[DS3231] write OK" : "[DS3231] write FAILED");
}

// ── SdFat CS オーバーライド ─────────────────
// CS は tca9534Init() で P3=LOW に固定済み。
// SdFat の CS 操作（sdCsWrite）は no-op とし、D9 ダミーピンへの操作を無視する。
void sdCsInit(SdCsPin_t pin)            { (void)pin; }
void sdCsWrite(SdCsPin_t pin, bool lvl) { (void)pin; (void)lvl; }

// ═══════════════════════════════════════
// ▼ SD カード ユーティリティ
// ═══════════════════════════════════════

/**
 * SD カード初期化
 *
 * sdCsWrite() オーバーライドにより SdFat の CS 操作が TCA9534 P3 に直結する。
 * SdFat が内部で行う「CS=HIGH → 80クロック → CS=LOW → CMD0」シーケンスが
 * そのまま TCA9534 P3 のアサート/デアサートとして機能する。
 */
static bool initSd() {
  delay(2000);  // 3V3_SW 安定待ち (500ms では不安定なため 2000ms に延長)

  // ── SD カード SPI モード移行シーケンス ──────────────────────
  // SD 仕様: 電源投入後、CS=HIGH のまま 74 クロック以上送ること。
  // 本基板は TCA9534 P3 が CS を制御するため、SdFat の CS 操作は
  // ダミーピン(D9)に逃がしており実 CS は常時 LOW のまま。
  // そのため、ここで手動で CS=HIGH → 80 クロック → CS=LOW を実施する。
  tca9534SetBit(3, 1);  // P3=HIGH (CS 一時デアサート)
  delay(5);
  SD_SPI.begin();
  SD_SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 10; i++) SD_SPI.transfer(0xFF);  // 80 クロック
  SD_SPI.endTransaction();
  tca9534SetBit(3, 0);  // P3=LOW (CS アサート・以降固定)
  delay(5);
  // ────────────────────────────────────────────────────────────

  pinMode(PIN_SD_CS_DUMMY, OUTPUT);
  digitalWrite(PIN_SD_CS_DUMMY, HIGH);

  SdSpiConfig cfg(PIN_SD_CS_DUMMY, DEDICATED_SPI, SD_SCK_MHZ(SPI_SPEED_MHZ), &SD_SPI);
  if (!sd.begin(cfg)) {
    Serial.print("[SD] init failed  err=0x");
    Serial.print(sd.card()->errorCode(), HEX);
    Serial.print("/0x");
    Serial.println(sd.card()->errorData(), HEX);
    return false;
  }
  Serial.print("[SD] init OK  capacity=");
  Serial.print((uint32_t)(0.000512f * sd.card()->sectorCount()));
  Serial.println("MB");

  // ログファイル: 存在しなければヘッダ行を作成
  if (ENABLE_LOGGING && !sd.exists(LOG_FILE)) {
    FsFile f = sd.open(LOG_FILE, O_WRITE | O_CREAT);
    if (f) {
      f.println("datetime,people,devices");
      f.close();
      Serial.println("[SD] /log.csv created");
    }
  }
  return true;
}

/** 1 サイクル分のデータを /log.csv に追記する */
static void logRecord(int people, int devCount) {
  if (!ENABLE_LOGGING || !s_sdReady) return;

  FsFile f = sd.open(LOG_FILE, O_WRITE | O_APPEND);
  if (!f) { Serial.println("[SD] open failed"); return; }

  char ts[24];
  getTimestamp(ts, sizeof(ts));

  f.print(ts); f.print(","); f.print(people); f.print(","); f.println(devCount);
  f.close();
}

/** SD ログを全件シリアル出力 */
static void dumpLog() {
  if (!ENABLE_LOGGING) { Serial.println("[LOG] disabled"); return; }
  FsFile f = sd.open(LOG_FILE, O_READ);
  if (!f) { Serial.println("[LOG] no file"); return; }
  Serial.println("=== LOG DUMP ===");
  while (f.available()) Serial.write(f.read());
  f.close();
  Serial.println("=== END ===");
}

/** SD ログを削除 */
static void eraseLog() {
  if (!ENABLE_LOGGING) { Serial.println("[LOG] disabled"); return; }
  if (sd.exists(LOG_FILE)) {
    sd.remove(LOG_FILE);
    Serial.println("[LOG] erased");
  } else {
    Serial.println("[LOG] no file");
  }
}

// ═══════════════════════════════════════
// ▼ 時刻ユーティリティ
// ═══════════════════════════════════════

static bool isLeapYear(uint16_t y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}
static uint8_t daysInMonth(uint8_t m, uint16_t y) {
  const uint8_t d[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  return (m == 2 && isLeapYear(y)) ? 29 : d[m - 1];
}

// Y/Mo/D h:mi:s → 2000-01-01 起点の秒値
static uint32_t toEpoch2000(uint16_t Y, uint8_t Mo, uint8_t D,
                             uint8_t h, uint8_t mi, uint8_t s) {
  uint32_t days = 0;
  for (uint16_t y = 2000; y < Y; y++) days += isLeapYear(y) ? 366 : 365;
  for (uint8_t m = 1; m < Mo; m++) days += daysInMonth(m, Y);
  days += D - 1;
  return days * 86400UL + h * 3600UL + mi * 60UL + s;
}

// 2000-01-01 起点の秒値 → Y/Mo/D h:mi:s
static void fromEpoch2000(uint32_t e,
                           uint16_t &Y, uint8_t &Mo, uint8_t &D,
                           uint8_t &h, uint8_t &mi, uint8_t &s) {
  s  = (uint8_t)(e % 60); e /= 60;
  mi = (uint8_t)(e % 60); e /= 60;
  h  = (uint8_t)(e % 24); e /= 24;
  Y  = 2000;
  while (true) { uint16_t dy = isLeapYear(Y) ? 366 : 365; if (e < dy) break; e -= dy; Y++; }
  Mo = 1;
  while (true) { uint8_t dm = daysInMonth(Mo, Y); if (e < dm) break; e -= dm; Mo++; }
  D = (uint8_t)(e + 1);
}

// タイムスタンプ文字列を buf に書く
//   DS3231 読取成功:  "2026-07-01 14:30:00" (19 chars)
//   millis フォールバック設定済み: 同上
//   未設定:            "00:01:37"            (8 chars, 起動からの経過時間)
static void getTimestamp(char *buf, size_t len) {
  uint16_t Y; uint8_t Mo, D, h, mi, s;
  if (ds3231Read(Y, Mo, D, h, mi, s)) {
    snprintf(buf, len, "%04u-%02u-%02u %02u:%02u:%02u", Y, Mo, D, h, mi, s);
    return;
  }
  // DS3231 読み取り失敗: millis() オフセットにフォールバック
  if (s_rtcSet) {
    uint32_t now = s_rtcBase + (millis() - s_rtcSetMs) / 1000;
    fromEpoch2000(now, Y, Mo, D, h, mi, s);
    snprintf(buf, len, "%04u-%02u-%02u %02u:%02u:%02u", Y, Mo, D, h, mi, s);
  } else {
    uint32_t sec = millis() / 1000UL;
    snprintf(buf, len, "%02lu:%02lu:%02lu", sec / 3600, (sec % 3600) / 60, sec % 60);
  }
}

// ═══════════════════════════════════════
// ▼ ユーティリティ
// ═══════════════════════════════════════

static void printTimestamp() {
  char buf[24];
  getTimestamp(buf, sizeof(buf));
  Serial.print(buf);
}

// ═══════════════════════════════════════
// ▼ BLE スキャン (xiao_ble_scan と同一ロジック)
// ═══════════════════════════════════════

static int findDevice(uint8_t *mac) {
  for (int i = 0; i < deviceCount; i++)
    if (memcmp(devices[i].mac, mac, 6) == 0) return i;
  return -1;
}

static void updateDevice(uint8_t *mac, int rssi) {
  if (rssi < RSSI_THRESHOLD) return;
  int idx = findDevice(mac);
  if (idx >= 0) {
    devices[idx].count++;
    devices[idx].rssi_sum += rssi;
  } else if (deviceCount < MAX_DEVICES) {
    memcpy(devices[deviceCount].mac, mac, 6);
    devices[deviceCount].count    = 1;
    devices[deviceCount].rssi_sum = rssi;
    deviceCount++;
  }
}

static int estimatePeople() {
  int rssiList[MAX_DEVICES];
  int n = 0;

  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].count >= MIN_HITS)
      rssiList[n++] = devices[i].rssi_sum / devices[i].count;
  }
  if (n == 0) return 0;

  // 降順ソート
  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
      if (rssiList[i] < rssiList[j]) { int t = rssiList[i]; rssiList[i] = rssiList[j]; rssiList[j] = t; }

  // RSSI クラスタリング
  int  people = 0;
  bool used[MAX_DEVICES] = {};
  for (int i = 0; i < n; i++) {
    if (used[i]) continue;
    people++; used[i] = true;
    for (int j = i + 1; j < n; j++)
      if (!used[j] && abs(rssiList[i] - rssiList[j]) <= RSSI_MERGE_GAP)
        used[j] = true;
  }
  return (int)(people * CALIBRATION);
}

static void scanCallback(ble_gap_evt_adv_report_t *report) {
  updateDevice(report->peer_addr.addr, report->rssi);
  Bluefruit.Scanner.resume();
}

// ═══════════════════════════════════════
// ▼ キャリブレーション
// ═══════════════════════════════════════

static void findBestParams(int actual, int &outMinHits, float &outCalib) {
  outMinHits = MIN_HITS;
  outCalib   = CALIBRATION;
  float bestScore = 1e9f;
  for (int minH = 1; minH <= 20; minH++) {
    int q = 0;
    for (int i = 0; i < deviceCount; i++)
      if (devices[i].count >= minH) q++;
    if (q == 0) break;
    float calib = (float)actual / (float)q;
    float score = fabsf(calib - 1.0f);
    if (score < bestScore) { bestScore = score; outMinHits = minH; outCalib = calib; }
  }
}

static void printCalibResult() {
  if (calibSamples == 0) { Serial.println("[CALIB] No data. Send c<N>"); return; }
  int   minHits = (calibMinHitsSum + calibSamples / 2) / calibSamples;
  float calib   = calibRatioSum / (float)calibSamples;
  Serial.println("=== CALIB RESULT ===");
  Serial.print("  Samples : "); Serial.println(calibSamples);
  Serial.println("  --- Paste into your code ---");
  Serial.print("  static int   const MIN_HITS    = "); Serial.print(minHits);    Serial.println(";");
  Serial.print("  static float const CALIBRATION = "); Serial.print(calib, 2); Serial.println(";");
  Serial.println("====================");
}

// ═══════════════════════════════════════
// ▼ シリアルコマンド
// ═══════════════════════════════════════

static void processCommand(const char *cmd) {
  if (!cmd[0]) return;
  if      (cmd[0]=='d'||cmd[0]=='D') dumpLog();
  else if (cmd[0]=='e'||cmd[0]=='E') eraseLog();
  else if (cmd[0]=='c'||cmd[0]=='C') {
    int n = atoi(cmd + 1);
    if (n > 0) {
      calibMode = true; calibActual = n;
      calibSamples = 0; calibRatioSum = 0.0f; calibMinHitsSum = 0;
      Serial.print("[CALIB] actual="); Serial.print(n);
      Serial.print("  collecting "); Serial.print(CALIB_MIN_SAMPLES); Serial.println(" samples...");
    } else {
      Serial.println("[CALIB] Usage: c<N>  e.g. c5");
    }
  }
  else if (cmd[0]=='r'||cmd[0]=='R') printCalibResult();
  else if (cmd[0]=='x'||cmd[0]=='X') { calibMode = false; Serial.println("[CALIB] exited"); }
  else if (cmd[0]=='t'||cmd[0]=='T') {
    if (strlen(cmd) == 15) {
      char tmp[5];
      strncpy(tmp, cmd+1,  4); tmp[4] = '\0'; uint16_t Y  = (uint16_t)atoi(tmp);
      strncpy(tmp, cmd+5,  2); tmp[2] = '\0'; uint8_t  Mo = (uint8_t)atoi(tmp);
      strncpy(tmp, cmd+7,  2); tmp[2] = '\0'; uint8_t  D  = (uint8_t)atoi(tmp);
      strncpy(tmp, cmd+9,  2); tmp[2] = '\0'; uint8_t  h  = (uint8_t)atoi(tmp);
      strncpy(tmp, cmd+11, 2); tmp[2] = '\0'; uint8_t  mi = (uint8_t)atoi(tmp);
      strncpy(tmp, cmd+13, 2); tmp[2] = '\0'; uint8_t  s  = (uint8_t)atoi(tmp);
      s_rtcBase  = toEpoch2000(Y, Mo, D, h, mi, s);
      s_rtcSetMs = millis();
      s_rtcSet   = true;
      Serial.println(ds3231Write(Y, Mo, D, h, mi, s) ? "[DS3231] set OK" : "[DS3231] set FAILED");
      char ts[24]; getTimestamp(ts, sizeof(ts));
      Serial.print("[TIME] "); Serial.println(ts);
    } else {
      Serial.println("[TIME] Usage: t<YYYYMMDDHHMMSS>  e.g. t20260701143000");
    }
  }
  else Serial.println("[CMD] d=dump  e=erase  c<N>=calib  r=result  x=exit  t<YYYYMMDDHHMMSS>=settime");
}

static void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) { cmdBuf[cmdLen] = '\0'; processCommand(cmdBuf); cmdLen = 0; }
    } else if (cmdLen < (int)sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

// ═══════════════════════════════════════
// ▼ setup
// ═══════════════════════════════════════

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Serial.println("\n=== Toyono Park Monitor (Flex v3.04 / Phase-1: BLE+SD) ===");
  Serial.println("    Commands: d=dump  e=erase  c<N>=calib  r=result  x=exit  t<YYYYMMDDHHMMSS>=settime");

  // ソフト I2C 初期化 → TCA9534 (3V3_SW ON / SD CS=HIGH)
  i2cInit();
  if (!tca9534Init()) {
    Serial.println("[ERROR] TCA9534 init failed — I2C 配線を確認してください");
  } else {
    Serial.println("[TCA9534] OK  P2=HIGH(3V3_SW ON)  P3=LOW(CS assert)");
  }

  // DS3231 初期時刻設定 (SET_RTC_ON_BOOT=1 のときのみ)
#if SET_RTC_ON_BOOT
  setRtcFromCompileTime();
#endif

  // DS3231 時刻確認
  {
    uint16_t Y; uint8_t Mo, D, h, mi, s;
    if (ds3231Read(Y, Mo, D, h, mi, s)) {
      Serial.printf("[DS3231] %04u-%02u-%02u %02u:%02u:%02u\n", Y, Mo, D, h, mi, s);
    } else {
      Serial.println("[DS3231] 未接続 — t<YYYYMMDDHHMMSS> または SET_RTC_ON_BOOT=1 で時刻設定");
    }
  }

  // SD カード初期化 (3V3_SW ON 後に呼ぶこと)
  s_sdReady = initSd();

  // LED
  pinMode(LED_BLUE, OUTPUT);
  digitalWrite(LED_BLUE, HIGH);  // 消灯 (アクティブ LOW)

  // BLE
  Bluefruit.begin(1, 0);
  Bluefruit.setName("ParkMonitor_Flex");
  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.useActiveScan(false);
  Bluefruit.Scanner.setInterval(
    (uint16_t)(SCAN_INTERVAL_MS * 1000 / 625),
    (uint16_t)(SCAN_WINDOW_MS   * 1000 / 625)
  );

  Serial.println("[READY]");
}

// ═══════════════════════════════════════
// ▼ loop — スキャン(30s) → 記録 → スリープ(90s)
// ═══════════════════════════════════════

void loop() {

  // ① スキャン開始
  deviceCount = 0;
  Bluefruit.Scanner.start(0);
  Serial.print("[SCAN] "); Serial.print(SCAN_DURATION_MS / 1000); Serial.println("s start");

  uint32_t scanEnd   = millis() + SCAN_DURATION_MS;
  uint32_t lastBlink = millis();
  bool     ledOn     = false;

  while (millis() < scanEnd) {
    handleSerial();
    if (millis() - lastBlink >= LED_BLINK_MS) {
      ledOn = !ledOn;
      digitalWrite(LED_BLUE, ledOn ? LOW : HIGH);
      lastBlink = millis();
    }
    delay(10);
  }

  // ② スキャン停止 / LED 消灯
  Bluefruit.Scanner.stop();
  digitalWrite(LED_BLUE, HIGH);

  // ③ 人数推定 + 出力 + ログ
  int people = estimatePeople();
  Serial.println("------------------------------");
  Serial.print("["); printTimestamp(); Serial.print("]");
  Serial.print("  People="); Serial.print(people);
  Serial.print("  devices="); Serial.println(deviceCount);
  logRecord(people, deviceCount);

  // ④ キャリブレーション
  if (calibMode) {
    int   bestMinHits;
    float bestCalib;
    findBestParams(calibActual, bestMinHits, bestCalib);
    calibMinHitsSum += bestMinHits;
    calibRatioSum   += bestCalib;
    calibSamples++;
    Serial.print("[CALIB] #"); Serial.print(calibSamples);
    Serial.print("  actual=");        Serial.print(calibActual);
    Serial.print("  MIN_HITS→");     Serial.print(bestMinHits);
    Serial.print("  CALIBRATION→");  Serial.println(bestCalib, 2);
    if (calibSamples >= CALIB_MIN_SAMPLES) { printCalibResult(); calibMode = false; }
  }

  // ⑤ スリープ (nRF52 delay は低消費電力スリープ)
  Serial.print("[SLEEP] "); Serial.print(SLEEP_DURATION_MS / 1000); Serial.println("s");
  Serial.flush();

  uint32_t sleepEnd = millis() + SLEEP_DURATION_MS;
  while (millis() < sleepEnd) { handleSerial(); delay(10); }
}


// ╔══════════════════════════════════════════════════════════╗
// ║  Phase-2: PIR センサ (有効化手順)                        ║
// ║  配線: Sigfox コネクタライン                              ║
// ║    D8 = PIR OUT, D9 = (未使用), VCC/GND も同列           ║
// ║  → D9 は SD CS ダミー用途から PIR 用 GND に変更可         ║
// ╚══════════════════════════════════════════════════════════╝
//
// Step 1: 以下のコメントを外す
//
// #define PIR_PIN D8
// static int s_pirHits = 0;
//
// Step 2: setup() 末尾に追加
//   pinMode(PIR_PIN, INPUT);
//
// Step 3: loop() の while (millis() < scanEnd) ブロック内に追加
//   if (digitalRead(PIR_PIN) == HIGH) s_pirHits++;
//
// Step 4: ③ 人数推定 + 出力 の行を変更
//   Serial.print("  PIR="); Serial.println(s_pirHits);
//   logRecord(people, deviceCount, s_pirHits);  // ← 引数追加
//   s_pirHits = 0;
//
// Step 5: logRecord() のシグネチャと CSV ヘッダを更新
//   "timestamp,people,devices,pir"
//
// ─────────────────────────────────────────────────────────────
//
// ╔══════════════════════════════════════════════════════════╗
// ║  Phase-3: 音声 (PDM) センサ (有効化手順)                 ║
// ║  必要ハード: XIAO nRF52840 Sense (PDM マイク内蔵)        ║
// ╚══════════════════════════════════════════════════════════╝
//
// Step 1: platformio.ini の kosme/arduinoFFT のコメントを外す
//
// Step 2: 以下のインクルードを追加
//   #include <PDM.h>
//   #include <arduinoFFT.h>
//
// Step 3: 音声取得 + FFT の実装は以下を参照:
//   case00_common/nrf52Sense_pdm_sound_monitor/src/main.cpp
//   主要パラメータ: PDM_GAIN=30, FFT_SIZE=256, SAMPLE_RATE=16000
//   4バンド: L(125-500Hz), ML(500-2kHz), MH(2-4kHz), H(4-8kHz)
//
// Step 4: logRecord() に音声値を追加し CSV ヘッダを更新
//   "timestamp,people,devices,pir,rms_dbfs,L,ML,MH,H"
