/**
 * Monita LoRa 検証 Step19: 親機（受信側）— E220-900T22S(JP) 透過モード受信テスト
 *
 * 【目的】
 *   18_lora_child と対で使う。E220が受信したバイト列をそのままUSBシリアルへ
 *   転送するだけの最小構成。Flex基板・TCA9534は使わず、XIAO nRF52840に
 *   E220 を直結する。
 *
 * 【配線】18_lora_child と同じ
 *   XIAO D8 (TX, 3.3V)  → E220 RXD
 *   XIAO D9 (RX, 3.3V)  ← E220 TXD
 *   XIAO 3V3            → E220 VCC
 *   XIAO GND            → E220 GND
 *   E220 M0             → GND（★必須。Mode0=透過送受信モードに固定する）
 *   E220 M1             → GND（★必須）
 *   E220 AUX            → 未接続でよい
 *
 * 【動作】
 *   Serial1（E220 UART）から "MONITA " という7バイトのプレフィックスを探し、
 *   見つけたらそこから6桁のカウンタ＋続く1バイトのRSSI値を読み取り、
 *   "[RX] MONITA 000001  RSSI=-42dBm" の形でSerial（USB）へ表示する。
 *   10秒以上受信が無ければ警告ログを出す（配線・電源・距離の確認用）。
 *
 *   ★RSSIバイトはE220が REG3(bit7=RSSIバイト有効化) の設定に従って、受信データの
 *     直後に自動付加する。dBm換算式: dBm = RSSIバイト（0〜255の生値）－256
 *     （E220-900T22S(JP) 公式データシート記載の式）。
 *
 * 【プレフィックス再同期について（2026-07-17追加）】
 *   当初は「毎回必ずぴったり13バイト＋RSSI1バイト」という決め打ちの位置カウンタ
 *   実装だったが、実機テストで電波状況悪化により1バイト欠落が起き、それ以降
 *   永久に読み取り位置がずれ続ける不具合が発生した（本番ファームは同期バイト＋
 *   長さ＋チェックサム方式のため起きない問題）。
 *   本版では "MONITA " という固定の7文字を目印にして常にスキャンし続け、
 *   バイト抜け・混入があっても次のメッセージから自動的に読み取り位置を
 *   復帰できるようにしている。ただし本番ファームのようなチェックサム検証は
 *   無いため、稀に文字化けしたまま偶然プレフィックスに一致するような場合の
 *   誤検出までは防げない（テスト用の簡易実装であることに変わりはない）。
 *
 * 【前提・注意】
 *   - E220の設定（アドレス・チャンネル・送信出力・送信方式・RSSIバイト有効化）は
 *     `21_lora_fix_config` で書き込み済みであることが前提。
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>  // USB CDC（Serial）。USE_TINYUSBビルド時に必要

// ブレッドボード配線（18_lora_childと同じ）。M0/M1はGNDへ直結（Mode0固定）してから書き込むこと。
#define LORA_TX_PIN 8
#define LORA_RX_PIN 9
#define LORA_BAUD   9600
#define RX_TIMEOUT_WARN_MS 10000UL

static const char PREFIX[] = "MONITA ";      // 18_lora_childのメッセージ先頭7バイトと一致させる
#define PREFIX_LEN 7
#define DIGITS_LEN 6                          // "%06lu" のカウンタ桁数と一致させる
#define FIELD_TIMEOUT_MS 200UL                // プレフィックス一致後、続きが届かない場合の見切り時間

static uint32_t s_lastRxMs = 0;
static uint32_t s_lastWarnMs = 0;

// 受信状態
enum RxState { SEEK_PREFIX, READ_DIGITS, READ_RSSI };
static RxState s_state = SEEK_PREFIX;

// プレフィックス探索用のスライディングバッファ（直近PREFIX_LEN バイトを保持）
static char    s_ring[PREFIX_LEN];
static uint8_t s_ringPos   = 0;   // 次に書き込む位置（循環）
static uint8_t s_ringCount = 0;   // 有効バイト数（PREFIX_LEN で頭打ち）

static char     s_digits[DIGITS_LEN + 1];
static uint8_t  s_digitsIdx = 0;
static uint32_t s_fieldStartMs = 0;

// リングバッファに1バイト積み、直近 PREFIX_LEN バイトが PREFIX と一致するか調べる
static bool pushAndCheckPrefix(char c) {
  s_ring[s_ringPos] = c;
  s_ringPos = (uint8_t)((s_ringPos + 1) % PREFIX_LEN);
  if (s_ringCount < PREFIX_LEN) s_ringCount++;
  if (s_ringCount < PREFIX_LEN) return false;

  for (uint8_t i = 0; i < PREFIX_LEN; i++) {
    char ch = s_ring[(uint8_t)((s_ringPos + i) % PREFIX_LEN)];
    if (ch != PREFIX[i]) return false;
  }
  return true;
}

static void resetToSeek() {
  s_state = SEEK_PREFIX;
  s_ringPos = 0;
  s_ringCount = 0;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Serial.println(F("\n[Step19] LoRa親機（受信側）起動"));
  Serial.println(F("※ E220のM0/M1はGNDに直結してください（Mode0固定）"));

  Serial1.setPins(LORA_RX_PIN, LORA_TX_PIN);
  Serial1.begin(LORA_BAUD);
  delay(500);  // E220 起動待ち（暫定値）

  Serial.print(F("[LoRa] UART開始 baud=")); Serial.println(LORA_BAUD);
  Serial.println(F("[LoRa] 受信待機中...（プレフィックス再同期あり）"));

  s_lastRxMs = millis();
}

void loop() {
  while (Serial1.available()) {
    uint8_t b = (uint8_t)Serial1.read();
    s_lastRxMs = millis();

    switch (s_state) {
      case SEEK_PREFIX:
        if (pushAndCheckPrefix((char)b)) {
          s_state = READ_DIGITS;
          s_digitsIdx = 0;
          s_fieldStartMs = millis();
        }
        break;

      case READ_DIGITS:
        s_digits[s_digitsIdx++] = (char)b;
        if (s_digitsIdx >= DIGITS_LEN) {
          s_digits[DIGITS_LEN] = '\0';
          s_state = READ_RSSI;
          s_fieldStartMs = millis();
        }
        break;

      case READ_RSSI: {
        int rssiDbm = (int)b - 256;
        Serial.print(F("[RX] MONITA "));
        Serial.print(s_digits);
        Serial.print(F("  RSSI="));
        Serial.print(rssiDbm);
        Serial.println(F("dBm"));
        resetToSeek();
        break;
      }
    }
  }

  // プレフィックス一致後、続きのバイトが規定時間内に届かない場合は探索状態に戻す
  // （届かないバイトを待ち続けて次のメッセージまで巻き込まれるのを防ぐ）
  if (s_state != SEEK_PREFIX && millis() - s_fieldStartMs > FIELD_TIMEOUT_MS) {
    Serial.println(F("[WARN] メッセージ途中でタイムアウト。再同期します"));
    resetToSeek();
  }

  uint32_t now = millis();
  if (now - s_lastRxMs > RX_TIMEOUT_WARN_MS && now - s_lastWarnMs > RX_TIMEOUT_WARN_MS) {
    s_lastWarnMs = now;
    Serial.println(F("\n[WARN] 10秒以上受信なし（配線・電源・距離・M0/M1を確認してください）"));
  }
  yield();
}
