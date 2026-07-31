/**
 * Monita LoRa 検証 Step21: E220-900T22S(JP) 設定修正
 *
 * 【背景】
 *   Step20（設定値readback）で、子機・親機とも以下の異常な設定値が判明した:
 *     REG1(0x03) = 0x00 → 下位2bit=00 = 送信出力「Not available」
 *                          （公式データシート初期値は 01=13dBm）
 *     REG3(0x05) = 0x40 → bit6=1 = 「固定(Fixed-block)送信モード」
 *                          （公式データシート初期値は 0=トランスペアレント送信モード）
 *   18/19番のテストは透過モード前提の単純なテキスト送受信のため、固定送信モードの
 *   ままでは正しく動作しない。本スケッチで両方を修正する。
 *
 * 【対象レジスタと書き込む値】（ADDH/ADDL/REG0/REG2はStep20の読み出し値のまま維持）
 *     ADDH = 0x00（維持）
 *     ADDL = 0x00（維持）
 *     REG0 = 0x68（維持。UART 9600bps + エア速度設定はそのまま）
 *     REG1 = 0x01（修正: 送信出力を 01=13dBm へ。ペイロード長200byte/RSSIノイズ無効は維持）
 *     REG2 = 0x00（維持。チャンネル）
 *     REG3 = 0x80（修正: bit7=RSSIバイト有効化ON、bit6=0で送信方式は引き続き
 *                  トランスペアレント送信モード。受信側は受信データの直後に
 *                  自動付加されるRSSIバイトを読めるようになる）
 *
 * 【配線】20_lora_config_readと同じ
 *   E220 M0 → 3V3（★一時的にGNDから付け替える。Mode3=設定モードにするため）
 *   E220 M1 → 3V3（★同上）
 *   ★書き込み完了後は、M0/M1をGNDへ戻し、18/19を書き込み直してから再テストすること。
 *
 * 【動作】
 *   起動時に1回、WRITEコマンド(0xC0、不揮発保存)で上記6バイトを書き込み、
 *   その後READコマンド(0xC1)で読み直して書き込み結果を確認する。
 *   子機・親機の両方でこのスケッチを実行すること。
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>  // USB CDC（Serial）。USE_TINYUSBビルド時に必要

// ★2026-07-24: 書き込む対象基板に応じてピン割当を切り替える。
//   TARGET_GATEWAY=0 … ブレッドボード子機（D8=TX, D9=RX, M0/M1は手動で3V3へ）
//   TARGET_GATEWAY=1 … Gateway v1.1実機（D0=RX, D1=TX, D2=M0/M1共通駆動をGPIOで制御）
#define TARGET_GATEWAY 0

#if TARGET_GATEWAY
  #define LORA_TX_PIN 1
  #define LORA_RX_PIN 0
  #define LORA_M0M1_PIN 2   // Gateway基板はM0/M1短絡・D2共通駆動。設定モードはHIGH駆動
#else
  #define LORA_TX_PIN 8
  #define LORA_RX_PIN 9
  // ブレッドボードはM0/M1を手動で3V3へ接続する（GPIO駆動しない）
#endif
#define LORA_BAUD   9600

// ★2026-07-24 干渉切り分けテスト: チャンネルだけ変更する。
// 「強いRSSI(-45dBm)なのに7〜8割ロス」という症状は、周波数チャンネル0(バンド端)上の
// 干渉（Wi-SUN等の920MHz帯機器）or 2台間の周波数ズレが疑われる。チャンネルをバンド中央寄りへ
// 動かして受信率が改善するか確認する。★子機・親機の両方のE220にこのスケッチを流すこと。
// （改善したら 18/22/gateway_v1.1 の REG2 もこの値に合わせる。改善しなければ0x00へ戻す）
#define TEST_CHANNEL 0x00  // 本番/22_lora_multi_childの既定値へ戻す（診断中は0x0Aにしていた）
// 書き込む6バイト（ADDH, ADDL, REG0, REG1, REG2, REG3）
static const uint8_t NEW_CFG[6] = {0x00, 0x00, 0x68, 0x01, TEST_CHANNEL, 0x80};

static void printRegs(const uint8_t *d) {
  Serial.print(F("  ADDH=0x")); Serial.print(d[0], HEX);
  Serial.print(F(" ADDL=0x"));  Serial.print(d[1], HEX);
  Serial.print(F(" REG0=0x"));  Serial.print(d[2], HEX);
  Serial.print(F(" REG1=0x"));  Serial.print(d[3], HEX);
  Serial.print(F(" REG2=0x"));  Serial.print(d[4], HEX);
  Serial.print(F(" REG3=0x"));  Serial.println(d[5], HEX);
}

// READ(0xC1) で ADDH(0x00) から6バイト読み出す。成功時true、outに格納
static bool readConfig(uint8_t *out6) {
  while (Serial1.available()) Serial1.read();

  Serial1.write((uint8_t)0xC1);
  Serial1.write((uint8_t)0x00);
  Serial1.write((uint8_t)0x06);

  uint8_t resp[9];
  int idx = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 500UL && idx < (int)sizeof(resp)) {
    if (Serial1.available()) resp[idx++] = (uint8_t)Serial1.read();
  }

  if (idx < 9 || resp[0] != 0xC1) {
    Serial.print(F("  → 応答異常（受信")); Serial.print(idx); Serial.println(F("バイト）"));
    return false;
  }
  memcpy(out6, &resp[3], 6);
  return true;
}

// WRITE(0xC0, 不揮発保存) で6バイト書き込む
static void writeConfig(const uint8_t *cfg6) {
  Serial1.write((uint8_t)0xC0);
  Serial1.write((uint8_t)0x00);
  Serial1.write((uint8_t)0x06);
  for (int i = 0; i < 6; i++) Serial1.write(cfg6[i]);
  delay(200);  // モジュール内部の不揮発メモリ書き込み待ち
  unsigned long t0 = millis();
  while (millis() - t0 < 300UL) { while (Serial1.available()) Serial1.read(); }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Serial.println(F("\n[Step21] E220 設定修正"));
#if TARGET_GATEWAY
  Serial.println(F("★ Gateway基板モード: D2でM0/M1をHIGH駆動しMode3(設定)にします"));
  pinMode(LORA_M0M1_PIN, OUTPUT);
  digitalWrite(LORA_M0M1_PIN, HIGH);  // Mode3=設定モード
  delay(100);
#else
  Serial.println(F("★ M0/M1を一時的に3V3へ接続してから実行してください（Mode3=設定モード）"));
#endif

  Serial1.setPins(LORA_RX_PIN, LORA_TX_PIN);
  Serial1.begin(LORA_BAUD);
  delay(500);

  Serial.println(F("\n[BEFORE] 現在の設定値:"));
  uint8_t before[6] = {0};
  if (readConfig(before)) printRegs(before);

  Serial.println(F("\n[WRITE] 修正値を書き込みます..."));
  printRegs(NEW_CFG);
  writeConfig(NEW_CFG);

  delay(200);
  Serial.println(F("\n[AFTER] 書き込み後の設定値（再読み出し）:"));
  uint8_t after[6] = {0};
  if (readConfig(after)) {
    printRegs(after);
    bool ok = memcmp(after, NEW_CFG, 6) == 0;
    Serial.println(ok ? F("\n✓ 書き込み成功。M0/M1をGNDへ戻し、18/19を書き込み直してください")
                       : F("\n✗ 書き込み後の値が一致しません。配線・M0M1を確認してもう一度実行してください"));
  }
}

void loop() {
  // 何もしない（setup()の1回のみで完結）
}
