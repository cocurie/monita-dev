/**
 * Monita LoRa 検証 Step23: GPIO M0/M1 切替タイミング診断
 *
 * 【背景】
 *   22_lora_multi_child で config read が失敗する事象があり、以下は切り分け済み:
 *     - Step20（M0/M1を手動で3V3に直結ジャンパ）→ 成功（モジュール・UART・電源は正常）
 *     - D1(M0)/D2(M1)の導通確認 → 問題なし
 *   残る違いは「Step20は起動直後からずっとHIGH」「22番は起動時LOW→設定時のみHIGHに
 *   切り替える」というシーケンスの違い。これがconfig read失敗の原因かを切り分ける。
 *
 * 【配線】22_lora_multi_childと同じ
 *   XIAO D8 (TX) → E220 RXD / XIAO D9 (RX) ← E220 TXD
 *   XIAO D1 → E220 M0 / XIAO D2 → E220 M1
 *   XIAO 3V3 → E220 VCC / XIAO GND → E220 GND
 *
 * 【動作】
 *   起動時に3パターンを順番に試し、それぞれconfig readの成否をログ出力する:
 *     A) M0/M1をUART開始前からHIGH固定（Step20の手動ジャンパを疑似再現）
 *     B) M0/M1をLOW→（100ms待ち）→HIGHに切替（22番と同じシーケンス・同じ待ち時間）
 *     C) M0/M1をLOW→（500ms待ち）→HIGHに切替（Bより長い待ち時間で切り替え後を安定させる）
 *   3つの結果を比較することで、原因がタイミング（切替後の待ち時間不足）かどうかが分かる。
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

#define LORA_TX_PIN 8
#define LORA_RX_PIN 9
#define LORA_M0_PIN 1
#define LORA_M1_PIN 2
#define LORA_UART_BAUD 9600
#define LORA_CFG_REG_LEN 6

static bool loraReadConfig(uint8_t *out6) {
  while (Serial1.available()) Serial1.read();
  Serial1.write((uint8_t)0xC1);
  Serial1.write((uint8_t)0x00);
  Serial1.write((uint8_t)LORA_CFG_REG_LEN);

  const int respLen = 3 + LORA_CFG_REG_LEN;
  uint8_t resp[3 + LORA_CFG_REG_LEN];
  int idx = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 500UL && idx < respLen) {
    if (Serial1.available()) resp[idx++] = (uint8_t)Serial1.read();
  }
  if (idx < respLen) {
    Serial.print("  → 応答不足（受信"); Serial.print(idx); Serial.println("バイト）");
    return false;
  }
  if (resp[0] != 0xC1) {
    Serial.println("  → ヘッダ不一致");
    return false;
  }
  memcpy(out6, &resp[3], LORA_CFG_REG_LEN);
  return true;
}

static void printResult(const char *label, bool ok, const uint8_t *cfg) {
  Serial.print("[");
  Serial.print(label);
  Serial.print("] ");
  Serial.println(ok ? "成功" : "失敗");
  if (ok) {
    Serial.print("  raw: ");
    for (int i = 0; i < LORA_CFG_REG_LEN; i++) {
      if (cfg[i] < 0x10) Serial.print('0');
      Serial.print(cfg[i], HEX);
      Serial.print(' ');
    }
    Serial.println();
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();
  delay(500);

  Serial.println("\n[Step23] GPIO M0/M1 切替タイミング診断");

  pinMode(LORA_M0_PIN, OUTPUT);
  pinMode(LORA_M1_PIN, OUTPUT);

  // ── パターンA: 起動直後からM0/M1をHIGH固定（Step20の手動ジャンパを疑似再現） ──
  digitalWrite(LORA_M0_PIN, HIGH);
  digitalWrite(LORA_M1_PIN, HIGH);

  Serial1.setPins(LORA_RX_PIN, LORA_TX_PIN);
  Serial1.begin(LORA_UART_BAUD);
  delay(500);  // E220 起動待ち（22番と同じ）

  Serial.println("\n=== パターンA: 起動直後からHIGH固定 ===");
  uint8_t cfgA[LORA_CFG_REG_LEN] = {0};
  bool okA = loraReadConfig(cfgA);
  printResult("A: 起動直後からHIGH固定", okA, cfgA);

  delay(1000);

  // ── パターンB: LOW→100ms待ち→HIGH（22番と同じシーケンス） ──
  Serial.println("\n=== パターンB: LOW→100ms待ち→HIGH（22番と同条件） ===");
  digitalWrite(LORA_M0_PIN, LOW);
  digitalWrite(LORA_M1_PIN, LOW);
  delay(200);  // Normalモードとして少し安定させる
  digitalWrite(LORA_M0_PIN, HIGH);
  digitalWrite(LORA_M1_PIN, HIGH);
  delay(100);  // 22番のLORA_MODE_SWITCH_DELAY_MSと同じ
  uint8_t cfgB[LORA_CFG_REG_LEN] = {0};
  bool okB = loraReadConfig(cfgB);
  printResult("B: LOW→100ms待ち→HIGH", okB, cfgB);

  delay(1000);

  // ── パターンC: LOW→500ms待ち→HIGH（切替後の待ち時間を延長） ──
  Serial.println("\n=== パターンC: LOW→500ms待ち→HIGH（待ち時間を延長） ===");
  digitalWrite(LORA_M0_PIN, LOW);
  digitalWrite(LORA_M1_PIN, LOW);
  delay(200);
  digitalWrite(LORA_M0_PIN, HIGH);
  digitalWrite(LORA_M1_PIN, HIGH);
  delay(500);  // Bより延長
  uint8_t cfgC[LORA_CFG_REG_LEN] = {0};
  bool okC = loraReadConfig(cfgC);
  printResult("C: LOW→500ms待ち→HIGH", okC, cfgC);

  // 通常モードへ戻して終了
  digitalWrite(LORA_M0_PIN, LOW);
  digitalWrite(LORA_M1_PIN, LOW);

  Serial.println("\n=== まとめ ===");
  Serial.print("A(常時HIGH): "); Serial.println(okA ? "OK" : "NG");
  Serial.print("B(100ms待ち): "); Serial.println(okB ? "OK" : "NG");
  Serial.print("C(500ms待ち): "); Serial.println(okC ? "OK" : "NG");
  Serial.println("\n診断完了。この結果を報告してください。");
}

void loop() {
  yield();
}
