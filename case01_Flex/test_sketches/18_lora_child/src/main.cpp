/**
 * Monita LoRa 検証 Step18: 子機（送信側）— E220-900T22S(JP) 透過モード送信テスト
 *
 * 【目的】
 *   E220モジュール2台（本スケッチ + 19_lora_parent）でLoRa透過モードの疎通を
 *   確認する、最も単純な送受信テスト。Flex基板・TCA9534は使わず、
 *   XIAO nRF52840に E220 を直結する。
 *
 * 【配線】
 *   XIAO D8 (TX, 3.3V)  → E220 RXD
 *   XIAO D9 (RX, 3.3V)  ← E220 TXD
 *   XIAO 3V3            → E220 VCC
 *   XIAO GND            → E220 GND
 *   E220 M0             → GND（★必須。Mode0=透過送受信モードに固定する）
 *   E220 M1             → GND（★必須）
 *   E220 AUX            → 未接続でよい（本テストでは監視しない）
 *
 * 【動作】
 *   2秒ごとに "MONITA 000001" のような**固定13バイト長**のテキストをSerial1へ
 *   書き込む（E220が透過モードのため書いたバイト列がそのまま電波送信される）。
 *   送信内容はUSBシリアルにもログ出力する。
 *
 *   ★固定長にしているのは、親機側（19_lora_parent）でRSSIバイト（E220が
 *     REG3の設定により受信データの直後に自動付加する1バイト）と本文の区切りを
 *     明確にするため。可変長だと「どこまでが本文でどこからRSSIか」を親機側で
 *     判別できない。
 *
 * 【前提・注意】
 *   - E220の設定（アドレス・チャンネル・送信出力・送信方式・RSSIバイト有効化）は
 *     `21_lora_fix_config` で書き込み済みであることが前提（工場出荷デフォルトの
 *     ままだと送信出力が無効・固定送信モードになっており届かないことがある。
 *     詳細は `e220_register_map_and_bug` メモリ参照）。
 *   - 本番ファームとの違い: TCA9534経由のM0/M1制御・設定確認/書込ロジック・
 *     フレーム化（同期バイト+長さ+チェックサム）は行わない、固定長の生テキスト
 *     送信のみ。
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>  // USB CDC（Serial）。USE_TINYUSBビルド時に必要

#define LORA_TX_PIN 8
#define LORA_RX_PIN 9
#define LORA_BAUD   9600
#define SEND_INTERVAL_MS 2000UL
#define MSG_LEN 13  // "MONITA " (7バイト) + 6桁カウンタ = 13バイト固定長

static uint32_t s_counter = 0;
static uint32_t s_lastSendMs = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Serial.println(F("\n[Step18] LoRa子機（送信側）起動"));
  Serial.println(F("※ E220のM0/M1はGNDに直結してください（Mode0固定）"));

  Serial1.setPins(LORA_RX_PIN, LORA_TX_PIN);
  Serial1.begin(LORA_BAUD);
  delay(500);  // E220 起動待ち（暫定値）

  Serial.print(F("[LoRa] UART開始 baud=")); Serial.println(LORA_BAUD);
  Serial.println(F("[LoRa] 送信開始（2秒間隔、固定13バイト長）"));
}

void loop() {
  uint32_t now = millis();
  if (now - s_lastSendMs >= SEND_INTERVAL_MS) {
    s_lastSendMs = now;
    s_counter++;
    if (s_counter > 999999UL) s_counter = 1;  // 6桁に収める

    char msg[MSG_LEN + 1];
    snprintf(msg, sizeof(msg), "MONITA %06lu", (unsigned long)s_counter);
    Serial1.write((const uint8_t*)msg, MSG_LEN);

    Serial.print(F("[TX] "));
    Serial.println(msg);
  }
  yield();
}
