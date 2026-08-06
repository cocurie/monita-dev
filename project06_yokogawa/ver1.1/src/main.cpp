/**
 * Monita Flex 横河基板 ver1.1 — 本番用スケッチ
 *
 * 【対象ハード】
 *   XIAO ESP32-C3 + ver1.1基板（HX711×5 / 74HC4051 MUX / MCP23008 / ADS1115 / MCP9600 / SD）
 *
 * 【チャンネル構成（本番仕様）】
 *   CH1〜CH5 : HX711（MUX ch0〜4）。CH_TYPE[] でチャンネルごとに「ひずみ」「変位」を選択
 *   CH6      : 熱電対 K型（MCP9600）固定
 *   CH7, CH8 : 電圧入力（ADS1115 差動 2ch）固定
 *
 * 【既知の不具合】
 *   CH4（MUX ch3, 74HC4051 Y3/pin12）はMUXチップの部分故障によりタイムアウトする
 *   （2026/08/04 ブレッドボード検証で切り分け済み。74HC4051交換で解消見込み）。
 *
 * 【使い方】
 *   1. src/main.cpp を退避する（例: main.cpp → main_debug.cpp.bak）
 *   2. このファイルを main.cpp にコピーする
 *      cp main_production.cpp.bak main.cpp
 *   3. 下記「設定項目」を編集してビルド・書き込み
 *
 * 【通信方式が COMM_USE_BLE のとき】
 *   常時: BLEアドバタイズ(Manufacturer Specific Data)でCH1〜8実測値をブロードキャスト配信。
 *   追加: GATTサーバー(Nordic UART Service)を常設し、コントローラーからの設定変更・
 *         Tare・時刻同期・ログダンプなどのコマンドを受け付ける（接続時のみ）。
 *   コマンド一覧は下記 handleCommand() を参照。
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <algorithm>
#include <Preferences.h>
#include "soft_i2c.h"

#if defined(COMM_USE_BLE)
#include <NimBLEDevice.h>
#endif
#if defined(COMM_USE_WIFI)
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#endif

// ============================================================================
// ■■■ 設定項目（本番運用でここだけ編集する想定） ■■■
// ============================================================================

// ---- 計測間隔・平均化のデフォルト値 ----
// 実際に使う値は起動時にNVSから読み込む（NVSに保存が無い初回のみ、この値を使う）。
// 計測間隔は分単位で管理する（秒単位の細かい制御は運用上不要なため）。
static const uint32_t MEASURE_INTERVAL_MIN_DEFAULT = 1;  // 計測間隔（分）
static const uint8_t  AVG_N_DEFAULT = 5;   // 1回の測定あたりの平均サンプル数
static const uint8_t  AVG_M_DEFAULT = 5;   // 平均値をM回とり、その中央値(メジアン)を採用

static const uint32_t INTERVAL_MIN_MIN = 1;      // 最短1分
static const uint32_t INTERVAL_MIN_MAX = 1440;   // 最長24時間
static const uint8_t  AVG_N_MIN = 1, AVG_N_MAX = 50;
static const uint8_t  AVG_M_MIN = 1, AVG_M_MAX = 25;  // g_hxSamples[] のサイズと連動

// ---- RTC時刻設定 ----
// 本基板には専用RTCチップ（DS3231等）は搭載されていない（netlist確認済み・2026/08時点）。
// ESP32内蔵RTC（time.h）をソフトウェアで運用する。
//   - 工場出荷時 / 初回書き込み時はコンパイル時刻を初期値として設定する
//   - COMM_USE_BLEビルドはWiFiを持たずNTP同期できないため、現場ではコントローラーから
//     SETTIME コマンドで時刻を設定する運用とする（電源を切るたびに再設定が必要）
#define RTC_DEFAULT_YEAR   2026
#define RTC_DEFAULT_MONTH  1     // 1-12
#define RTC_DEFAULT_DAY    1
#define RTC_DEFAULT_HOUR   0
#define RTC_DEFAULT_MIN    0
#define RTC_DEFAULT_SEC    0

// ---- CH1〜CH5 種別選択（ひずみ / 変位）----
enum ChannelType : uint8_t {
    CH_TYPE_STRAIN       = 0,  // ひずみゲージ
    CH_TYPE_DISPLACEMENT = 1,  // 変位計
};
// index 0=CH1, 1=CH2, 2=CH3, 3=CH4, 4=CH5 ―― 現場のセンサー構成に合わせて編集する
static const ChannelType CH_TYPE[5] = {
    CH_TYPE_STRAIN,        // CH1
    CH_TYPE_STRAIN,        // CH2
    CH_TYPE_STRAIN,        // CH3
    CH_TYPE_DISPLACEMENT,  // CH4
    CH_TYPE_DISPLACEMENT,  // CH5
};

// ---- ひずみ・変位 変換係数 ----
// raw(HX711 24bit符号付き) → 物理値(µε等) の変換係数。 physical = (raw - offset[ch]) / COEFF
//
// HX711 VCC=3V（規定動作範囲2.6〜5.5V内）で運用。2Vはレギュレーターを介さず3Vに戻した
// （2Vでは内部PGAのゲイン圧縮により不安定だったため。詳細: test_results/CH1_strain_test_2V_20260804.md）。
// 係数1110は3V実測（印加200〜1000µε）でほぼ1:1・誤差1〜2%程度を確認済み（2026/08/04）。
// offset[ch] はチャンネルごとのゼロ点補正値。デフォルト0、TAREコマンドで更新しNVSに保存する。
static constexpr float STRAIN_DISP_COEFF = 1110.0f;

// ---- SDカード ----
#define SD_LOG_ENABLED_DEFAULT true   // 起動時デフォルトでSD保存を有効にする

// ---- 通信方式選択 ----
// platformio.ini の build_flags で以下のいずれか1つを指定する:
//   -D COMM_USE_WIFI    … WiFi送信モード
//   -D COMM_USE_BLE     … BLE NUS送信モード（コントローラー/Gateway接続）
//   -D COMM_USE_SERIAL  … デバッグ用。送信せずシリアルモニタに計測結果を出力するだけ
#if !defined(COMM_USE_WIFI) && !defined(COMM_USE_BLE) && !defined(COMM_USE_SERIAL)
#error "platformio.ini の build_flags に -D COMM_USE_WIFI / -D COMM_USE_BLE / -D COMM_USE_SERIAL のいずれかを指定してください"
#endif

// ---- デバイス識別（WiFi/BLE共通） ----
// 実際に使う値は起動時にNVSから読み込む（複数台運用時はコントローラーのDEVIDコマンドで変更する）。
static const uint8_t DEVICE_ID_NUM_DEFAULT = 0x01;

#if defined(COMM_USE_WIFI)
static const char* WIFI_SSID     = "GlocalNet_0VWUPL";
static const char* WIFI_PASSWORD = "63388885";
// GAS（Google Apps Script）Webアプリの/execURL。
// POSTはESP32とのSSLリダイレクト処理の相性が悪いため使わない（case00_common/esp32c3_gsheets
// で確認済みの既知の問題）。GAS側は doGet() で受ける前提とし、GET+クエリパラメーターで送信する。
static const char* GAS_URL       = "https://script.google.com/macros/s/AKfycbw6Pf1dmkufEoGBUwp9DIU3to34SyVcenCJYUEok_KFEHmxu5wh4sbNXi6O1spZZbtSJw/exec";
static const char* DEVICE_ID     = "yokogawa_ver1_1_TODO";

// ---- NTP時刻同期（WiFi接続時） ----
static const char* NTP_SERVER1   = "ntp.nict.jp";       // 産総研NICT（国内優先）
static const char* NTP_SERVER2   = "time.google.com";
static const long  NTP_GMT_OFFSET_SEC = 9 * 3600;        // JST = UTC+9
static const int   NTP_DAYLIGHT_OFFSET_SEC = 0;           // 日本はサマータイムなし
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
static const uint32_t NTP_SYNC_TIMEOUT_MS     = 10000;
#endif

#if defined(COMM_USE_BLE)
// ---- BLEアドバタイズ設定 ----
// project07_NEXCO/firmware_child のMSD（Manufacturer Specific Data）方式を踏襲。
// CompanyID 0xFFFF はBluetooth SIG未割当のテスト用領域（Gateway側は自前でフィルタする前提）。
static const uint8_t  BLE_COMPANY_ID_LO = 0xFF;
static const uint8_t  BLE_COMPANY_ID_HI = 0xFF;
// 横河専用Gateway（project06_yokogawa/gateway_v1.1、EXPECTED_PKT_TYPE=0x11）向け。
// 汎用gateway_v1.1のv3.03互換フィルタ（PktType=0x03、CH1-4固定オフセット）とは非互換のため、
// 0x03を間借りせず横河専用の値に戻した（2026-08-05、8CH対応Gateway新設に伴う変更）。
static const uint8_t  BLE_PKT_TYPE      = 0x11;
static const uint32_t BLE_ADV_INTERVAL_MS = 1000;

// ---- GATT NUS (コントローラーからのコマンド受信用) ----
// monita-controller (project06_yokogawa) 側と同一のUUIDを使用。
static const char* NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char* NUS_CHAR_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";  // コントローラー→本機
static const char* NUS_CHAR_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";  // 本機→コントローラー
static const uint16_t BLE_PREFERRED_MTU = 247;
static const size_t   DUMP_CHUNK_SIZE   = 180;  // BLE_PREFERRED_MTU-3 以下に収める
#endif

// ============================================================================
// ピン定義（ver1.1 netlist 確定値）
// ============================================================================
#define HX711_PD_SCK  D1
#define HX711_DOUT    D2
#define PIN_SDA       D4
#define PIN_SCL       D5
#define SD_SCK        D8
#define SD_MISO       D9
#define SD_MOSI       D10
#define SD_CS         D7

// MUX切替後、HX711読み出し開始までの待ち時間（接触不良/信号なまり対策で調整）
static const uint16_t MUX_SETTLE_MS = 50;

// ============================================================================
// 実行時設定（NVS永続化） — 計測間隔・平均化回数・デバイスID・チャンネルオフセット
// ============================================================================
static Preferences g_prefs;

static volatile uint32_t g_measure_interval_min = MEASURE_INTERVAL_MIN_DEFAULT;  // NVSに保存する正の値(分)
static volatile uint8_t  g_avg_n = AVG_N_DEFAULT;
static volatile uint8_t  g_avg_m = AVG_M_DEFAULT;
static volatile uint8_t  g_device_id = DEVICE_ID_NUM_DEFAULT;
static volatile bool     g_running = true;
static float g_ch_offset[5] = {0, 0, 0, 0, 0};  // TAREで更新するチャンネルごとのゼロ点(raw単位)

// loop()の間隔判定は内部的にミリ秒で行うため、分→ミリ秒に変換して返すヘルパー
static inline uint32_t measureIntervalMs() { return g_measure_interval_min * 60UL * 1000UL; }

static void settingsLoad() {
    g_prefs.begin("monita", true);  // read-only open
    g_measure_interval_min = g_prefs.getUInt("intervalMin", MEASURE_INTERVAL_MIN_DEFAULT);
    g_avg_n      = g_prefs.getUChar("avgN", AVG_N_DEFAULT);
    g_avg_m      = g_prefs.getUChar("avgM", AVG_M_DEFAULT);
    g_device_id  = g_prefs.getUChar("devid", DEVICE_ID_NUM_DEFAULT);
    for (int i = 0; i < 5; i++) {
        char key[8];
        snprintf(key, sizeof(key), "off%d", i);
        g_ch_offset[i] = g_prefs.getFloat(key, 0.0f);
    }
    g_prefs.end();
}
static void settingsSaveInterval() { g_prefs.begin("monita", false); g_prefs.putUInt("intervalMin", g_measure_interval_min); g_prefs.end(); }
static void settingsSaveAvgN()     { g_prefs.begin("monita", false); g_prefs.putUChar("avgN", g_avg_n); g_prefs.end(); }
static void settingsSaveAvgM()     { g_prefs.begin("monita", false); g_prefs.putUChar("avgM", g_avg_m); g_prefs.end(); }
static void settingsSaveDevId()    { g_prefs.begin("monita", false); g_prefs.putUChar("devid", g_device_id); g_prefs.end(); }
static void settingsSaveOffset(int ch) {
    char key[8];
    snprintf(key, sizeof(key), "off%d", ch);
    g_prefs.begin("monita", false);
    g_prefs.putFloat(key, g_ch_offset[ch]);
    g_prefs.end();
}

// ============================================================================
// MCP23008 — 74HC4051 MUXチャンネル選択
// 実配線: MCP23008 GP2→S0(pin11), GP1→S1(pin10), GP0→S2(pin9)
// ============================================================================
static constexpr uint8_t MCP23008_ADDR = 0x20;
static constexpr uint8_t MCP_IODIR     = 0x00;
static constexpr uint8_t MCP_GPIO      = 0x09;

static bool mcpWrite(uint8_t reg, uint8_t val) { return softI2CWriteReg(MCP23008_ADDR, reg, val); }

static bool mcpInit() {
    if (!mcpWrite(MCP_IODIR, 0xF8)) return false;  // GP0/GP1/GP2 = 出力
    return mcpWrite(MCP_GPIO, 0x00);
}

// ch(0〜4) → 74HC4051物理ピン: 0=Y0(pin13) 1=Y1(pin14) 2=Y2(pin15) 3=Y3(pin12) 4=Y4(pin1)
static bool muxSelect(uint8_t ch) {
    uint8_t val = ((ch & 0x01) << 2)   // S0(bit0)→GP2
                | ((ch & 0x02) << 0)   // S1(bit1)→GP1
                | ((ch & 0x04) >> 2);  // S2(bit2)→GP0
    return mcpWrite(MCP_GPIO, val);
}

// CH番号(0=CH1〜4=CH5) → MUX ch(0〜4) の対応表。
// 実配線でCH2/CH5が入れ替わっていたため、ソフト側でスワップして吸収している
// （2026/08/04、ひずみ発生装置での実測で判明。CH2⇔CH5の物理配線は未修正）。
static const uint8_t CH_TO_MUX[5] = { 0, 4, 2, 3, 1 };  // CH1,CH2,CH3,CH4,CH5

// ============================================================================
// HX711 — ビットバング読み出し（全CH共通 PD_SCK/DOUT、MUX選択後に呼ぶ）
// ============================================================================
static int32_t hx711Read() {
    uint32_t deadline = millis() + 200;
    while (digitalRead(HX711_DOUT) == HIGH) {
        if (millis() > deadline) return INT32_MIN;
        delay(1);
    }
    uint32_t raw = 0;
    for (int i = 0; i < 24; i++) {
        digitalWrite(HX711_PD_SCK, HIGH); delayMicroseconds(1);
        raw = (raw << 1) | digitalRead(HX711_DOUT);
        digitalWrite(HX711_PD_SCK, LOW);  delayMicroseconds(1);
    }
    digitalWrite(HX711_PD_SCK, HIGH); delayMicroseconds(1);
    digitalWrite(HX711_PD_SCK, LOW);
    if (raw & 0x800000) raw |= 0xFF000000;
    return (int32_t)raw;
}

// g_avg_n回サンプリングして単純平均した生値を返す（読み取り失敗が全て失敗ならINT32_MIN相当でNANを返す）
static float hx711ReadAveraged() {
    int64_t sum = 0;
    int count = 0;
    uint8_t n = g_avg_n;
    for (uint8_t i = 0; i < n; i++) {
        int32_t r = hx711Read();
        if (r != INT32_MIN) { sum += r; count++; }
    }
    if (count == 0) return NAN;
    return (float)((double)sum / count);
}

// hx711ReadAveraged() をg_avg_m回繰り返し、その中央値(メジアン)を最終raw値として返す。
// 事前に muxSelect() 済みであること。
static float hx711ReadMedianOfAverages() {
    float vals[AVG_M_MAX];
    int n = 0;
    uint8_t m = g_avg_m;
    for (uint8_t i = 0; i < m && n < AVG_M_MAX; i++) {
        float v = hx711ReadAveraged();
        if (!isnan(v)) vals[n++] = v;
    }
    if (n == 0) return NAN;
    std::sort(vals, vals + n);
    return vals[n / 2];
}

static float hx711ToPhysical(float rawMedian, uint8_t ch) {
    return (rawMedian - g_ch_offset[ch]) / STRAIN_DISP_COEFF;
}

// ============================================================================
// ADS1115 — CH7, CH8（電圧入力、差動）
// ============================================================================
static constexpr uint8_t  ADS_ADDR     = 0x48;
static constexpr uint8_t  ADS_REG_CONV = 0x00;
static constexpr uint8_t  ADS_REG_CFG  = 0x01;
static constexpr uint16_t ADS_CFG_CH7  = 0x8583;  // A0-A1 / PGA±2.048V / single / 128SPS
static constexpr uint16_t ADS_CFG_CH8  = 0xB583;  // A2-A3

// チャンネルごとのゲイン・オフセット補正（Vin = ADS電圧 × GAIN + OFFSET）
// 2026/08/04 の±10V掃引実測データ（test_results/CH7_voltage_test_20260804.md,
// CH8_voltage_test_20260804.md）を最小二乗回帰して算出。基板ごとに要再校正。
static const float GAIN_CH7   = 7.5407f;
static const float OFFSET_CH7 = 0.086f;
static const float GAIN_CH8   = 7.5658f;
static const float OFFSET_CH8 = 0.067f;

static int16_t adsReadSingle(uint16_t config) {
    if (!softI2CWriteReg16(ADS_ADDR, ADS_REG_CFG, config)) return INT16_MIN;
    delay(15);
    uint32_t deadline = millis() + 50;
    while (millis() < deadline) {
        uint8_t buf[2];
        if (softI2CReadReg(ADS_ADDR, ADS_REG_CFG, buf, 2) && (buf[0] & 0x80)) break;
        delay(2);
    }
    uint8_t buf[2];
    if (!softI2CReadReg(ADS_ADDR, ADS_REG_CONV, buf, 2)) return INT16_MIN;
    return (int16_t)((buf[0] << 8) | buf[1]);
}

static float adsReadVoltage(uint16_t config, float gain, float offset) {
    int16_t raw = adsReadSingle(config);
    if (raw == INT16_MIN) return NAN;
    float v_ads = raw * (2.048f / 32768.0f);
    return v_ads * gain + offset;
}

// ============================================================================
// MCP9600 — CH6 熱電対 K型
// ============================================================================
static constexpr uint8_t MCP9600_HOTJUNCTION  = 0x00;
static constexpr uint8_t MCP9600_STATUS       = 0x04;
static constexpr uint8_t MCP9600_SENSORCONFIG = 0x05;
static constexpr uint8_t MCP9600_DEVICECONFIG = 0x06;
static constexpr uint8_t MCP9600_DEVICEID     = 0x20;

// MCP9601のSTATUSレジスタ ビット位置（MCP9600とは異なる。
// case01_Flex/test_sketches/15_mcp9600 のコメント参照）。
static constexpr uint8_t MCP9601_STATUS_OPENCIRCUIT  = 0x10;  // bit4
static constexpr uint8_t MCP9601_STATUS_SHORTCIRCUIT = 0x20;  // bit5

// 断線・短絡フラグのデバウンス回数（VSENSEノードが高インピーダンスでノイズを拾いやすいため、
// 単発フラグは誤報とみなし連続でこの回数立った時のみ確定させる。15_mcp9600での実測検証値を踏襲）。
static constexpr uint8_t MCP9600_FAULT_DEBOUNCE_COUNT = 10;

static uint8_t g_mcp9600_addr = 0;

static uint8_t mcp9600Scan() {
    for (uint8_t a = 0x60; a <= 0x67; a++) if (softI2CProbe(a)) return a;
    return 0;
}
static bool mcp9600Init(uint8_t addr) {
    uint8_t id[2] = {};
    if (!softI2CReadReg(addr, MCP9600_DEVICEID, id, 2)) return false;
    // 0x40=MCP9600, 0x41=MCP9601（熱電対FlexモジュールはMCp9601採用に更新済み。
    // case01_Flex/test_sketches/15_mcp9600 も Adafruit_MCP9601 に対応済み）。
    // 0x40固定チェックだと実チップ(0x41)と不一致になり誤って未検出扱いになるため両対応する。
    if (id[0] != 0x40 && id[0] != 0x41) return false;
    softI2CWriteReg(addr, MCP9600_SENSORCONFIG, 0x03);
    softI2CWriteReg(addr, MCP9600_DEVICECONFIG, 0x80);
    return true;
}
static float mcp9600ReadTemp(uint8_t addr) {
    uint8_t buf[2];
    if (!softI2CReadReg(addr, MCP9600_HOTJUNCTION, buf, 2)) return NAN;
    return (int16_t)((buf[0] << 8) | buf[1]) * 0.0625f;
}
static uint8_t mcp9600ReadStatus(uint8_t addr) {
    uint8_t st = 0;
    softI2CReadReg(addr, MCP9600_STATUS, &st, 1);
    return st;
}

// STATUSレジスタをMCP9600_FAULT_DEBOUNCE_COUNT回、約1秒間隔でサンプリングし、
// 全回連続でフラグが立っていた場合のみ断線/短絡を確定させる（15_mcp9600のデバウンス方式を踏襲）。
// 1回でも立たなかった回があれば「連続で立った」ことにはならないため未確定のまま。
static void mcp9600CheckFaults(uint8_t addr, bool& ocFault, bool& scFault,
                                uint8_t& ocRaisedCount, uint8_t& scRaisedCount) {
    ocRaisedCount = 0;
    scRaisedCount = 0;
    for (uint8_t i = 0; i < MCP9600_FAULT_DEBOUNCE_COUNT; i++) {
        uint8_t st = mcp9600ReadStatus(addr);
        if (st & MCP9601_STATUS_OPENCIRCUIT)  ocRaisedCount++;
        if (st & MCP9601_STATUS_SHORTCIRCUIT) scRaisedCount++;
        if (i < MCP9600_FAULT_DEBOUNCE_COUNT - 1) delay(1000);
    }
    ocFault = (ocRaisedCount >= MCP9600_FAULT_DEBOUNCE_COUNT);
    scFault = (scRaisedCount >= MCP9600_FAULT_DEBOUNCE_COUNT);
}

// ============================================================================
// RTC（ESP32内蔵、ソフトウェア運用）
// ============================================================================
RTC_DATA_ATTR static bool g_rtc_set = false;

// 専用RTCチップが無いため、コントローラーからSETTIMEを受けるまでの間は
// 「ファームをビルドした日時」を暫定の初期値として使う（固定値2026/01/01よりは実態に近い）。
static bool parseBuildDateTime(struct tm& out) {
    static const char* MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char monStr[4] = {};
    int day, year, hh, mm, ss;
    // __DATE__ 例: "Aug 10 2026", __TIME__ 例: "05:51:00"
    if (sscanf(__DATE__, "%3s %d %d", monStr, &day, &year) != 3) return false;
    if (sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss) != 3) return false;

    const char* p = strstr(MONTHS, monStr);
    if (p == nullptr) return false;
    int mon = (int)((p - MONTHS) / 3);  // 0-11

    out = {};
    out.tm_year = year - 1900;
    out.tm_mon  = mon;
    out.tm_mday = day;
    out.tm_hour = hh;
    out.tm_min  = mm;
    out.tm_sec  = ss;
    return true;
}

static void rtcApplyDefault() {
    struct tm tm0 = {};
    if (!parseBuildDateTime(tm0)) {
        // 万一パースに失敗したらフォールバック
        tm0.tm_year = RTC_DEFAULT_YEAR - 1900;
        tm0.tm_mon  = RTC_DEFAULT_MONTH - 1;
        tm0.tm_mday = RTC_DEFAULT_DAY;
        tm0.tm_hour = RTC_DEFAULT_HOUR;
        tm0.tm_min  = RTC_DEFAULT_MIN;
        tm0.tm_sec  = RTC_DEFAULT_SEC;
    }
    time_t t = mktime(&tm0);
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
}

// BLE経由の SETTIME コマンドから呼ぶ
static void rtcSetTime(uint16_t year, uint8_t mon, uint8_t day, uint8_t hh, uint8_t mm, uint8_t ss) {
    struct tm tm0 = {};
    tm0.tm_year = year - 1900;
    tm0.tm_mon  = mon - 1;
    tm0.tm_mday = day;
    tm0.tm_hour = hh;
    tm0.tm_min  = mm;
    tm0.tm_sec  = ss;
    time_t t = mktime(&tm0);
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
    g_rtc_set = true;
}

static void rtcNowString(char* out, size_t outLen) {
    time_t now; time(&now);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    strftime(out, outLen, "%Y-%m-%d %H:%M:%S", &tmNow);
}

// ============================================================================
// SDカード ロギング
// ============================================================================
static bool g_sd_ok = false;
static bool g_sd_log_enabled = SD_LOG_ENABLED_DEFAULT;
static const char* LOG_PATH = "/monita_log.csv";

// loop()タスクとBLEタスク(DUMP読み出し)から同時にSDへアクセスされるのを防ぐミューテックス
static SemaphoreHandle_t g_sdMutex = nullptr;

static bool sdInit() {
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS)) return false;
    if (!SD.exists(LOG_PATH)) {
        File f = SD.open(LOG_PATH, FILE_WRITE);
        if (f) {
            f.println("timestamp,ch1,ch1_type,ch2,ch2_type,ch3,ch3_type,ch4,ch4_type,ch5,ch5_type,ch6_tempC,ch7_V,ch8_V");
            f.close();
        }
    }
    return true;
}

static const char* chTypeLabel(ChannelType t) {
    return t == CH_TYPE_STRAIN ? "strain" : "disp";
}

// ============================================================================
// 全CH計測 → SD保存 → 送信
// ============================================================================
struct Measurement {
    float    hx_raw[5];   // hx711ReadMedianOfAverages() の結果（オフセット適用前）
    float    hx_phys[5];
    bool     hx_ok[5];
    float    ch6_tempC;
    bool     ch6_ok;
    bool     ch6_openCircuit;   // デバウンス確定した断線
    bool     ch6_shortCircuit;  // デバウンス確定した短絡
    float    ch7_V, ch8_V;
    bool     ch7_ok, ch8_ok;
};

static Measurement measureAll() {
    Measurement m = {};

    for (uint8_t ch = 0; ch < 5; ch++) {
        muxSelect(CH_TO_MUX[ch]);
        delay(MUX_SETTLE_MS);
        float raw = hx711ReadMedianOfAverages();
        m.hx_ok[ch]   = !isnan(raw);
        m.hx_raw[ch]  = raw;
        m.hx_phys[ch] = m.hx_ok[ch] ? hx711ToPhysical(raw, ch) : NAN;
    }

    if (g_mcp9600_addr) {
        float t = mcp9600ReadTemp(g_mcp9600_addr);
        m.ch6_ok = !isnan(t);
        m.ch6_tempC = t;

        uint8_t ocN = 0, scN = 0;
        mcp9600CheckFaults(g_mcp9600_addr, m.ch6_openCircuit, m.ch6_shortCircuit, ocN, scN);
        if (ocN && !m.ch6_openCircuit) {
            Serial.printf("[NOISE] CH6 オープン回路フラグを %u/%u 回検出（デバウンス未確定のため棄却）\n",
                          ocN, MCP9600_FAULT_DEBOUNCE_COUNT);
        }
        if (scN && !m.ch6_shortCircuit) {
            Serial.printf("[NOISE] CH6 ショートフラグを %u/%u 回検出（デバウンス未確定のため棄却）\n",
                          scN, MCP9600_FAULT_DEBOUNCE_COUNT);
        }
    }

    m.ch7_V = adsReadVoltage(ADS_CFG_CH7, GAIN_CH7, OFFSET_CH7);
    m.ch7_ok = !isnan(m.ch7_V);
    m.ch8_V = adsReadVoltage(ADS_CFG_CH8, GAIN_CH8, OFFSET_CH8);
    m.ch8_ok = !isnan(m.ch8_V);

    return m;
}

// 現在のMUX位置のままチャンネルchを再計測し、その生値をゼロ点として記録する（TAREコマンド用）
static float tareChannel(uint8_t ch) {
    muxSelect(CH_TO_MUX[ch]);
    delay(MUX_SETTLE_MS);
    float raw = hx711ReadAveraged();
    if (isnan(raw)) return NAN;
    g_ch_offset[ch] = raw;
    settingsSaveOffset(ch);
    return raw;
}

static void logToSD(const Measurement& m) {
    if (!g_sd_log_enabled || !g_sd_ok) return;
    if (xSemaphoreTake(g_sdMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        Serial.println("[SD] busy(dump中?) → 今回のログ保存をスキップ");
        return;
    }
    File f = SD.open(LOG_PATH, FILE_APPEND);
    if (f) {
        char ts[24];
        rtcNowString(ts, sizeof(ts));

        f.print(ts);
        for (uint8_t ch = 0; ch < 5; ch++) {
            f.print(',');
            if (m.hx_ok[ch]) f.print(m.hx_phys[ch], 3); else f.print("NaN");
            f.print(',');
            f.print(chTypeLabel(CH_TYPE[ch]));
        }
        f.print(',');
        if (m.ch6_ok) f.print(m.ch6_tempC, 2); else f.print("NaN");
        f.print(',');
        if (m.ch7_ok) f.print(m.ch7_V, 3); else f.print("NaN");
        f.print(',');
        if (m.ch8_ok) f.print(m.ch8_V, 3); else f.print("NaN");
        f.println();
        f.close();
    }
    xSemaphoreGive(g_sdMutex);
}

// ============================================================================
// 通信（WiFi / BLE / シリアルデバッグ）
// ============================================================================
#if defined(COMM_USE_BLE)

static NimBLECharacteristic* g_pTxCharacteristic = nullptr;
static volatile bool g_bleControllerConnected = false;

static void notifyReply(const String& msg) {
    if (g_pTxCharacteristic == nullptr || !g_bleControllerConnected) return;
    g_pTxCharacteristic->setValue((uint8_t*)msg.c_str(), msg.length());
    g_pTxCharacteristic->notify();
    Serial.print("[BLE] notify: ");
    Serial.println(msg);
}

// SDログファイルをチャンク分割してNotifyでコントローラーへ送る。
// フレーミング: "DUMPHDR:<total_bytes>" → 生データチャンクを複数notify → "DUMPEND"
static void handleDumpCommand() {
    if (!g_sd_ok) { notifyReply("ERR:NOSD"); return; }
    if (xSemaphoreTake(g_sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        notifyReply("ERR:SDBUSY");
        return;
    }
    File f = SD.open(LOG_PATH, FILE_READ);
    if (!f) {
        xSemaphoreGive(g_sdMutex);
        notifyReply("ERR:OPEN");
        return;
    }

    uint32_t total = f.size();
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "DUMPHDR:%lu", (unsigned long)total);
    notifyReply(hdr);
    delay(30);

    uint8_t buf[DUMP_CHUNK_SIZE];
    size_t r;
    uint32_t sent = 0;
    while ((r = f.read(buf, sizeof(buf))) > 0 && g_bleControllerConnected) {
        g_pTxCharacteristic->setValue(buf, r);
        g_pTxCharacteristic->notify();
        sent += r;
        delay(20);  // BLEスタックの送信キュー詰まり防止
    }
    f.close();
    xSemaphoreGive(g_sdMutex);

    Serial.printf("[BLE] DUMP完了: %lu/%lu バイト送信\n", (unsigned long)sent, (unsigned long)total);
    notifyReply("DUMPEND");
}

// コマンド解釈:
//   GET                          現在の設定・状態を取得
//   SETTIME:YYYYMMDDHHMMSS       時刻設定
//   INTERVAL:N                   計測間隔をN分に変更（1〜1440）
//   AVGN:N                       平均化サンプル数を変更（1〜50）
//   AVGM:N                       メジアン算出用の測定回数を変更（1〜25）
//   START / STOP                 計測ループの再開・一時停止
//   TARE                         CH1〜5のゼロ点補正（現在値を新しいゼロとする）
//   DEVID:N                      デバイスIDを変更（1〜255）
//   DUMP                         SDログをコントローラーへ転送
static void handleCommand(const String& raw) {
    String cmd = raw;
    cmd.trim();
    Serial.print("[BLE] recv: ");
    Serial.println(cmd);

    if (cmd == "GET") {
        char ts[24];
        rtcNowString(ts, sizeof(ts));
        char reply[128];
        snprintf(reply, sizeof(reply), "INTERVAL=%lu;N=%u;M=%u;RUN=%d;DEVID=%u;TIME=%s",
                  (unsigned long)g_measure_interval_min, g_avg_n, g_avg_m,
                  g_running ? 1 : 0, g_device_id, ts);
        notifyReply(reply);

    } else if (cmd.startsWith("SETTIME:")) {
        String v = cmd.substring(8);
        if (v.length() == 14) {
            uint16_t year = v.substring(0, 4).toInt();
            uint8_t  mon  = v.substring(4, 6).toInt();
            uint8_t  day  = v.substring(6, 8).toInt();
            uint8_t  hh   = v.substring(8, 10).toInt();
            uint8_t  mm   = v.substring(10, 12).toInt();
            uint8_t  ss   = v.substring(12, 14).toInt();
            rtcSetTime(year, mon, day, hh, mm, ss);
            char ts[24];
            rtcNowString(ts, sizeof(ts));
            notifyReply("OK:TIME=" + String(ts));
        } else {
            notifyReply("ERR:SETTIME");
        }

    } else if (cmd.startsWith("INTERVAL:")) {
        long n = cmd.substring(9).toInt();
        if (n >= (long)INTERVAL_MIN_MIN && n <= (long)INTERVAL_MIN_MAX) {
            g_measure_interval_min = (uint32_t)n;
            settingsSaveInterval();
            notifyReply("OK:INTERVAL=" + String((unsigned long)g_measure_interval_min));
        } else {
            notifyReply("ERR:INTERVAL");
        }

    } else if (cmd.startsWith("AVGN:")) {
        long n = cmd.substring(5).toInt();
        if (n >= AVG_N_MIN && n <= AVG_N_MAX) {
            g_avg_n = (uint8_t)n;
            settingsSaveAvgN();
            notifyReply("OK:AVGN=" + String(g_avg_n));
        } else {
            notifyReply("ERR:AVGN");
        }

    } else if (cmd.startsWith("AVGM:")) {
        long n = cmd.substring(5).toInt();
        if (n >= AVG_M_MIN && n <= AVG_M_MAX) {
            g_avg_m = (uint8_t)n;
            settingsSaveAvgM();
            notifyReply("OK:AVGM=" + String(g_avg_m));
        } else {
            notifyReply("ERR:AVGM");
        }

    } else if (cmd == "START") {
        g_running = true;
        notifyReply("OK:RUN=1");

    } else if (cmd == "STOP") {
        g_running = false;
        notifyReply("OK:RUN=0");

    } else if (cmd == "TARE") {
        bool allOk = true;
        for (uint8_t ch = 0; ch < 5; ch++) {
            if (isnan(tareChannel(ch))) allOk = false;
        }
        notifyReply(allOk ? "OK:TARE" : "ERR:TARE_PARTIAL");

    } else if (cmd.startsWith("DEVID:")) {
        long n = cmd.substring(6).toInt();
        if (n >= 1 && n <= 255) {
            g_device_id = (uint8_t)n;
            settingsSaveDevId();
            // アドバタイズ名（スキャンレスポンス）を新しいIDで反映し直す
            char bleName[24];
            snprintf(bleName, sizeof(bleName), "Monita-%02X", g_device_id);
            NimBLEAdvertisementData scanResponseData;
            scanResponseData.setName(bleName);
            NimBLEDevice::getAdvertising()->setScanResponseData(scanResponseData);
            notifyReply("OK:DEVID=" + String(g_device_id));
        } else {
            notifyReply("ERR:DEVID");
        }

    } else if (cmd == "DUMP") {
        handleDumpCommand();

    } else {
        notifyReply("ERR:UNKNOWN");
    }
}

class MonitaServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer) override {
        g_bleControllerConnected = true;
        Serial.println("[BLE] コントローラー接続");
    }
    void onDisconnect(NimBLEServer* pServer) override {
        g_bleControllerConnected = false;
        Serial.println("[BLE] コントローラー切断 → 再アドバタイズ");
        NimBLEDevice::startAdvertising();
    }
};

class MonitaRxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic) override {
        std::string value = pCharacteristic->getValue();
        if (value.empty()) return;
        handleCommand(String(value.c_str()));
    }
};

// BLEアドバタイズのMSD(Manufacturer Specific Data)に計測値を載せて常時ブロードキャストしつつ、
// GATTサーバー(NUS)も常設してコントローラーからのコマンド接続を受け付ける。
static void commInit() {
    char bleName[24];
    snprintf(bleName, sizeof(bleName), "Monita-%02X", g_device_id);
    NimBLEDevice::init(bleName);
    NimBLEDevice::setMTU(BLE_PREFERRED_MTU);

    NimBLEServer* pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MonitaServerCallbacks());

    NimBLEService* pService = pServer->createService(NUS_SERVICE_UUID);
    NimBLECharacteristic* pRx = pService->createCharacteristic(NUS_CHAR_RX_UUID, NIMBLE_PROPERTY::WRITE);
    pRx->setCallbacks(new MonitaRxCallbacks());
    g_pTxCharacteristic = pService->createCharacteristic(NUS_CHAR_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
    pService->start();

    // 主アドバタイズパケット（31バイト上限）はManufacturer Data(20B)でほぼ埋まるため、
    // デバイス名はスキャンレスポンス（別枠31バイト）に載せる。
    // nRF Connect等のアクティブスキャナーはスキャンレスポンスも受信するため、
    // ここで一度設定しておけば bleAdvertiseMeasurement() 内の
    // setAdvertisementData()（主パケットのみ更新）に上書きされず維持される。
    NimBLEAdvertisementData scanResponseData;
    scanResponseData.setName(bleName);
    NimBLEDevice::getAdvertising()->setScanResponseData(scanResponseData);

    Serial.printf("[BLE] 初期化完了: %s（GATT NUSサーバー常設）\n", bleName);
}
static void commSendPayload(const uint8_t* data, size_t len) { (void)data; (void)len; }
#elif defined(COMM_USE_WIFI)
// WiFi接続 → 接続できたらNTPで時刻同期。送信本体（HTTP POST）はTODO。
static void wifiSyncTimeFromNTP() {
    Serial.println("[NTP] 時刻同期開始...");
    configTime(NTP_GMT_OFFSET_SEC, NTP_DAYLIGHT_OFFSET_SEC, NTP_SERVER1, NTP_SERVER2);

    struct tm tmNow;
    uint32_t deadline = millis() + NTP_SYNC_TIMEOUT_MS;
    while (!getLocalTime(&tmNow, 100)) {
        if (millis() > deadline) {
            Serial.println("[NTP] 同期タイムアウト → 内蔵RTCの現在値を維持");
            return;
        }
    }
    g_rtc_set = true;  // NTPで正しい時刻が settimeofday 済み（getLocalTimeが内部で反映）

    char ts[24];
    rtcNowString(ts, sizeof(ts));
    Serial.printf("[NTP] 同期完了: %s\n", ts);
}

static void commInit() {
    Serial.printf("[WiFi] 接続中: %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        delay(200);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[OK] WiFi接続: "); Serial.println(WiFi.localIP());
        wifiSyncTimeFromNTP();
    } else {
        Serial.println("[WARN] WiFi接続タイムアウト → 時刻同期スキップ、内蔵RTCの現在値を維持");
    }
}
// TODO: HTTPClientでSERVER_URLへPOST
static void commSendPayload(const uint8_t* data, size_t len) { (void)data; (void)len; }
#elif defined(COMM_USE_SERIAL)
// デバッグ用。実送信はせず、シリアルモニタに出すだけ（loop()側で計測値を表示済みのためここでは何もしない）
static void commInit()          { Serial.println("[COMM] シリアルデバッグモード（送信なし）"); }
static void commSendPayload(const uint8_t* data, size_t len) { (void)data; (void)len; }
#endif

#if defined(COMM_USE_BLE)
// ============================================================================
// BLE アドバタイズ（COMM_USE_BLE時）
//
// MSD フォーマット（20バイト、project07_NEXCO/firmware_child と同じ考え方を8ch分に拡張）:
//   [0-1]   Company ID  : 0xFF 0xFF（Bluetooth SIG未割当のテスト用領域）
//   [2]     Pkt type    : BLE_PKT_TYPE（0x11 = project06_yokogawa/gateway_v1.1のEXPECTED_PKT_TYPEに合わせる）
//   [3]     Device ID   : g_device_id
//   [4-5]   CH1 ひずみ/変位 : int16 LE（µε相当、NaN時は0x7FFF）
//   [6-7]   CH2             : int16 LE
//   [8-9]   CH3             : int16 LE
//   [10-11] CH4             : int16 LE
//   [12-13] CH5             : int16 LE
//   [14-15] CH6 熱電対       : int16 LE（0.1℃単位、NaN時は0x7FFF）
//   [16-17] CH7 電圧         : int16 LE（mV単位）
//   [18-19] CH8 電圧         : int16 LE（mV単位）
// 合計20バイト（Legacy ADV 31バイト上限に十分収まる）
// ============================================================================
static void bleWriteI16(uint8_t* buf, int idx, int32_t val) {
    int16_t v = (int16_t)val;
    buf[idx]     = (uint8_t)(v & 0xFF);
    buf[idx + 1] = (uint8_t)((v >> 8) & 0xFF);
}

static void bleAdvertiseMeasurement(const Measurement& m) {
    uint8_t buf[20];
    buf[0] = BLE_COMPANY_ID_LO;
    buf[1] = BLE_COMPANY_ID_HI;
    buf[2] = BLE_PKT_TYPE;
    buf[3] = g_device_id;

    for (uint8_t ch = 0; ch < 5; ch++) {
        int32_t v = m.hx_ok[ch] ? (int32_t)lroundf(m.hx_phys[ch]) : 0x7FFF;
        bleWriteI16(buf, 4 + ch * 2, v);
    }
    bleWriteI16(buf, 14, m.ch6_ok ? (int32_t)lroundf(m.ch6_tempC * 10.0f) : 0x7FFF);
    bleWriteI16(buf, 16, m.ch7_ok ? (int32_t)lroundf(m.ch7_V * 1000.0f) : 0x7FFF);
    bleWriteI16(buf, 18, m.ch8_ok ? (int32_t)lroundf(m.ch8_V * 1000.0f) : 0x7FFF);

    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->stop();
    NimBLEAdvertisementData advData;
    advData.setManufacturerData(std::string((char*)buf, sizeof(buf)));
    pAdv->setAdvertisementData(advData);
    pAdv->setMinInterval((BLE_ADV_INTERVAL_MS * 1000) / 625);  // 0.625ms単位
    pAdv->setMaxInterval((BLE_ADV_INTERVAL_MS * 1000) / 625);
    pAdv->start();

    // シリアルモニタでペイロード内容を確認できるようにhexダンプ
    Serial.print("[BLE] MSD payload (");
    Serial.print(sizeof(buf));
    Serial.print("B): ");
    for (uint8_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] < 0x10) Serial.print('0');
        Serial.print(buf[i], HEX);
        Serial.print(' ');
    }
    Serial.println();
    Serial.printf("[BLE]  CompanyID=0x%02X%02X PktType=0x%02X DeviceID=0x%02X\n",
                  buf[1], buf[0], buf[2], buf[3]);
    Serial.printf("[BLE]  CH1=%d CH2=%d CH3=%d CH4=%d CH5=%d  CH6=%.1fC  CH7=%dmV CH8=%dmV\n",
                  (int16_t)(buf[4]  | (buf[5]  << 8)),
                  (int16_t)(buf[6]  | (buf[7]  << 8)),
                  (int16_t)(buf[8]  | (buf[9]  << 8)),
                  (int16_t)(buf[10] | (buf[11] << 8)),
                  (int16_t)(buf[12] | (buf[13] << 8)),
                  (int16_t)(buf[14] | (buf[15] << 8)) / 10.0f,
                  (int16_t)(buf[16] | (buf[17] << 8)),
                  (int16_t)(buf[18] | (buf[19] << 8)));
}

// アドバタイズは接続確立後に止まる機材があり、その間コントローラー側の表示が更新できなくなる。
// GATT接続中は計測の度にNotifyで直接最新値+実時刻を送り、接続中でもリアルタイム表示できるようにする。
static void sendLiveUpdateIfConnected(const Measurement& m) {
    if (!g_bleControllerConnected || g_pTxCharacteristic == nullptr) return;

    char ts[24];
    rtcNowString(ts, sizeof(ts));

    char line[160];
    int n = snprintf(line, sizeof(line),
        "LIVE:CH1=%s,CH2=%s,CH3=%s,CH4=%s,CH5=%s,CH6=%s,CH7=%s,CH8=%s,TIME=%s",
        m.hx_ok[0] ? String(m.hx_phys[0], 2).c_str() : "NaN",
        m.hx_ok[1] ? String(m.hx_phys[1], 2).c_str() : "NaN",
        m.hx_ok[2] ? String(m.hx_phys[2], 2).c_str() : "NaN",
        m.hx_ok[3] ? String(m.hx_phys[3], 2).c_str() : "NaN",
        m.hx_ok[4] ? String(m.hx_phys[4], 2).c_str() : "NaN",
        m.ch6_ok   ? String(m.ch6_tempC, 1).c_str()  : "NaN",
        m.ch7_ok   ? String(m.ch7_V, 3).c_str()      : "NaN",
        m.ch8_ok   ? String(m.ch8_V, 3).c_str()      : "NaN",
        ts);
    if (n <= 0 || (size_t)n >= sizeof(line)) return;

    g_pTxCharacteristic->setValue((uint8_t*)line, (size_t)n);
    g_pTxCharacteristic->notify();
    Serial.print("[BLE] live notify: ");
    Serial.println(line);
}
#endif

#if defined(COMM_USE_WIFI)
// GASのWebアプリへGET+クエリパラメーターで送信（POST不使用の理由は commInit 手前のコメント参照）
static bool sendMeasurementToGAS(const Measurement& m) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[GAS] WiFi未接続のため送信スキップ");
        return false;
    }

    // URLに埋め込むため、スペース無しのISO8601形式（区切りを"T"）にする
    time_t now; time(&now);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    char ts[24];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tmNow);

    char url[768];
    snprintf(url, sizeof(url),
        "%s?timestamp=%s&device_id=%s"
        "&ch1=%.3f&ch2=%.3f&ch3=%.3f&ch4=%.3f&ch5=%.3f"
        "&ch6=%.2f&ch7=%.3f&ch8=%.3f",
        GAS_URL, ts, DEVICE_ID,
        m.hx_ok[0] ? m.hx_phys[0] : NAN,
        m.hx_ok[1] ? m.hx_phys[1] : NAN,
        m.hx_ok[2] ? m.hx_phys[2] : NAN,
        m.hx_ok[3] ? m.hx_phys[3] : NAN,
        m.hx_ok[4] ? m.hx_phys[4] : NAN,
        m.ch6_ok ? m.ch6_tempC : NAN,
        m.ch7_ok ? m.ch7_V : NAN,
        m.ch8_ok ? m.ch8_V : NAN
    );

    WiFiClientSecure client;
    client.setInsecure();  // 証明書検証をスキップ（検証運用。本番導入時は要見直し）

    HTTPClient http;
    http.begin(client, url);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000);

    int httpCode = http.GET();
    String response = http.getString();
    http.end();

    if (httpCode == HTTP_CODE_OK) {
        Serial.printf("[GAS] 送信成功: %s\n", response.c_str());
        return true;
    }
    Serial.printf("[GAS] 送信失敗（HTTPステータス: %d）: %s\n", httpCode, response.c_str());
    return false;
}
#endif

static void sendMeasurement(const Measurement& m) {
#if defined(COMM_USE_WIFI)
    sendMeasurementToGAS(m);
#elif defined(COMM_USE_BLE)
    bleAdvertiseMeasurement(m);
#else
    (void)m;
#endif
}

// ============================================================================
// setup / loop
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("=== Monita ver1.1 本番用 ===");

    if (!g_rtc_set) rtcApplyDefault();

    g_sdMutex = xSemaphoreCreateMutex();

    settingsLoad();
    Serial.printf("[NVS] interval=%lumin N=%u M=%u devid=0x%02X\n",
                  (unsigned long)g_measure_interval_min, g_avg_n, g_avg_m, g_device_id);

    softI2CInit(PIN_SDA, PIN_SCL);
    delay(50);

    pinMode(HX711_PD_SCK, OUTPUT);
    pinMode(HX711_DOUT, INPUT_PULLUP);
    digitalWrite(HX711_PD_SCK, LOW);

    if (mcpInit()) Serial.println("[OK] MCP23008");
    else           Serial.println("[ERROR] MCP23008 初期化失敗");

    g_mcp9600_addr = mcp9600Scan();
    if (g_mcp9600_addr && mcp9600Init(g_mcp9600_addr)) {
        Serial.println("[OK] MCP9600 (CH6)");
    } else {
        Serial.println("[WARN] MCP9600 未検出");
    }

    if (softI2CProbe(ADS_ADDR)) Serial.println("[OK] ADS1115 (CH7/CH8)");
    else                        Serial.println("[WARN] ADS1115 未検出");

    g_sd_ok = sdInit();
    Serial.println(g_sd_ok ? "[OK] SD カード" : "[WARN] SD カード 未検出");

    commInit();

    Serial.println();
}

void loop() {
    // 起動直後は即1回目を実行し、以降はg_measure_interval_min間隔で繰り返す
    static bool firstRun = true;
    static uint32_t lastMeasure = 0;
    uint32_t now = millis();

    if (!g_running) {
        delay(50);
        return;
    }

    if (!firstRun && (now - lastMeasure < measureIntervalMs())) {
        delay(10);
        return;
    }
    firstRun = false;
    lastMeasure = now;

    Measurement m = measureAll();
    logToSD(m);
    sendMeasurement(m);
#if defined(COMM_USE_BLE)
    sendLiveUpdateIfConnected(m);
#endif

    char ts[24];
    rtcNowString(ts, sizeof(ts));
    Serial.printf("[%s] CH1=%.2f CH2=%.2f CH3=%.2f CH4=%.2f CH5=%.2f  CH6=%.2fC  CH7=%.3fV CH8=%.3fV\n",
                  ts, m.hx_phys[0], m.hx_phys[1], m.hx_phys[2], m.hx_phys[3], m.hx_phys[4],
                  m.ch6_tempC, m.ch7_V, m.ch8_V);
    if (m.ch6_openCircuit)  Serial.println("[WARN] CH6 オープン回路（熱電対未接続または断線）");
    if (m.ch6_shortCircuit) Serial.println("[WARN] CH6 ショート検出（GNDまたはVCC）");
}
