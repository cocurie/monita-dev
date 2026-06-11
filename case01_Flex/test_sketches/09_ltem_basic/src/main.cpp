/**
 * Monita Flex v3.02 — 検証 Step9 rev3: SIM7080G LTE-M 基本接続テスト（LED付き）
 *
 * 変更点（rev3）:
 *   XIAO nRF52840 内蔵 RGB LED で状態を表示
 *
 * LED 状態一覧（アクティブ LOW: LOW=点灯 / HIGH=消灯）:
 *   起動待ち（15秒）    : Blue 1秒ごとに1チカ
 *   AT 確認中           : Blue 速点滅（試行ごと）
 *   AT NG               : Red 3チカ → loop() で Red 2秒ごと1チカ
 *   SIM READY           : Green 1チカ
 *   SIM NG              : Red 2チカ
 *   ネットワーク登録中  : Blue 点灯（solid）
 *   ネットワーク登録 OK : Green 2チカ
 *   ネットワーク登録 NG : Red 3チカ → loop() で Red 2秒ごと1チカ
 *   IP 取得 OK / 完了   : Green 点灯（solid）
 *   loop() 正常待機中   : Green 5秒ごとに heartbeat
 *
 * 確認内容:
 *   1. AT 疎通確認
 *   2. SIM 認識確認（AT+CPIN?）
 *   3. LTE-M ネットワーク登録確認（AT+CREG? / AT+CGATT?）
 *   4. IP アドレス取得確認（AT+CNACT?）
 *
 * 配線（Flex v3.02 基板上）:
 *   XIAO D8 (TX) → SIM7080G RX（基板上で接続済み）
 *   XIAO D9 (RX) ← SIM7080G TX（基板上で接続済み）
 *   D10 HIGH で 3V3_SW ON
 *   M5STAMP CatM 5V ← XIAO 5V ピン（ジャンパ追加）
 *
 * SIM: 1NCE IoT SIM（APN: iot.1nce.net）
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

static const uint8_t  SW_POWER_PIN = 10;
static const uint8_t  LTE_TX_PIN   = 8;    // D8 → SIM7080G RX
static const uint8_t  LTE_RX_PIN   = 9;    // D9 ← SIM7080G TX
static const uint32_t LTE_BAUD     = 115200;

static const char* APN = "iot.1nce.net";

// ============================================================
// LED（XIAO nRF52840 内蔵 RGB、アクティブ LOW）
//   LED_RED / LED_GREEN / LED_BLUE は Adafruit BSP で定義済み
//   LOW = 点灯、HIGH = 消灯
// ============================================================
static void ledInit() {
  pinMode(LED_RED,   OUTPUT); digitalWrite(LED_RED,   HIGH);
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, HIGH);
  pinMode(LED_BLUE,  OUTPUT); digitalWrite(LED_BLUE,  HIGH);
}

static void ledOff() {
  digitalWrite(LED_RED,   HIGH);
  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(LED_BLUE,  HIGH);
}

static void ledOn(uint8_t pin)     { digitalWrite(pin, LOW);  }
static void ledPinOff(uint8_t pin) { digitalWrite(pin, HIGH); }

// count 回点滅（onMs 点灯 → offMs 消灯）
static void ledBlink(uint8_t pin, int count, int onMs = 150, int offMs = 150) {
  for (int i = 0; i < count; i++) {
    digitalWrite(pin, LOW);  delay(onMs);
    digitalWrite(pin, HIGH); if (i < count - 1) delay(offMs);
  }
}

// setup() 完了フラグ（loop() での LED 振る舞いを切り替え）
static bool s_setupOk = false;  // true = 全ステップ正常完了

// ============================================================
// AT コマンド送信（受信を waitMs 待って返す）
// ============================================================
static String sendAT(const String& cmd, int waitMs = 5000) {
  Serial.print(F(">> "));
  Serial.println(cmd);
  Serial1.print(cmd + "\r\n");

  long start = millis();
  String resp = "";
  while (millis() - start < waitMs) {
    while (Serial1.available()) resp += (char)Serial1.read();
    yield();  // FreeRTOS watchdog 対策
  }
  if (resp.length() > 0) {
    Serial.println(resp);
  } else {
    Serial.println(F("(応答なし)"));
  }
  return resp;
}

// ============================================================
// [1] AT 疎通確認（115200 固定・最大 20 回リトライ）
//     試行ごとに Blue 速点滅
// ============================================================
static bool probeAT() {
  Serial.println(F("  115200 bps 固定で試行..."));

  for (int t = 0; t < 20; t++) {
    Serial.print('.');
    ledBlink(LED_BLUE, 1, 80, 0);  // Blue 速点滅（試行中）

    Serial1.print("AT\r\n");
    delay(420);  // 80ms blink + 420ms = 500ms/回

    String r = "";
    unsigned long s = millis();
    while (millis() - s < 500) {
      while (Serial1.available()) r += (char)Serial1.read();
      yield();
    }

    if (r.indexOf("OK") >= 0) {
      Serial.println();
      Serial.println(F("  AT → OK ✓"));
      Serial.print(F("  応答: "));
      Serial.println(r);
      ledOff();
      return true;
    }
    while (Serial1.available()) Serial1.read();
  }

  Serial.println();
  ledOff();
  return false;
}

// ============================================================
// Arduino エントリ
// ============================================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) yield();
  delay(1000);

  ledInit();

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  // Serial1 は一度だけ初期化（end()/begin() ループはしない）
  Serial1.setPins(LTE_RX_PIN, LTE_TX_PIN);
  Serial1.begin(LTE_BAUD);
  delay(100);

  Serial.println(F("\n[STEP9 rev3] SIM7080G LTE-M 基本接続テスト（LED付き）"));
  Serial.println(F("SIM: 1NCE IoT SIM / APN: iot.1nce.net"));
  Serial.println(F("========================================"));

  // モジュール起動待ち — 1秒ごとに Blue 1チカ
  Serial.print(F("SIM7080G 起動待ち（15秒）"));
  for (int i = 0; i < 15; i++) {
    ledBlink(LED_BLUE, 1, 200, 0);
    delay(800);
    Serial.print('.');
    while (Serial1.available()) {
      char c = Serial1.read();
      Serial.write(c);  // 起動ログを表示
    }
  }
  ledOff();
  Serial.println();

  // [1] AT 疎通 — probeAT() 内で Blue 速点滅
  Serial.println(F("\n[1] AT疎通確認"));
  if (!probeAT()) {
    Serial.println(F("\n★ AT応答なし。確認項目:"));
    Serial.println(F("  - M5STAMP CatM の 5V ピンに電源が来ているか"));
    Serial.println(F("  - D8(TX)/D9(RX) の基板上配線が正しいか"));
    Serial.println(F("  - シリアルモニタから「AT」と入力して Enter → 応答確認"));
    Serial.println(F("\nloop() でシリアルパススルーモードに入ります。"));
    ledBlink(LED_RED, 3, 200, 200);  // Red 3チカ → エラー通知
    return;  // loop() で Red 点滅継続
  }

  // Echo off
  sendAT("ATE0", 2000);

  // [2] SIM 認識
  Serial.println(F("\n[2] SIM認識確認"));
  String cpin = sendAT("AT+CPIN?", 5000);
  if (cpin.indexOf("READY") >= 0) {
    Serial.println(F("  → SIM READY ✓"));
    ledBlink(LED_GREEN, 1, 300, 0);   // Green 1チカ
  } else if (cpin.indexOf("SIM PIN") >= 0) {
    Serial.println(F("  → SIM PIN ロック中（PIN解除が必要）"));
    ledBlink(LED_RED, 2, 200, 200);   // Red 2チカ
  } else {
    Serial.println(F("  → SIM 未認識（カード挿入・接触を確認）"));
    ledBlink(LED_RED, 2, 200, 200);   // Red 2チカ → エラー
    return;
  }

  // モデム情報
  sendAT("AT+CGMM", 2000);   // モジュール型番
  sendAT("AT+CIMI", 2000);   // IMSI

  // [3] ネットワーク登録 — Blue solid で待機中を表現
  Serial.println(F("\n[3] LTE-M ネットワーク登録（最大60秒）"));
  ledOn(LED_BLUE);  // 登録待ち = Blue 点灯
  sendAT("AT+CNMP=38", 2000); delay(300);  // LTE only
  sendAT("AT+CMNB=1",  2000); delay(300);  // LTE-M
  sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\"", 2000);

  bool registered = false;
  for (int i = 0; i < 12; i++) {
    String r = sendAT("AT+CREG?", 3000);
    if (r.indexOf("0,1") >= 0 || r.indexOf("0,5") >= 0) {
      Serial.println(F("  → ネットワーク登録 OK ✓"));
      registered = true;
      break;
    }
    Serial.print('.');
    delay(5000);
  }
  if (!registered) {
    String att = sendAT("AT+CGATT?", 3000);
    if (att.indexOf("+CGATT: 1") >= 0) {
      Serial.println(F("  → Attach 済み（CREG未確認だが続行）✓"));
      registered = true;
    } else {
      Serial.println(F("  → 登録タイムアウト（SIM/アンテナ/エリアを確認）"));
    }
  }

  ledOff();

  if (!registered) {
    ledBlink(LED_RED, 3, 200, 200);  // Red 3チカ → エラー
    Serial.println(F("\nloop() でATコマンドを手動送信できます。"));
    return;
  }

  // 登録OK → Green 2チカ
  ledBlink(LED_GREEN, 2, 200, 150);

  // [4] IP アドレス取得
  Serial.println(F("\n[4] IP アドレス取得"));
  delay(3000);
  sendAT("AT+CNACT=0,1", 15000);
  delay(3000);
  String cnact = sendAT("AT+CNACT?", 3000);
  if (cnact.indexOf("0,1") >= 0) {
    Serial.println(F("  → IP取得 OK ✓"));
    ledOn(LED_GREEN);  // 完了 = Green 点灯（solid）
  } else {
    Serial.println(F("  → IP取得失敗"));
    ledBlink(LED_RED, 3, 200, 200);  // Red 3チカ → エラー
    return;
  }

  Serial.println(F("\n=== Step9 完了 ==="));
  Serial.println(F("loop() でATコマンドを手動送信できます。"));
  s_setupOk = true;  // 正常完了フラグ
}

// ============================================================
// loop: シリアルパススルー + LED heartbeat / エラー点滅
// ============================================================
static uint32_t s_lastHeartbeat = 0;

void loop() {
  uint32_t now = millis();

  if (s_setupOk) {
    // 正常完了後: Green 5秒ごとに heartbeat（solid を一瞬切って戻す）
    if (now - s_lastHeartbeat >= 5000) {
      s_lastHeartbeat = now;
      ledPinOff(LED_GREEN); delay(80); ledOn(LED_GREEN);
    }
  } else {
    // エラー後: Red 2秒ごとに1チカ
    if (now - s_lastHeartbeat >= 2000) {
      s_lastHeartbeat = now;
      ledBlink(LED_RED, 1, 100, 0);
    }
  }

  // シリアルパススルー（手動 AT コマンド）
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      Serial1.print(line + "\r\n");
      Serial.print(F(">> "));
      Serial.println(line);
    }
  }
  while (Serial1.available()) {
    Serial.write(Serial1.read());
  }
  yield();
}
