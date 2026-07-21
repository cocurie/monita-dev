/**
 * Monita LoRa 検証 Step20: E220-900T22S(JP) 設定値readback（診断用）
 *
 * 【目的】
 *   18_lora_child / 19_lora_parent で送受信が確認できない場合の切り分け用。
 *   E220を設定モード(Mode3)にして現在のレジスタ値（ADDH/ADDL/SPED/TRANS_MODE/
 *   CHANNEL/OPTION）を読み出し、USBシリアルに表示する。
 *   子機・親機それぞれのE220で本スケッチを実行し、表示された値を見比べることで
 *   「工場出荷設定・以前の設定が2台で食い違っていないか」を確認できる。
 *
 * 【配線】18/19と同じUART配線（D8→RXD, D9←TXD, 3V3, GND）だが、
 *   この検証中だけ以下を変更する:
 *
 *     E220 M0 → 3V3（★一時的にGNDから付け替える）
 *     E220 M1 → 3V3（★同上）
 *
 *   これでMode3（設定/スリープモード）になり、設定コマンドに応答するようになる。
 *   ★検証が終わったら忘れずにM0/M1をGNDへ戻すこと（18/19の通常動作にはMode0が必要）。
 *
 * 【動作】
 *   5秒おきにREADコマンド(0xC1)でADDH〜OPTION(6バイト)を読み出し、
 *   生の応答バイト列と主要レジスタの解釈をUSBシリアルに表示する。
 *
 * 【レジスタ配置について】
 *   xreef/EByte_LoRa_E220_Series_Library のレジスタ配置に基づく。
 *   ビットレベルの正確な意味は E220-900T22S(JP) 公式データシートで最終確認すること
 *   （本スケッチはあくまで2台の設定値が一致しているかを見比べるための診断用）。
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>  // USB CDC（Serial）。USE_TINYUSBビルド時に必要

#define LORA_TX_PIN 8
#define LORA_RX_PIN 9
#define LORA_BAUD   9600
#define READ_INTERVAL_MS 5000UL

// READ(0xC1) で ADDH(0x00) から6バイト（ADDH,ADDL,SPED,TRANS_MODE,CHANNEL,OPTION）読み出す
static void readConfig() {
  while (Serial1.available()) Serial1.read();  // 受信バッファ掃除

  Serial1.write((uint8_t)0xC1);
  Serial1.write((uint8_t)0x00);  // 開始レジスタ ADDH
  Serial1.write((uint8_t)0x06);  // 読み出しバイト数

  uint8_t resp[16];
  int idx = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 500UL && idx < (int)sizeof(resp)) {
    if (Serial1.available()) resp[idx++] = (uint8_t)Serial1.read();
  }

  Serial.print(F("[READ] 受信バイト数=")); Serial.println(idx);

  if (idx == 0) {
    Serial.println(F("  → 応答なし。M0/M1が本当に3V3に繋がっているか、配線・電源を確認してください"));
    return;
  }

  Serial.print(F("  raw: "));
  for (int i = 0; i < idx; i++) {
    if (resp[i] < 0x10) Serial.print('0');
    Serial.print(resp[i], HEX);
    Serial.print(' ');
  }
  Serial.println();

  // ヘッダ(0xC1, 開始アドレス, 長さ) + データ6バイト = 9バイトを想定
  if (idx >= 9 && resp[0] == 0xC1) {
    Serial.print(F("  ADDH=0x"));              Serial.print(resp[3], HEX);
    Serial.print(F(" ADDL=0x"));               Serial.print(resp[4], HEX);
    Serial.print(F(" SPED(REG0)=0x"));         Serial.print(resp[5], HEX);
    Serial.print(F(" TRANS_MODE(REG1)=0x"));   Serial.print(resp[6], HEX);
    Serial.print(F(" CHANNEL(REG2)=0x"));      Serial.print(resp[7], HEX);
    Serial.print(F(" OPTION(REG3)=0x"));       Serial.println(resp[8], HEX);
  } else {
    Serial.println(F("  → ヘッダが想定と異なる（0xC1で始まっていない等）。配線・M0M1・ボーレートを確認してください"));
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Serial.println(F("\n[Step20] E220 設定値readback"));
  Serial.println(F("★ M0/M1を一時的に3V3へ接続してから実行してください（Mode3=設定モード）"));
  Serial.println(F("  （18/19の通常動作に戻すときはM0/M1をGNDへ戻すのを忘れずに）"));

  Serial1.setPins(LORA_RX_PIN, LORA_TX_PIN);
  Serial1.begin(LORA_BAUD);
  delay(500);
}

void loop() {
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  if (lastMs == 0 || now - lastMs >= READ_INTERVAL_MS) {
    lastMs = now;
    Serial.println(F("\n[READ] 設定値を読み出します..."));
    readConfig();
  }
  yield();
}
