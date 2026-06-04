/**
 * Monita Flex WiFi — メインスケッチ
 *
 * 【対象ハード】
 *   Seeed XIAO ESP32-C3（回路図作成予定）
 *
 * 【役割】
 *   - HX711 最大4ch（SN74LV4052 MUX + TCA9534 A/B制御）から荷重を取得
 *   - 温度・電池電圧をアナログ取得
 *   - BLE NUS（Nordic UART Service）でMonitaControllerからの設定変更を受信
 *     - "SLP:N"  → スリープ間隔をN分に変更
 *     - "TARE"   → 全CH tare実行
 *     - "GET"    → 現在の設定値を返信
 *   - WiFiでデータ送信（実装予定）
 *   - 設定値はNVS（不揮発性ストレージ）に保存
 *
 * 【ピン割当】（回路図確定後に更新）
 *   HX711_SCK : D6
 *   HX711_DOUT: D7
 *   BATT_ADC  : A3
 *   TEMP_ADC  : A2
 *   SW_POWER  : D10（3V3_SW MOSFET）
 *   USER_BTN  : D0
 */

#include <Arduino.h>

// ============================================================
// アプリ設定
// ============================================================

#define DEBUG_MODE        1    // 1: シリアルデバッグログ有効
#define DEFAULT_SLEEP_MIN 15   // デフォルトスリープ時間（分）

// ============================================================
// ピン番号（回路図確定後に更新）
// ============================================================

#define HX711_SCK_PIN   6
#define HX711_DOUT_PIN  7
#define BATT_ADC_PIN    A3
#define TEMP_ADC_PIN    A2
#define SW_POWER_PIN    10
#define USER_BTN_PIN    0

// ============================================================
// グローバル変数
// ============================================================

static int g_sleep_minutes = DEFAULT_SLEEP_MIN;

// ============================================================
// setup / loop（今後ここに実装を追加していく）
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Monita Flex WiFi — XIAO ESP32-C3 ===");
  Serial.printf("Sleep interval: %d min\n", g_sleep_minutes);

  // TODO: NVSから設定値読み込み
  // TODO: BLE NUSサーバー初期化
  // TODO: HX711初期化
  // TODO: WiFi接続
}

void loop() {
  // TODO: 計測 → BLE/WiFi送信 → スリープ
  delay(1000);
}
