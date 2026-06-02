/**
 * Monita Flex v3.02 — 検証 Step9: SIM7080G LTE-M 基本接続テスト（診断版）
 *
 * 確認内容:
 *   1. AT 疎通確認（115200 / 9600 両方で試す）
 *   2. SIM 認識確認（AT+CPIN?）
 *   3. LTE-M ネットワーク登録確認（AT+CREG? / AT+CGATT?）
 *   4. IP アドレス取得確認（AT+CNACT?）
 *
 * 配線（Flex v3.02 基板上で完結）:
 *   XIAO D8 (TX) → SIM7080G RX  ← 基板上で接続済み
 *   XIAO D9 (RX) ← SIM7080G TX  ← 基板上で接続済み
 *   D10 HIGH で 3V3_SW ON
 *
 * SIM: 1NCE IoT SIM（APN: iot.1nce.net）
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

static const uint8_t SW_POWER_PIN = 10;
static const uint8_t LTE_TX_PIN   = 8;   // D8 → SIM7080G RX（確認済み）
static const uint8_t LTE_RX_PIN   = 9;   // D9 ← SIM7080G TX

static const char* APN = "iot.1nce.net";

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
  }
  if (resp.length() > 0) Serial.println(resp);
  return resp;
}

// ============================================================
// [1] AT 疎通確認（115200 → 9600 → 自動ボーレート順に試す）
// ============================================================
static uint32_t probeAT() {
  uint32_t bauds[] = {115200, 9600, 4800, 19200};
  for (uint32_t baud : bauds) {
    Serial.print(F("  試行: "));
    Serial.print(baud);
    Serial.print(F(" bps ... "));
    Serial1.end();
    delay(100);
    Serial1.setPins(LTE_RX_PIN, LTE_TX_PIN);
    Serial1.begin(baud);
    delay(500);

    // 3回試行
    for (int t = 0; t < 3; t++) {
      Serial1.print("AT\r\n");
      delay(500);
      String r = "";
      unsigned long s = millis();
      while (millis() - s < 1000) {
        while (Serial1.available()) r += (char)Serial1.read();
      }
      if (r.indexOf("OK") >= 0) {
        Serial.println(F("OK ✓"));
        Serial.print(F("  応答: "));
        Serial.println(r);
        return baud;
      }
    }
    Serial.println(F("応答なし"));
  }
  return 0;
}

// ============================================================
// Arduino エントリ
// ============================================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) yield();
  delay(2000);


  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  Serial.println(F("\n[STEP9] SIM7080G LTE-M 基本接続テスト"));
  Serial.println(F("SIM: 1NCE IoT SIM / APN: iot.1nce.net"));
  Serial.println(F("========================================"));

  // モジュール起動待ち（SIM7080G は最大 10 秒かかることがある）
  Serial.print(F("SIM7080G 起動待ち（10秒）"));
  for (int i = 0; i < 10; i++) {
    delay(1000);
    Serial.print('.');
  }
  Serial.println();

  // [1] AT 疎通（ボーレート自動検出）
  Serial.println(F("\n[1] AT疎通確認（ボーレート自動検出）"));
  uint32_t baud = probeAT();
  if (baud == 0) {
    Serial.println(F("\n★ AT応答なし。確認項目:"));
    Serial.println(F("  - SIM7080G モジュールが基板に実装されているか"));
    Serial.println(F("  - D8(TX)/D9(RX) の基板上配線が正しいか"));
    Serial.println(F("  - モジュールへの電源供給（3V3_SW or 外部電源）"));
    Serial.println(F("  - PWRKEY ピンのパルスが必要な場合はコード追加が必要"));
    Serial.println(F("\nloop() でシリアルパススルーモードに入ります。"));
    Serial.println(F("シリアルモニタから AT と入力して Enter → 応答が返るか確認"));
    return;
  }

  Serial.print(F("\n通信ボーレート確定: "));
  Serial.print(baud);
  Serial.println(F(" bps"));

  // 115200以外だったら統一
  if (baud != 115200) {
    Serial.println(F("115200 bps に変更中..."));
    sendAT("AT+IPR=115200", 2000);
    delay(500);
    Serial1.end();
    delay(100);
    Serial1.setPins(LTE_RX_PIN, LTE_TX_PIN);
    Serial1.begin(115200);
    delay(500);
    Serial.println(F("変更完了"));
  }

  // [2] SIM 認識
  Serial.println(F("\n[2] SIM認識確認"));
  String cpin = sendAT("AT+CPIN?", 5000);
  if (cpin.indexOf("READY") >= 0) {
    Serial.println(F("  → SIM READY ✓"));
  } else if (cpin.indexOf("SIM PIN") >= 0) {
    Serial.println(F("  → SIM PIN ロック中（PIN解除が必要）"));
  } else {
    Serial.println(F("  → SIM 未認識（カード挿入・接触を確認）"));
  }

  // モデム情報
  sendAT("AT+CGMM", 2000);   // モジュール型番
  sendAT("AT+CIMI", 2000);   // IMSI

  // [3] ネットワーク登録
  Serial.println(F("\n[3] LTE-M ネットワーク登録（最大60秒）"));
  sendAT("AT+CNMP=38", 2000); delay(300);
  sendAT("AT+CMNB=1",  2000); delay(300);
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

  if (!registered) {
    Serial.println(F("\nloop() でATコマンドを手動送信できます。"));
    return;
  }

  // [4] IP アドレス取得
  Serial.println(F("\n[4] IP アドレス取得"));
  delay(3000);
  sendAT("AT+CNACT=0,1", 15000);
  delay(3000);
  String cnact = sendAT("AT+CNACT?", 3000);
  if (cnact.indexOf("0,1") >= 0) {
    Serial.println(F("  → IP取得 OK ✓"));
  } else {
    Serial.println(F("  → IP取得失敗"));
  }

  Serial.println(F("\n=== Step9 完了 ==="));
  Serial.println(F("loop() でATコマンドを手動送信できます。"));
}

void loop() {
  // シリアルパススルー（手動 AT コマンド）
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) sendAT(line, 5000);
  }
  while (Serial1.available()) {
    Serial.write(Serial1.read());
  }
}
