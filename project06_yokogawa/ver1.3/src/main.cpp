/**
 * Monita Flex 横河基板 ver1.3 — 本番用スケッチ（LoRa対応基板）
 *
 * 【対象ハード】
 *   XIAO ESP32-C3 + ver1.3基板（HX711×5 / 74HC4051 MUX / MCP23008 / ADS1115 / MCP9600 / SD /
 *   LoRaモジュール E220-900T22S(JP)）。ver1.1からの主な変更点は「通信方式のLoRa対応」で、
 *   これに伴いSPI_CS(SDカード用)をD7からD0へ移設し、空いたD7/D6をLoRaのUART TX/RXに割り当てた
 *   （netlist "ver1.2 (~recovered).ipc" として書き出されているが、内容はver1.3基板のもの。
 *   2026/09/03確認）。ver1.1のコード自体はこのファイルとは別に維持している（変更していない）。
 *
 * 【チャンネル構成（本番仕様）】
 *   CH1〜CH5 : HX711（MUX ch0〜4）。CH_TYPE[] でチャンネルごとに「ひずみ」「変位」を選択
 *   CH6      : 熱電対 K型（MCP9600）固定
 *   CH7, CH8 : 電圧入力（ADS1115 差動 2ch）固定
 *
 * 【使い方】
 *   1. src/main.cpp を退避する（例: main.cpp → main_debug.cpp.bak）
 *   2. このファイルを main.cpp にコピーする
 *      cp main_production.cpp.bak main.cpp
 *   3. 下記「設定項目」を編集してビルド・書き込み
 *
 * 【通信方式が COMM_USE_LORA のとき（本基板唯一の標準構成。Gateway接続は常にLoRa）】
 *   毎計測サイクルごとに、Serial1(UART, TX=D6/RX=D7、2026/09/03実機確認済み)経由でLoRaモジュールへCH1〜8実測値を
 *   透過モードで単発送信する（case01_Flex/v3.20 のLoRa実装を移植。ACK無しのため同一フレームを
 *   2回送信して冗長性を持たせる）。モジュールのM0/M1はMCP23008 GP3で共通駆動する
 *   （ver1.3基板でM0/M1をGP3に短絡配線）。v3.20にあるダウンリンク受信（設定変更コマンド）は
 *   本ファームには未移植（送信専用。必要になれば別途追加する）。
 *
 *   これとは別に、コントローラー連携用のBLE GATTサーバー(Nordic UART Service)を常設し、
 *   コントローラーからの設定変更・Tare・時刻同期・ログダンプなどのコマンドを受け付ける
 *   （接続時のみ）。LoRaモジュールはUART接続の別ハードウェアでESP32-C3内蔵のBLE無線とは
 *   独立しているため、センサ値をLoRaで送信しながらBLE制御も同時に使える
 *   （2026-09-06改修。以前は「データ送信方式」の選択でBLEを排他的に無効化していた）。
 *   コマンド一覧は下記 handleCommand() を参照。
 *
 *   ★2026-09-06: 本基板はGatewayとの接続を常にLoRaで行う前提のため、データ送信方式としての
 *   単体BLEモード（BLEアドバタイズでのセンサ値ブロードキャスト）は削除した。BLEはコントローラー
 *   連携の制御チャンネル専用として、LoRaビルドの中で常時動く形に一本化している。
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <cstring>
#include <algorithm>
#include <Preferences.h>
#include "esp_sleep.h"
#include "soft_i2c.h"

#if defined(COMM_USE_LORA)
// コントローラー連携BLE GATTサーバー用（詳細は下記GATT NUS節のコメント参照）。
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
static const uint32_t MEASURE_INTERVAL_MIN_DEFAULT = 5;  // 計測間隔（分）
static const uint8_t  AVG_N_DEFAULT = 5;   // 1回の測定あたりの平均サンプル数
static const uint8_t  AVG_M_DEFAULT = 5;   // 平均値をM回とり、その中央値(メジアン)を採用

static const uint32_t INTERVAL_MIN_MIN = 5;      // 最短5分
static const uint32_t INTERVAL_MIN_MAX = 1440;   // 最長24時間
static const uint8_t  AVG_N_MIN = 1, AVG_N_MAX = 50;
static const uint8_t  AVG_M_MIN = 1, AVG_M_MAX = 25;  // g_hxSamples[] のサイズと連動

// ---- RTC時刻設定 ----
// 本基板には専用RTCチップ（DS3231等）は搭載されていない（netlist確認済み・2026/08時点）。
// ESP32内蔵RTC（time.h）をソフトウェアで運用する。
//   - 工場出荷時 / 初回書き込み時はコンパイル時刻を初期値として設定する
//   - COMM_USE_LORAビルドはWiFiを持たずNTP同期できないため、現場ではコントローラーから
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
// offset[ch] はチャンネルごとのゼロ点補正値。デフォルト0、TAREコマンドで更新しNVSに保存する。
//
// ★2026-09-15: 納品機0001基板でのひずみ発生装置による実測（100〜5000µε、CH1・2・4・5）で、
// 684だと全域で一貫して約-1.0〜-1.2%低く出ることを確認。CH1・2・4・5は684×0.989≈676〜677に
// 綺麗に収束したため、676へ変更した。CH3のみ約-2.1%（最適値は約669〜670）と他chより
// 明確に外れているが、再測定でも再現した安定した個体差（センサーのゲージファクターばらつき
// の範囲内）であり、故障・配線ミスの兆候ではない。CH1〜5は本定数を共有しているため、
// 676への変更でCH3の誤差は-2.1%→-0.9%程度まで改善するが完全には解消しない
// （詳細: 02_案件/project06_yokogawa/260915_ver1.3_納品機_動作検証記録.md）。
// この定数は全ビルド共通（デバイスごとの個別値は持てない）ため、他基板（0002等）に
// そのまま使う場合は同様の実機検証を別途行うこと。
static constexpr float STRAIN_DISP_COEFF = 676.0f;

// ---- SDカード ----
#define SD_LOG_ENABLED_DEFAULT true   // 起動時デフォルトでSD保存を有効にする

// ---- 通信方式選択 ----
// platformio.ini の build_flags で以下のいずれか1つを指定する:
//   -D COMM_USE_LORA    … LoRa送信モード（E220-900T22S(JP)搭載、本基板ver1.3の標準構成。
//                          コントローラー連携用のBLE GATTサーバーも同時に常設する）
//   -D COMM_USE_WIFI    … WiFi送信モード
//   -D COMM_USE_SERIAL  … デバッグ用。送信せずシリアルモニタに計測結果を出力するだけ
// ★2026-09-06: 本基板はGatewayとの接続を常にLoRaで行う前提のため、データ送信方式としての
// 単体BLEモード（COMM_USE_BLE）は削除した。BLEはコントローラー連携の制御チャンネル専用として
// COMM_USE_LORAビルドの中に統合している（詳細はファイル冒頭のコメント参照）。
#if !defined(COMM_USE_WIFI) && !defined(COMM_USE_LORA) && !defined(COMM_USE_SERIAL)
#error "platformio.ini の build_flags に -D COMM_USE_LORA / -D COMM_USE_WIFI / -D COMM_USE_SERIAL のいずれかを指定してください"
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

#if defined(COMM_USE_LORA)
// ---- GATT NUS (コントローラーからのコマンド受信用) ----
// monita-controller (project06_yokogawa) 側と同一のUUIDを使用。
// センサ値の送信はLoRaで行うが、コントローラーとの制御チャンネル（TARE・計測間隔変更等）は
// BLE GATTサーバーとして常設する（ファイル冒頭のコメント参照）。
static const char* NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char* NUS_CHAR_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";  // コントローラー→本機
static const char* NUS_CHAR_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";  // 本機→コントローラー
static const uint16_t BLE_PREFERRED_MTU = 247;
static const size_t   DUMP_CHUNK_SIZE   = 180;  // BLE_PREFERRED_MTU-3 以下に収める

// ---- BLE簡易ブロードキャスト（発見・値表示用、2026-09-10復活） ----
// monita-controller は接続不要のパッシブスキャンで主パケットのManufacturer Specific
// Data(MSD)を直接パースしてデバイス一覧・実測値を表示する実装になっている
// （project06_yokogawa/monita-controller/src/main.cpp のMonitaAdvertisedDeviceCallbacks参照）。
// データの実体（クラウドへの送信）はLoRaが担うが、この一覧表示・簡易値確認のためだけに、
// 計測サイクルごとにLoRa送信と並行してこのMSDも主パケットへ乗せる（GATT接続とは独立）。
// フォーマットはコントローラー側の期待値と完全一致させる必要がある（CompanyID・PktType固定）。
static const uint8_t  BLE_COMPANY_ID_LO = 0xFF;
static const uint8_t  BLE_COMPANY_ID_HI = 0xFF;
static const uint8_t  BLE_PKT_TYPE      = 0x11;  // monita-controllerのMONITA_PKT_TYPEと一致させる
static const uint32_t BLE_ADV_INTERVAL_MS = 1000;
#endif

// ============================================================================
// ピン定義（ver1.3 netlist "ver1.2 (~recovered).ipc" 確定値、2026/09/03確認）
// ============================================================================
#define HX711_PD_SCK  D1
#define HX711_DOUT    D2
#define PIN_SDA       D4
#define PIN_SCL       D5
#define SD_SCK        D8
#define SD_MISO       D9
#define SD_MOSI       D10

// ver1.3: LoRa追加のためSPI_CS(SD_CS)をD7からD0(GPIO2、ESP32-C3ブートストラップピン)へ移設
// （netlist上もJP7-1ネット名"SPI_CS"がD0物理ピンに一致することを確認済み）。
// D0は起動時HIGH固定が必要（ブートモード判定）だが、SPI_CSも非選択時はHIGHが望ましいため
// 要件が一致する。基板上のR6(プルアップ)がストラッピング要件とSPI_CS要件を1本で兼ねる。
// 空いたD7/D6はLoRaモジュールのTX/RXに割り当てる（下記 COMM_USE_LORA 節、JP25で確認済み）。
#define SD_CS         D0

#if defined(COMM_USE_LORA)
// LoRaモジュール(E220-900T22S(JP))とのUART接続。JP25: pin4=D7, pin5=D6, pin6/7=GP3(M0/M1共通)。
// ★2026/09/03実機確認: 当初ネットリストのXIAO側ピン名からD7=TX/D6=RXと推測したが、
// 実機でconfig read応答が一切無かった（UART無応答）。TX/RXを入れ替えたところ
// config read 一致・TX送信まで正常動作を確認。モジュール側TXD/RXDとの対応が推測と
// 逆だったと判断し、下記の割り当てを正としている。
#define LORA_TX_PIN   D6
#define LORA_RX_PIN   D7
static const uint32_t LORA_UART_BAUD = 9600;  // E220-900T22S(JP) デフォルト
#endif

// ver1.2: E+励起電圧ライン（2V）のON/OFFスイッチ用MOSFET(2SJ496)のGate駆動ピン。
// 回路: 3V3→1N5818→2SJ496(P-ch, Gate=D3, R7でSourceへプルアップ)→MCP1700(2V)→E+バス(CH1-5)
// Pチャネルのため active-low：D3=LOWでON、D3=HIGH(またはHi-Z)でOFF（R7プルアップによりデフォルトOFF）
#define EPLUS_SW_PIN  D3

// MUX切替後、HX711読み出し開始までの待ち時間（接触不良/信号なまり対策で調整）
static const uint16_t MUX_SETTLE_MS = 50;

// E+をONにしてから計測を始めるまでの待ち時間（MCP1700の起動・ブリッジ電流の安定待ち）
static const uint16_t EPLUS_SETTLE_MS = 30;

static bool g_eplus_on = false;

static void eplusOn() {
    if (g_eplus_on) return;
    digitalWrite(EPLUS_SW_PIN, LOW);   // Gate=LOW → Pチャネル ON
    delay(EPLUS_SETTLE_MS);
    g_eplus_on = true;
}

static void eplusOff() {
    digitalWrite(EPLUS_SW_PIN, HIGH);  // Gate=HIGH → OFF（R7プルアップと合わせデフォルトOFFにもなる）
    g_eplus_on = false;
}

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

#if defined(COMM_USE_LORA)
static constexpr uint8_t MCP_IODIR_VAL = 0xF0;  // GP0-2=MUX / GP3=LoRa M0・M1 共通駆動、いずれも出力
static constexpr uint8_t MCP_BIT_LORA_M0M1 = 3;
#else
static constexpr uint8_t MCP_IODIR_VAL = 0xF8;  // GP0/GP1/GP2 = 出力
#endif

static bool mcpWrite(uint8_t reg, uint8_t val) { return softI2CWriteReg(MCP23008_ADDR, reg, val); }

// GPIOレジスタは全ビット一括書き込みのため、MUX選択(bit0-2)とLoRa M0/M1(bit3)が
// 互いを上書きしないよう、直近の書き込み値をシャドウ保持してビット単位で読み書きする。
static uint8_t g_mcp_gpio_shadow = 0x00;

static bool mcpGpioWrite(uint8_t val) {
    if (!mcpWrite(MCP_GPIO, val)) return false;
    g_mcp_gpio_shadow = val;
    return true;
}

static bool mcpGpioSetBit(uint8_t bit, bool high) {
    uint8_t val = high ? (uint8_t)(g_mcp_gpio_shadow | (1u << bit))
                        : (uint8_t)(g_mcp_gpio_shadow & ~(1u << bit));
    return mcpGpioWrite(val);
}

static bool mcpInit() {
    if (!mcpWrite(MCP_IODIR, MCP_IODIR_VAL)) return false;
    return mcpGpioWrite(0x00);
}

// ch(0〜4) → 74HC4051物理ピン: 0=Y0(pin13) 1=Y1(pin14) 2=Y2(pin15) 3=Y3(pin12) 4=Y4(pin1)
//
// 実配線（ver1.2 PCBネットリストで確認、2026/08/10）: GP0→S0, GP1→S1, GP2→S2（クロスなし直結）。
// 以前はブレッドボード実測を根拠にGP2→S0/GP0→S2という逆順で書いていたが、これはブレッドボードの
// 手配線とPCBの配線が異なっていたため。逆順のままだとchの3bitが反転されて伝わり、
// ch=1↔4(CH2⇔CH5)が入れ替わって見え、ch=3(CH4)は未接続のY6が選ばれてしまい常に無応答になっていた。
static bool muxSelect(uint8_t ch) {
    // bit0(S0)→GP0, bit1(S1)→GP1, bit2(S2)→GP2。bit3(LoRa M0/M1)は保持する。
    uint8_t val = (uint8_t)((g_mcp_gpio_shadow & ~0x07u) | (ch & 0x07u));
    return mcpGpioWrite(val);
}

// CH番号(0=CH1〜4=CH5) → MUX ch(0〜4) の対応表。
// muxSelect()のビット順バグを補正するための入れ替えだったため、バグ修正に伴い恒等対応に戻した
// （2026/08/10）。
static const uint8_t CH_TO_MUX[5] = { 0, 1, 2, 3, 4 };  // CH1,CH2,CH3,CH4,CH5

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
static constexpr uint8_t  ADS_REG_CFG  = 0x01;    // ADS1115 Configレジスタの固定アドレス（ハードウェア仕様値）
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
// 「ファームをビルド・書き込みした日時」を暫定の初期値として使う（固定値2026/01/01よりは実態に近い）。
// ★2026-09-12: 以前は__DATE__/__TIME__（main.cppのソースを実際に再コンパイルした瞬間の値。
// ソースに変更が無いとpio runしても更新されず、一見「時刻埋め込みが壊れている」ように見えて
// 混乱した経緯がある）を使っていたが、platformio.iniのbuild_flagsでpio run/upload実行の
// たびに必ず新しく評価されるFIELD_BUILD_EPOCH（ビルドマシンのローカル時刻からのnaive epoch秒。
// rtcSetTime()と同じ「TZ変換なし」の流儀）に置き換えた。
static void rtcApplyDefault() {
#if defined(FIELD_BUILD_EPOCH)
    time_t t = (time_t)FIELD_BUILD_EPOCH;
#else
    struct tm tm0 = {};
    tm0.tm_year = RTC_DEFAULT_YEAR - 1900;
    tm0.tm_mon  = RTC_DEFAULT_MONTH - 1;
    tm0.tm_mday = RTC_DEFAULT_DAY;
    tm0.tm_hour = RTC_DEFAULT_HOUR;
    tm0.tm_min  = RTC_DEFAULT_MIN;
    tm0.tm_sec  = RTC_DEFAULT_SEC;
    time_t t = mktime(&tm0);
#endif
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

// ★2026-09-12: LoRa/BLEへ送る「実測epoch」と、シリアルモニタ表示・SDログの時刻が、
// それぞれ別のタイミングでtime()を読み直していたため、LoRa送信直後の待ち時間
// （LORA_TX_REPEATの完了待ち＋DOWNLINK_RX_WINDOW_MSの約3.2秒）の分だけ、
// スプレッドシート（LoRaのepochが由来）とシリアルモニタ（表示直前に読み直した時刻）が
// 常時ズレて見える事象があった。Measurement::epochを計測直後に1回だけ確定させ、
// 送信（LoRa/BLE）・シリアルモニタ表示・SDログの全てで同じ値を使うことで解消する。
static void epochToString(uint32_t epoch, char* out, size_t outLen) {
    time_t t = (time_t)epoch;
    struct tm tmNow;
    localtime_r(&t, &tmNow);
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
    uint32_t epoch;  // この計測が完了した瞬間のepoch。LoRa/BLE送信・シリアル表示・SDログで共通して使う
};

static Measurement measureAll() {
    Measurement m = {};

    // E+（ブリッジ励起2V）はHX711計測中だけON。ブリッジ電流(120Ω負荷で十数mA)を
    // 計測時以外は流さないことで消費電力を抑える。
    eplusOn();
    for (uint8_t ch = 0; ch < 5; ch++) {
        muxSelect(CH_TO_MUX[ch]);
        delay(MUX_SETTLE_MS);
        float raw = hx711ReadMedianOfAverages();
        m.hx_ok[ch]   = !isnan(raw);
        m.hx_raw[ch]  = raw;
        m.hx_phys[ch] = m.hx_ok[ch] ? hx711ToPhysical(raw, ch) : NAN;
    }
    eplusOff();

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

    // ★2026-09-12: 計測完了直後にepochを1回だけ確定させる（詳細はepochToString()のコメント参照）。
    {
        time_t t; time(&t);
        m.epoch = (uint32_t)t;
    }

    return m;
}

// 現在のMUX位置のままチャンネルchを再計測し、その生値をゼロ点として記録する（TAREコマンド用）
// ★2026-09-12: 以前はhx711ReadAveraged()（N回平均のみ、外れ値フィルタなし）でゼロ点を
// 決めていたが、通常計測（measureAll()）はhx711ReadMedianOfAverages()（N回平均をM回繰り返し
// 中央値を採用、外れ値に強い）を使っており、両者の耐ノイズ性が不一致だった。センサー未接続の
// フローティング入力等でTARE時に瞬間的なノイズを拾うと、そのままゼロ点として固定され、通常
// 計測（ノイズ除去済み）との差分が常時出続ける事象が実機で確認された。同じ読み取り方式に揃える。
static float tareChannel(uint8_t ch) {
    eplusOn();
    muxSelect(CH_TO_MUX[ch]);
    delay(MUX_SETTLE_MS);
    float raw = hx711ReadMedianOfAverages();
    eplusOff();
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
        epochToString(m.epoch, ts, sizeof(ts));  // ★2026-09-12: m.epoch使用（詳細はepochToString()参照）

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
// 通信（WiFi / LoRa / シリアルデバッグ）
// ============================================================================
#if defined(COMM_USE_LORA)
// ============================================================================
// コントローラー連携 BLE GATTサーバー（NUS）
// ============================================================================
// センサ値の送信方式（LoRa/WiFi/Serial）とは独立して、LoRaビルドでは常設する
// （詳細は冒頭のGATT NUS定数のコメント参照）。

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

// GATTサーバー(NUS)を初期化し、コントローラーからのコマンド接続を受け付ける状態にする。
// センサ値の送信方式によらず、LoRaビルドでは常時呼ぶ。
static void controllerBleInit() {
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

    // ★2026-09-10: monita-controllerはパッシブスキャンで主パケットのMSDだけを見ており、
    // スキャンレスポンス（サービスUUID等）は一切見えないことが判明。デバイス名は
    // スキャンレスポンス側に載せる（主パケットの20バイトMSDとは容量的に共存できないため。
    // 主パケットの中身自体はbleAdvertiseMeasurement()が計測サイクルごとに更新する）。
    NimBLEAdvertisementData scanResponseData;
    scanResponseData.setName(bleName);
    NimBLEDevice::getAdvertising()->setScanResponseData(scanResponseData);
    NimBLEDevice::getAdvertising()->start();

    Serial.printf("[BLE] コントローラー連携GATTサーバー初期化完了: %s\n", bleName);
}

// ============================================================================
// BLE簡易ブロードキャスト（発見・値表示用、2026-09-10復活）
//
// MSD フォーマット（20バイト。monita-controllerのMonitaAdvertisedDeviceCallbacksと
// 完全一致させる必要がある）:
//   [0-1]   Company ID  : 0xFF 0xFF（Bluetooth SIG未割当のテスト用領域）
//   [2]     Pkt type    : BLE_PKT_TYPE（0x11固定、コントローラー側のMONITA_PKT_TYPEと一致）
//   [3]     Device ID   : g_device_id
//   [4-5]   CH1 ひずみ/変位 : int16 LE（µε相当、NaN時は0x7FFF）
//   [6-7]   CH2             : int16 LE
//   [8-9]   CH3             : int16 LE
//   [10-11] CH4             : int16 LE
//   [12-13] CH5             : int16 LE
//   [14-15] CH6 熱電対       : int16 LE（0.1℃単位、NaN時は0x7FFF）
//   [16-17] CH7 電圧         : int16 LE（mV単位）
//   [18-19] CH8 電圧         : int16 LE（mV単位）
//   [20-23] 計測時刻(Epoch)  : uint32 LE（本機RTCのUNIXエポック秒。コントローラーが
//            未接続時でも計測時刻の絶対値を表示できるようにするため、2026-09-12追加）
// 合計24バイト（主パケット31バイト上限に収まる。デバイス名等の他要素は
// スキャンレスポンス側に分離済み、controllerBleInit()参照）。
// ============================================================================
static void bleWriteI16(uint8_t* buf, int idx, int32_t val) {
    int16_t v = (int16_t)val;
    buf[idx]     = (uint8_t)(v & 0xFF);
    buf[idx + 1] = (uint8_t)((v >> 8) & 0xFF);
}

static void bleWriteU32(uint8_t* buf, int idx, uint32_t val) {
    buf[idx]     = (uint8_t)(val & 0xFF);
    buf[idx + 1] = (uint8_t)((val >> 8) & 0xFF);
    buf[idx + 2] = (uint8_t)((val >> 16) & 0xFF);
    buf[idx + 3] = (uint8_t)((val >> 24) & 0xFF);
}

static void bleAdvertiseMeasurement(const Measurement& m) {
    uint8_t buf[24];
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
    // ★2026-09-12: LoRa送信と同じくm.epoch（計測完了時に1回だけ確定した値）を使う
    // （詳細はsendMeasurementToLoRa()内の同種コメント参照）。
    bleWriteU32(buf, 20, m.epoch);

    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->stop();
    NimBLEAdvertisementData advData;
    advData.setManufacturerData(std::string((char*)buf, sizeof(buf)));
    pAdv->setAdvertisementData(advData);
    pAdv->setMinInterval((BLE_ADV_INTERVAL_MS * 1000) / 625);  // 0.625ms単位
    pAdv->setMaxInterval((BLE_ADV_INTERVAL_MS * 1000) / 625);
    pAdv->start();

    Serial.print("[BLE] MSD payload (");
    Serial.print(sizeof(buf));
    Serial.print("B): ");
    for (uint8_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] < 0x10) Serial.print('0');
        Serial.print(buf[i], HEX);
        Serial.print(' ');
    }
    Serial.println();
}
#endif  // defined(COMM_USE_LORA)

#if defined(COMM_USE_WIFI)
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
#elif defined(COMM_USE_LORA)
// LoRaモジュール(E220-900T22S(JP))のUART初期化に加え、コントローラー連携BLE GATTサーバーも
// 起動する（2026-09-06改修。データ送信はLoRaだが、TARE・計測間隔変更等の制御チャンネルは
// BLEのまま使えるようにするため）。設定レジスタの確認・書込は選択肢A方式（毎送信サイクルで
// READ→不一致ならWRITE）のため sendMeasurementToLoRa() 側で行う。
static void commInit() {
    controllerBleInit();

    // ★2026-09-10: controllerBleInit()の時点では主パケットにMSDが一切載っておらず、
    // 起動直後（I2C初期化・SD初期化・最初の計測が終わるまでの数百ms〜数秒）はコントローラーの
    // パッシブスキャンに一切ひっかからない空白期間になっていた。これが「電源ONから見つかる
    // までのタイミングが不安定」の一因と考えられるため、実測前のプレースホルダ値（全CH NaN
    // 相当=0x7FFF）でひとまずMSDを送出し、起動直後から発見できるようにする。
    Measurement placeholder = {};
    bleAdvertiseMeasurement(placeholder);

    Serial1.begin(LORA_UART_BAUD, SERIAL_8N1, LORA_RX_PIN, LORA_TX_PIN);
    delay(500);  // E220 起動待ち（暫定値、case01_Flex/v3.20 と同じ値）
    Serial.println("[LORA] UART初期化完了");
}
static void commSendPayload(const uint8_t* data, size_t len) { (void)data; (void)len; }
#elif defined(COMM_USE_SERIAL)
// デバッグ用。実送信はせず、シリアルモニタに出すだけ（loop()側で計測値を表示済みのためここでは何もしない）
static void commInit()          { Serial.println("[COMM] シリアルデバッグモード（送信なし）"); }
static void commSendPayload(const uint8_t* data, size_t len) { (void)data; (void)len; }
#endif

#if defined(COMM_USE_LORA)
// アドバタイズは接続確立後に止まる機材があり、その間コントローラー側の表示が更新できなくなる
// （LoRaビルドではそもそもセンサ値を載せた主アドバタイズを更新していない）。
// GATT接続中は計測の度にNotifyで直接最新値+実時刻を送り、接続中でもリアルタイム表示できるようにする。
static void sendLiveUpdateIfConnected(const Measurement& m) {
    if (!g_bleControllerConnected || g_pTxCharacteristic == nullptr) return;

    char ts[24];
    epochToString(m.epoch, ts, sizeof(ts));  // ★2026-09-12: m.epoch使用（詳細はepochToString()参照）

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

#if defined(COMM_USE_LORA)
// ============================================================================
// LoRa（E220-900T22S(JP)、COMM_USE_LORA時）
// case01_Flex/v3.20 の実装を移植。相違点は下記のみ:
//   - M0/M1の駆動先がTCA9534 P2ではなくMCP23008 GP3（mcpGpioSetBit経由）
//   - UARTがSerial1(D7=TX/D6=RX、ESP32 Arduinoの明示的ピン指定API)
//   - ペイロードが4CH+電池電圧ではなく8CH（HX711×5・熱電対・電圧×2）
//   - PktTypeは0x04ではなく0x12（BLE_PKT_TYPE=0x11の横河ファミリーで次の空き番号）
//   - v3.20のダウンリンク受信（設定変更コマンド）は未移植（送信専用）
// レジスタ配置・チェックサム・冗長送信のロジックは実機確認済みの値をそのまま踏襲する。
// ============================================================================
#define LORA_MODE_SWITCH_DELAY_MS 100U  // M0/M1切替後の安定待ち（v3.20と同じ値）
#define LORA_CFG_REG_START 0x00
#define LORA_CFG_REG_LEN   6            // ADDH..OPTION (0x00-0x05)

// 想定設定値（v3.20で実機疎通確認済みの値をそのまま使用。Gatewayとの互換のため統一）
static const uint8_t LORA_CFG_ADDH = 0x00;
static const uint8_t LORA_CFG_ADDL = 0x00;
static const uint8_t LORA_CFG_REG0 = 0x68;  // UART9600bps + エア速度(SF7/BW125kHz)
static const uint8_t LORA_CFG_REG1 = 0x01;  // ペイロード長200B(default)/RSSIノイズ無効/送信出力13dBm
static const uint8_t LORA_CFG_REG2 = 0x00;  // チャンネル0
static const uint8_t LORA_CFG_REG3 = 0x80;  // RSSIバイト有効化ON(Gatewayとの設定統一用)/透過送信モード

static const uint8_t LORA_PKT_TYPE = 0x12;  // 横河LoRa版。値は暫定、Gateway実装確定時に見直す

// MCP23008 GP3でM0/M1を共通駆動する（ver1.3基板でM0/M1をGP3に短絡配線）。
// GP3=LOW→Mode0(通常送受信・透過モード) / GP3=HIGH→Mode3(設定モード)。AUX未接続のため
// 固定ディレイで代替する（LORA_MODE_SWITCH_DELAY_MS）。
static bool loraSetMode(bool configMode) {
    if (!mcpGpioSetBit(MCP_BIT_LORA_M0M1, configMode)) return false;
    delay(LORA_MODE_SWITCH_DELAY_MS);
    return true;
}
static inline bool loraModeNormal() { return loraSetMode(false); }
static inline bool loraModeConfig() { return loraSetMode(true); }

// 設定モード中に READ(0xC1) でレジスタ6バイトを読み出す
static bool loraReadConfig(uint8_t* out6) {
    {
        unsigned long drainStart = millis();
        while (Serial1.available()) {
            Serial1.read();
            if (millis() - drainStart > 300UL) break;
        }
    }
    Serial1.write((uint8_t)0xC1);
    Serial1.write((uint8_t)LORA_CFG_REG_START);
    Serial1.write((uint8_t)LORA_CFG_REG_LEN);

    const int respLen = 3 + LORA_CFG_REG_LEN;
    uint8_t resp[3 + LORA_CFG_REG_LEN];
    int idx = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < 500UL && idx < respLen) {
        if (Serial1.available()) {
            resp[idx++] = (uint8_t)Serial1.read();
        }
    }
    if (idx < respLen) return false;
    if (resp[0] != 0xC1) return false;
    memcpy(out6, &resp[3], LORA_CFG_REG_LEN);
    return true;
}

// 設定モード中に WRITE(0xC0, 不揮発保存) でレジスタ6バイトを書き込む
static void loraWriteConfig() {
    Serial1.write((uint8_t)0xC0);
    Serial1.write((uint8_t)LORA_CFG_REG_START);
    Serial1.write((uint8_t)LORA_CFG_REG_LEN);
    Serial1.write(LORA_CFG_ADDH);
    Serial1.write(LORA_CFG_ADDL);
    Serial1.write(LORA_CFG_REG0);
    Serial1.write(LORA_CFG_REG1);
    Serial1.write(LORA_CFG_REG2);
    Serial1.write(LORA_CFG_REG3);
    delay(200);
    unsigned long t0 = millis();
    while (millis() - t0 < 300UL) { while (Serial1.available()) Serial1.read(); }
}

static void loraPrintRegs(const char* label, const uint8_t regs[LORA_CFG_REG_LEN]) {
    Serial.print("[LORA] "); Serial.print(label); Serial.print(": ");
    for (int i = 0; i < LORA_CFG_REG_LEN; i++) {
        if (regs[i] < 0x10) Serial.print('0');
        Serial.print(regs[i], HEX);
        Serial.print(' ');
    }
    Serial.println();
}

// 送信サイクルの都度呼ぶ（選択肢A方式）。現在の設定値を確認し、想定値と異なれば書き込む。
static bool loraCheckAndConfigure() {
    if (!loraModeConfig()) return false;

    uint8_t cur[LORA_CFG_REG_LEN] = {0};
    bool readOk = loraReadConfig(cur);

    bool matches = readOk &&
        cur[0] == LORA_CFG_ADDH && cur[1] == LORA_CFG_ADDL &&
        cur[2] == LORA_CFG_REG0 && cur[3] == LORA_CFG_REG1 &&
        cur[4] == LORA_CFG_REG2 && cur[5] == LORA_CFG_REG3;

    Serial.print("[LORA] config read ");
    Serial.println(!readOk ? "失敗" : (matches ? "一致" : "不一致→書込"));
    if (readOk) {
        loraPrintRegs("実測値(読込)", cur);
        uint8_t expected[LORA_CFG_REG_LEN] = {LORA_CFG_ADDH, LORA_CFG_ADDL, LORA_CFG_REG0,
                                               LORA_CFG_REG1, LORA_CFG_REG2, LORA_CFG_REG3};
        loraPrintRegs("期待値      ", expected);
    }

    if (!readOk) {
        loraModeNormal();
        return false;
    }

    if (!matches) {
        bool verifyOk = false;
        for (int attempt = 1; attempt <= 2 && !verifyOk; attempt++) {
            loraWriteConfig();
            uint8_t verify[LORA_CFG_REG_LEN] = {0};
            bool verifyReadOk = loraReadConfig(verify);
            verifyOk = verifyReadOk &&
                verify[0] == LORA_CFG_ADDH && verify[1] == LORA_CFG_ADDL &&
                verify[2] == LORA_CFG_REG0 && verify[3] == LORA_CFG_REG1 &&
                verify[4] == LORA_CFG_REG2 && verify[5] == LORA_CFG_REG3;
            Serial.print("[LORA] config write 確認(");
            Serial.print(attempt); Serial.print("/2): ");
            Serial.println(verifyOk ? "OK" : "NG");
            if (verifyReadOk) loraPrintRegs("書込後の実測値", verify);
            else              Serial.println("[LORA] 書込後の読込自体に失敗（応答なし）");
        }
        if (!verifyOk) {
            loraModeNormal();
            return false;
        }
    }

    return loraModeNormal();
}

// 透過モードでフレームを送信する（[SYNC=0xAA][LEN][payload...][checksum]）
static void loraSendFrame(const uint8_t* msd, uint8_t msdLen) {
    uint8_t sum = (uint8_t)(0xAAU + msdLen);
    Serial1.write((uint8_t)0xAA);
    Serial1.write(msdLen);
    for (uint8_t i = 0; i < msdLen; i++) {
        Serial1.write(msd[i]);
        sum = (uint8_t)(sum + msd[i]);
    }
    Serial1.write(sum);
}

#define LORA_TX_COMPLETE_DELAY_MS 300U  // UART転送+LoRaエアタイム分の送信完了待ち（暫定値）
#define LORA_TX_REPEAT 2                // ACK無しのため同一フレームを複数回送り冗長性を持たせる
#define LORA_TX_REPEAT_GAP_MS 100U

// LoRa MSDペイロード（29バイト）。
// ★2026-09-10: CH1〜5はスプレッドシート側で小数精度が欲しいという要望を受け、
// int16(µεそのまま、丸め誤差=最大0.5µε)からint32(µε×100、小数第2位まで保持)へ拡張した。
// int16のまま桁を増やす（例: ×10）と実測で約5000µεまで出ている値が範囲(±32767)を
// 超えてオーバーフローする恐れがあったため、範囲に十分余裕のあるint32に変更している。
// CH6〜8は従来通りint16のまま（範囲的に桁を増やす余地は無いが、そもそも増やす要望も
// 無かったため現状維持）。このBLE MSD(bleAdvertiseMeasurement())とは別のバッファ・
// 別のフォーマットなので、こちらの変更はBLE側・monita-controller側には影響しない。
//   [0]     Pkt type    : LORA_PKT_TYPE
//   [1]     Device ID   : g_device_id
//   [2-5]   CH1         : int32 LE（µε×100、NaN時は0x7FFFFFFF）
//   [6-9]   CH2         : int32 LE
//   [10-13] CH3         : int32 LE
//   [14-17] CH4         : int32 LE
//   [18-21] CH5         : int32 LE
//   [22-23] CH6 熱電対   : int16 LE（0.1℃単位、NaN時は0x7FFF）
//   [24-25] CH7 電圧     : int16 LE（mV単位）
//   [26-27] CH8 電圧     : int16 LE（mV単位）
//   [28]    予備（0固定）
static void loraWriteI32(uint8_t* buf, int idx, int32_t val) {
    buf[idx]     = (uint8_t)(val & 0xFF);
    buf[idx + 1] = (uint8_t)((val >> 8) & 0xFF);
    buf[idx + 2] = (uint8_t)((val >> 16) & 0xFF);
    buf[idx + 3] = (uint8_t)((val >> 24) & 0xFF);
}

static void loraWriteI16(uint8_t* buf, int idx, int32_t val) {
    int16_t v = (int16_t)val;
    buf[idx]     = (uint8_t)(v & 0xFF);
    buf[idx + 1] = (uint8_t)((v >> 8) & 0xFF);
}

// ============================================================================
// LoRaダウンリンク受信（Gatewayからの時刻自動同期、2026-09-12追加）
// ============================================================================
// gateway_v1.2 側の sendTimeSyncDownlink() と対になる受信処理。
// 「子機の電源が入る→Gatewayとつながる→Gatewayから時刻を受け取る→計測開始」という
// フローにするため、アップリンク送信直後の短い時間だけE220からの受信を待ち、
// 自分宛の時刻同期ダウンリンクが来ていれば解析してRTCへ反映する。
// コントローラーからのSETTIME（手動入力）と両立する。両方使える状態にしておき、
// 現場ではGatewayが届く限り自動で時刻が合う（コントローラーでの手動設定は不要になる）。
//
// フレーム形式はGateway側と同一のtransparentプロトコル
// （[0xAA][LEN][payload...][checksum][RSSI]、RSSIはREG3のbit7で双方とも有効化済み）:
//   payload(15バイト): [0-1]CompanyID(0xC0DE,BE) [2]PktType(0x81) [3]宛先DeviceID
//                       [4]flags(bit0=時刻あり) [5-10]年%100/月/日/時/分/秒
//                       [11-12]sleepMin(BE,本機では未使用) [13]avg [14]median(本機では未使用)
static const uint16_t DOWNLINK_COMPANY_ID   = 0xC0DE;  // gateway_v1.2側と一致させること
static const uint8_t  DOWNLINK_PKT_TYPE     = 0x81;
static const uint8_t  DL_FLAG_TIME          = 1u << 0;
static const uint32_t DOWNLINK_RX_WINDOW_MS = 2500;    // 送信直後、この時間だけ受信を待つ
// ★2026-09-13: Gatewayが起動直後(初期設定完了後)に一定時間ブロードキャストする
// 起動ビーコン(宛先DeviceID=0xFF)向け。子機はまだ自分のDeviceIDを知らせていない
// 起動直後の段階でも、このIDへの一致だけで「自分宛」として受け取れるようにする。
static const uint8_t  DOWNLINK_BROADCAST_DEVICE_ID = 0xFF;
// 起動直後、最初の計測・送信を始める前にGatewayの起動ビーコンを待ち受ける時間。
static const uint32_t BOOT_TIME_SYNC_LISTEN_MS = 10000;

// 受信バイトを状態機械で処理し、フレーム(+RSSI)が完成するたびに中身を確認する。
// windowMsが経過するまでポーリングを続ける（複数フレーム来ても最後まで処理する）。
static void loraTryReceiveDownlink(uint32_t windowMs) {
    enum { WAIT_SYNC, WAIT_LEN, WAIT_BODY, WAIT_CKSUM, WAIT_RSSI } state = WAIT_SYNC;
    static uint8_t body[32];
    uint8_t bodyLen = 0, bodyIdx = 0, sum = 0;
    uint32_t t0 = millis();

    while (millis() - t0 < windowMs) {
        while (Serial1.available()) {
            uint8_t b = (uint8_t)Serial1.read();
            switch (state) {
                case WAIT_SYNC:
                    if (b == 0xAA) { sum = b; state = WAIT_LEN; }
                    break;
                case WAIT_LEN:
                    bodyLen = b;
                    sum = (uint8_t)(sum + b);
                    bodyIdx = 0;
                    state = (bodyLen == 0 || bodyLen > sizeof(body)) ? WAIT_SYNC : WAIT_BODY;
                    break;
                case WAIT_BODY:
                    body[bodyIdx++] = b;
                    sum = (uint8_t)(sum + b);
                    if (bodyIdx >= bodyLen) state = WAIT_CKSUM;
                    break;
                case WAIT_CKSUM:
                    state = (b == sum) ? WAIT_RSSI : WAIT_SYNC;  // 不一致は破棄して再同期
                    break;
                case WAIT_RSSI:
                    state = WAIT_SYNC;  // RSSIバイト自体は読み捨てる（本用途では不要）

                    if (bodyLen == 15 &&
                        body[0] == (uint8_t)(DOWNLINK_COMPANY_ID >> 8) &&
                        body[1] == (uint8_t)(DOWNLINK_COMPANY_ID & 0xFF) &&
                        body[2] == DOWNLINK_PKT_TYPE &&
                        (body[3] == g_device_id || body[3] == DOWNLINK_BROADCAST_DEVICE_ID)) {
                        uint8_t flags = body[4];
                        if (flags & DL_FLAG_TIME) {
                            uint16_t year = 2000 + body[5];
                            uint8_t  mon  = body[6];
                            uint8_t  day  = body[7];
                            uint8_t  hh   = body[8];
                            uint8_t  mm   = body[9];
                            uint8_t  ss   = body[10];
                            rtcSetTime(year, mon, day, hh, mm, ss);
                            Serial.printf("[LORA] Gatewayから時刻同期を受信: %04u-%02u-%02u %02u:%02u:%02u\n",
                                          (unsigned)year, mon, day, hh, mm, ss);
                        }
                    }
                    break;
            }
        }
        delay(2);
    }
}

static void sendMeasurementToLoRa(const Measurement& m) {
    if (!loraCheckAndConfigure()) {
        Serial.println("[LORA] config check失敗、今回の送信をスキップ");
        return;
    }

    uint8_t msd[33] = {0};
    msd[0] = LORA_PKT_TYPE;
    msd[1] = g_device_id;

    for (uint8_t ch = 0; ch < 5; ch++) {
        int32_t v = m.hx_ok[ch] ? (int32_t)lroundf(m.hx_phys[ch] * 100.0f) : (int32_t)0x7FFFFFFF;
        loraWriteI32(msd, 2 + ch * 4, v);
    }
    loraWriteI16(msd, 22, m.ch6_ok ? (int32_t)lroundf(m.ch6_tempC * 10.0f) : (int32_t)0x7FFF);
    loraWriteI16(msd, 24, m.ch7_ok ? (int32_t)lroundf(m.ch7_V * 1000.0f) : (int32_t)0x7FFF);
    loraWriteI16(msd, 26, m.ch8_ok ? (int32_t)lroundf(m.ch8_V * 1000.0f) : (int32_t)0x7FFF);
    // ★2026-09-12: Gateway受信時刻ではなく、フィールドユニット自身が計測した時刻を
    // そのまま「計測時刻」として使えるよう、実測epochを追加（Gateway・シリアルモニタ・
    // コントローラー表示の時刻源を統一し、数秒単位のズレを無くすため）。
    // ★同日追記: ここでtime()を読み直すと、この後のLORA_TX_REPEAT完了待ち＋
    // DOWNLINK_RX_WINDOW_MS（合計約3.2秒）の分だけシリアルモニタ表示より値が早くなり、
    // スプレッドシートとシリアルモニタが常時ズレる原因になっていた。m.epoch
    // （計測完了時に1回だけ確定した値）を使うことで、送信内容とシリアル表示を一致させる。
    bleWriteU32(msd, 29, m.epoch);

    for (uint8_t rep = 0; rep < LORA_TX_REPEAT; rep++) {
        loraSendFrame(msd, sizeof(msd));
        delay(LORA_TX_COMPLETE_DELAY_MS);
        if (rep + 1 < LORA_TX_REPEAT) delay(LORA_TX_REPEAT_GAP_MS);
    }

    Serial.print("[LORA] TX payload (");
    Serial.print(sizeof(msd));
    Serial.print("B): ");
    for (uint8_t i = 0; i < sizeof(msd); i++) {
        if (msd[i] < 0x10) Serial.print('0');
        Serial.print(msd[i], HEX);
        Serial.print(' ');
    }
    Serial.println();

    // Gatewayからの時刻同期ダウンリンクを短時間待ち受ける（上記loraTryReceiveDownlink参照）。
    loraTryReceiveDownlink(DOWNLINK_RX_WINDOW_MS);
}
#endif

static void sendMeasurement(const Measurement& m) {
#if defined(COMM_USE_WIFI)
    sendMeasurementToGAS(m);
#elif defined(COMM_USE_LORA)
    sendMeasurementToLoRa(m);
#else
    (void)m;
#endif
}

// ============================================================================
// デバッグモード: シリアルモニタからのコマンドで任意チャンネルを即時計測する
// ============================================================================
// 本基板は物理スイッチを持たないため、各センサーの検査（プロービング・荷重印加確認等）を
// シリアルモニタからその場で行えるようにする。計測間隔を待たずに即座に1回だけ計測して
// 結果を表示する（通常の周期計測（loop()内のmeasureAll()）とは独立して動作する）。
// ★2026-09-09: 当初はCOMM_USE_SERIAL専用だったが、本番ビルド（COMM_USE_LORA等）でも
// ビルドを切り替えずに現場で即座にチャンネル確認できるよう、全ビルド共通で常時有効にした。
//
// コマンド（シリアルモニタの入力欄に打って改行で送信）:
//   1〜5   CH1〜CH5（ひずみ/変位、HX711）を即時計測
//   6      CH6（熱電対、MCP9600）を即時計測
//   7      CH7（電圧、ADS1115）を即時計測
//   8      CH8（電圧、ADS1115）を即時計測
//   A      CH1〜8を全て即時計測
//   H / ?  コマンド一覧を表示
static void debugPrintHelp() {
    Serial.println("[DEBUG] コマンド: 1-5=CH1-5即時計測 6=CH6(熱電対) 7=CH7(電圧) 8=CH8(電圧) A=全CH即時計測 H/?=ヘルプ");
}

static void debugMeasureHxChannel(uint8_t ch) {
    eplusOn();
    muxSelect(CH_TO_MUX[ch]);
    delay(MUX_SETTLE_MS);
    float raw = hx711ReadMedianOfAverages();
    eplusOff();
    if (isnan(raw)) {
        Serial.printf("[DEBUG] CH%u: 読み取り失敗（HX711応答なし）\n", ch + 1);
        return;
    }
    float phys = hx711ToPhysical(raw, ch);
    Serial.printf("[DEBUG] CH%u (%s): raw=%.1f  値=%.2f\n",
                  ch + 1, chTypeLabel(CH_TYPE[ch]), raw, phys);
}

static void debugMeasureCh6() {
    if (!g_mcp9600_addr) {
        Serial.println("[DEBUG] CH6: MCP9600未検出");
        return;
    }
    float t = mcp9600ReadTemp(g_mcp9600_addr);
    if (isnan(t)) {
        Serial.println("[DEBUG] CH6: 読み取り失敗");
        return;
    }
    Serial.printf("[DEBUG] CH6 (熱電対): %.2fC\n", t);
}

static void debugMeasureCh7() {
    float v = adsReadVoltage(ADS_CFG_CH7, GAIN_CH7, OFFSET_CH7);
    if (isnan(v)) { Serial.println("[DEBUG] CH7: 読み取り失敗"); return; }
    Serial.printf("[DEBUG] CH7 (電圧): %.3fV\n", v);
}

static void debugMeasureCh8() {
    float v = adsReadVoltage(ADS_CFG_CH8, GAIN_CH8, OFFSET_CH8);
    if (isnan(v)) { Serial.println("[DEBUG] CH8: 読み取り失敗"); return; }
    Serial.printf("[DEBUG] CH8 (電圧): %.3fV\n", v);
}

static void debugMeasureAllChannels() {
    Measurement m = measureAll();
    char ts[24];
    rtcNowString(ts, sizeof(ts));
    Serial.printf("[DEBUG %s] CH1=%.2f CH2=%.2f CH3=%.2f CH4=%.2f CH5=%.2f  CH6=%.2fC  CH7=%.3fV CH8=%.3fV\n",
                  ts, m.hx_phys[0], m.hx_phys[1], m.hx_phys[2], m.hx_phys[3], m.hx_phys[4],
                  m.ch6_tempC, m.ch7_V, m.ch8_V);
}

// Serial入力を1行分読み、デバッグコマンドとして解釈する。改行が来るまでは何も実行しない
// （非ブロッキング）。loop()の先頭から毎回呼び、通常の周期計測とは独立して動作させる。
static void debugHandleSerialCommand() {
    static String buf;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (buf.length() == 0) continue;
            String cmd = buf;
            cmd.trim();
            cmd.toUpperCase();
            buf = "";

            if      (cmd == "1") debugMeasureHxChannel(0);
            else if (cmd == "2") debugMeasureHxChannel(1);
            else if (cmd == "3") debugMeasureHxChannel(2);
            else if (cmd == "4") debugMeasureHxChannel(3);
            else if (cmd == "5") debugMeasureHxChannel(4);
            else if (cmd == "6") debugMeasureCh6();
            else if (cmd == "7") debugMeasureCh7();
            else if (cmd == "8") debugMeasureCh8();
            else if (cmd == "A") debugMeasureAllChannels();
            else if (cmd == "H" || cmd == "?") debugPrintHelp();
            else Serial.printf("[DEBUG] 不明なコマンド: %s（Hでヘルプ表示）\n", cmd.c_str());
        } else {
            buf += c;
        }
    }
}

#if defined(BATTERY_MODE) && defined(COMM_USE_LORA)
// ============================================================================
// 電源管理（バッテリー駆動対応、2026-09-10改訂）
// ============================================================================
// ★当初はESP32の「自動ライトスリープ」（esp_pm_configure）を試したが、
// PlatformIOのArduino-ESP32ビルド済みライブラリはCONFIG_PM_ENABLE /
// CONFIG_FREERTOS_USE_TICKLESS_IDLEを有効化しておらず、esp_pm_configure()が
// ESP_ERR_NOT_SUPPORTED(262)を返して機能しないことを実機で確認した
// （2026-09-10、この環境でPM機能を使うにはArduinoをESP-IDFコンポーネントとして
// 再ビルドする必要があり、コストが大きい）。
//
// 本デバイスはMONITAの開発機能検証用途で、長期の現場常時計測は想定していない
// （2026-09-09、本人確認済み）ため、「コントローラーからいつでもBLE接続できる」
// 要件は不要と判断し、代わりにディープスリープ方式へ変更した。
//
// 【方式】計測・LoRa送信のたびにBLE_WAKE_WINDOW_MSだけ起きたままBLE広告を
// 続け、その間にコントローラー接続が無ければ、次の計測時刻までディープスリープする。
// ディープスリープからの復帰は電源再投入と同じ扱い（setup()から再実行、RAMは失われる）
// だが、計測間隔・平均回数等の設定はNVSに保存されているため引き継がれる。
// 接続中はスリープをスキップし、切断されるまで通常動作を続ける。
static const uint32_t BLE_WAKE_WINDOW_MS = 20000;  // 毎サイクル、この時間だけBLE接続の余地を残す

// 計測直後に呼ぶ。BLE接続が無ければ次の計測時刻までディープスリープへ入る
// （戻ってこない。復帰後はリセットと同じくsetup()から再実行される）。
// 接続中はスリープせず、そのままloop()へ戻る。
static void batterySleepIfIdle(uint32_t measureStartMs) {
    uint32_t windowStart = millis();
    while (millis() - windowStart < BLE_WAKE_WINDOW_MS) {
        debugHandleSerialCommand();
        if (g_bleControllerConnected) {
            Serial.println("[PM] コントローラー接続中のためディープスリープをスキップ");
            return;
        }
        delay(10);
    }

    uint32_t elapsedMs  = millis() - measureStartMs;
    uint32_t intervalMs = measureIntervalMs();
    uint32_t sleepMs    = (elapsedMs < intervalMs) ? (intervalMs - elapsedMs) : 1000;

    Serial.printf("[PM] ディープスリープ %lu ms（次回計測予定時刻まで）\n", (unsigned long)sleepMs);
    Serial.flush();
    esp_sleep_enable_timer_wakeup((uint64_t)sleepMs * 1000ULL);
    esp_deep_sleep_start();
    // ここには戻らない
}
#endif

// ============================================================================
// setup / loop
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("=== Monita ver1.3 本番用 ===");

    if (!g_rtc_set) rtcApplyDefault();

    g_sdMutex = xSemaphoreCreateMutex();

    settingsLoad();

#if defined(FORCE_DEVID)
    // ★工場出荷時の初期設定用。platformio.iniのbuild_flagsに-D FORCE_DEVID=<番号>を
    // 指定してビルド・書き込みした場合のみ、NVSの既存値を無視してこの番号で強制上書きする
    // （1台ごとにFORCE_DEVIDを変えて書き込む運用。設定後は通常ビルドに戻すこと）。
    if (g_device_id != (uint8_t)FORCE_DEVID) {
        g_device_id = (uint8_t)FORCE_DEVID;
        settingsSaveDevId();
        Serial.printf("[NVS] FORCE_DEVID=%u によりデバイスIDを強制上書き\n", (unsigned)FORCE_DEVID);
    }
#endif

    Serial.printf("[NVS] interval=%lumin N=%u M=%u devid=0x%02X\n",
                  (unsigned long)g_measure_interval_min, g_avg_n, g_avg_m, g_device_id);

    softI2CInit(PIN_SDA, PIN_SCL);
    delay(50);

    pinMode(HX711_PD_SCK, OUTPUT);
    pinMode(HX711_DOUT, INPUT_PULLUP);
    digitalWrite(HX711_PD_SCK, LOW);

    pinMode(EPLUS_SW_PIN, OUTPUT);
    digitalWrite(EPLUS_SW_PIN, HIGH);  // 起動直後はE+ OFF（R7プルアップと合わせて安全側）
    g_eplus_on = false;

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

#if defined(COMM_USE_LORA)
    // ★2026-09-13: 電源ON直後・最初の計測/送信を行う前に、Gatewayが起動直後2分間だけ
    // 出す起動ビーコン(宛先DeviceID=0xFF、時刻同期)を短時間待ち受ける。
    // これにより「Gateway起動→子機起動→アップリンク直後の同期」という従来の
    // 流れでは間に合わなかった、電源投入後いちばん最初の計測分から正しい時刻を
    // 使えるようにする(GatewayがAT通信中(s_atBusy)でビーコンを出せていない場合や
    // ビーコン期間(2分)を過ぎてから子機を起動した場合は、従来通りアップリンク直後の
    // 同期に委ねる。RTCはFIELD_BUILD_EPOCHで初期化済みなので受信できなくても問題ない)。
    Serial.printf("[LORA] 起動ビーコン待受: %lu ms\n", (unsigned long)BOOT_TIME_SYNC_LISTEN_MS);
    loraTryReceiveDownlink(BOOT_TIME_SYNC_LISTEN_MS);
#endif

    debugPrintHelp();

    Serial.println();
}

void loop() {
    debugHandleSerialCommand();

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
#if defined(COMM_USE_LORA)
    bleAdvertiseMeasurement(m);  // monita-controllerのパッシブスキャン向け簡易ブロードキャスト
    sendLiveUpdateIfConnected(m);
#endif

    char ts[24];
    epochToString(m.epoch, ts, sizeof(ts));  // ★2026-09-12: m.epoch使用（詳細はepochToString()参照）
    Serial.printf("[%s] CH1=%.2f CH2=%.2f CH3=%.2f CH4=%.2f CH5=%.2f  CH6=%.2fC  CH7=%.3fV CH8=%.3fV\n",
                  ts, m.hx_phys[0], m.hx_phys[1], m.hx_phys[2], m.hx_phys[3], m.hx_phys[4],
                  m.ch6_tempC, m.ch7_V, m.ch8_V);
    if (m.ch6_openCircuit)  Serial.println("[WARN] CH6 オープン回路（熱電対未接続または断線）");
    if (m.ch6_shortCircuit) Serial.println("[WARN] CH6 ショート検出（GNDまたはVCC）");

#if defined(BATTERY_MODE) && defined(COMM_USE_LORA)
    batterySleepIfIdle(now);
#endif
}
