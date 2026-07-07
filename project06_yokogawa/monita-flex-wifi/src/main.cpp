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
#include <NimBLEDevice.h>

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

// XIAO ESP32-C3にはユーザー制御可能なオンボードLEDが無いため、
// GATT疎通テスト用に外付けLED(+抵抗)を接続する想定のピン。
#define TEST_LED_PIN    3   // D1

// ============================================================
// グローバル変数
// ============================================================

static int g_sleep_minutes = DEFAULT_SLEEP_MIN;
static bool g_led_state = false;

// ============================================================
// BLE (Nordic UART Service) — MonitaControllerからの設定変更受信
// ============================================================
// AVLファーム(AVL-controller)と同一のNUS UUIDを使用。
// RX: コントローラー→本機（Write）, TX: 本機→コントローラー（Notify）
static BLEUUID serviceUUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
static BLEUUID charUUID_RX("6e400002-b5a3-f393-e0a9-e50e24dcca9e");
static BLEUUID charUUID_TX("6e400003-b5a3-f393-e0a9-e50e24dcca9e");

static NimBLECharacteristic* pTxCharacteristic = nullptr;
static bool g_ble_connected = false;

static void notifyReply(const String& msg) {
  if (pTxCharacteristic == nullptr || !g_ble_connected) return;
  pTxCharacteristic->setValue((uint8_t*)msg.c_str(), msg.length());
  pTxCharacteristic->notify();
#if DEBUG_MODE
  Serial.print("[BLE] notify: ");
  Serial.println(msg);
#endif
}

// "SLP:N" / "TARE" / "GET" を解釈して応答する
static void handleCommand(const String& raw) {
  String cmd = raw;
  cmd.trim();
#if DEBUG_MODE
  Serial.print("[BLE] recv: ");
  Serial.println(cmd);
#endif

  if (cmd.startsWith("SLP:")) {
    int n = cmd.substring(4).toInt();
    if (n > 0) {
      g_sleep_minutes = n;
      notifyReply("OK:SLP=" + String(g_sleep_minutes));
    } else {
      notifyReply("ERR:SLP");
    }
  } else if (cmd == "TARE") {
    // TODO: 実HX711接続後、全CHのtare処理を実装
    notifyReply("OK:TARE");
  } else if (cmd == "GET") {
    notifyReply("SLEEP=" + String(g_sleep_minutes));
  } else if (cmd == "LED:ON") {
    g_led_state = true;
    digitalWrite(TEST_LED_PIN, HIGH);
    notifyReply("OK:LED=ON");
  } else if (cmd == "LED:OFF") {
    g_led_state = false;
    digitalWrite(TEST_LED_PIN, LOW);
    notifyReply("OK:LED=OFF");
  } else if (cmd == "LED:TOGGLE") {
    g_led_state = !g_led_state;
    digitalWrite(TEST_LED_PIN, g_led_state ? HIGH : LOW);
    notifyReply(g_led_state ? "OK:LED=ON" : "OK:LED=OFF");
  } else {
    notifyReply("ERR:UNKNOWN");
  }
}

class MonitaServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer) override {
    g_ble_connected = true;
    Serial.println("[BLE] Controller connected");
  }
  void onDisconnect(NimBLEServer* pServer) override {
    g_ble_connected = false;
    Serial.println("[BLE] Controller disconnected -> re-advertise");
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

static void setupBLE() {
  NimBLEDevice::init("MonitaFlex");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MonitaServerCallbacks());

  NimBLEService* pService = pServer->createService(serviceUUID);

  NimBLECharacteristic* pRxCharacteristic = pService->createCharacteristic(
      charUUID_RX, NIMBLE_PROPERTY::WRITE);
  pRxCharacteristic->setCallbacks(new MonitaRxCallbacks());

  pTxCharacteristic = pService->createCharacteristic(
      charUUID_TX, NIMBLE_PROPERTY::NOTIFY);

  pService->start();

  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(serviceUUID);
  pAdvertising->setScanResponse(true);
  NimBLEDevice::startAdvertising();

  Serial.println("[BLE] Advertising as \"MonitaFlex\"");
}

// ============================================================
// setup / loop（今後ここに実装を追加していく）
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Monita Flex WiFi — XIAO ESP32-C3 ===");
  Serial.printf("Sleep interval: %d min\n", g_sleep_minutes);

  pinMode(TEST_LED_PIN, OUTPUT);
  digitalWrite(TEST_LED_PIN, LOW);

  // TODO: NVSから設定値読み込み
  setupBLE();
  // TODO: HX711初期化
  // TODO: WiFi接続
}

void loop() {
  // TODO: 計測 → BLE/WiFi送信 → スリープ

  static uint32_t lastStatusMs = 0;
  if (millis() - lastStatusMs > 5000) {
    lastStatusMs = millis();
    Serial.printf("[STATUS] connected=%s sleep=%dmin\n",
                  g_ble_connected ? "yes" : "no", g_sleep_minutes);
  }

  delay(50);
}
