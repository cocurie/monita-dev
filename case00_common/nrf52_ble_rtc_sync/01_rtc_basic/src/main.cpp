/**
 * 検証 Step1 + Step2: DS3231 単体動作確認 & 停電耐性確認
 *
 * Step1 合格基準:
 *   - シリアルモニタに時刻が毎秒正しく進む
 *   - 'S' 送信でコンパイル時刻にセットされる
 *
 * Step2 手順（停電耐性確認）:
 *   1. Step1 で時刻をセットして正常動作を確認
 *   2. USB ケーブルを抜く（電源断）
 *   3. 数分放置
 *   4. USB を再接続 → シリアルを開く
 *   5. 表示時刻が電源断前から継続していれば合格
 *      （コイン電池 CR2032 が DS3231 に供給されている必要あり）
 *
 * 配線:
 *   DS3231  SDA → XIAO D4 (SDA)
 *   DS3231  SCL → XIAO D5 (SCL)
 *   DS3231  VCC → 3.3V
 *   DS3231  GND → GND
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <RTClib.h>

RTC_DS3231 rtc;

// タイムゾーン設定（JST = UTC+9）
static const int32_t TZ_OFFSET_SEC = 9 * 3600L;  // 秒単位

// ── 起動時に表示するメッセージ ─────────────────────────
static void printHelp() {
  Serial.println(F("──────────────────────────────────────────"));
  Serial.println(F("コマンド:"));
  Serial.println(F("  S              → コンパイル時刻にセット（数分ずれる場合あり）"));
  Serial.println(F("  Txxxxxxxxxx    → Unix タイムスタンプで正確にセット"));
  Serial.println(F("                   例: T1749034800"));
  Serial.println(F("                   https://www.unixtimestamp.com/ で取得"));
  Serial.println(F("  ?              → このヘルプを表示"));
  Serial.println(F("──────────────────────────────────────────"));
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  Wire.begin();

  if (!rtc.begin()) {
    Serial.println(F("[ERROR] DS3231 が見つかりません。配線を確認してください。"));
    while (true) delay(1000);
  }

  if (rtc.lostPower()) {
    // コイン電池切れ or 初回起動 → コンパイル時刻で初期化
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    Serial.println(F("[RTC] 電源喪失を検出 → コンパイル時刻でセットしました"));
  }

  float temp = rtc.getTemperature();
  Serial.println(F("\n[STEP1+2] DS3231 動作確認"));
  Serial.print(F("[RTC] 温度: ")); Serial.print(temp); Serial.println(F(" °C"));
  printHelp();
}

void loop() {
  // シリアルコマンド処理
  if (Serial.available()) {
    char c = (char)Serial.read();

    if (c == 'S' || c == 's') {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      Serial.println(F("[RTC] コンパイル時刻にセットしました（数分ずれる場合は T コマンドを使用）"));

    } else if (c == 'T' || c == 't') {
      // Unix タイムスタンプ入力: T1749034800<Enter>
      char numBuf[12] = {0};
      uint8_t idx = 0;
      uint32_t start = millis();
      while (idx < 10 && millis() - start < 5000) {
        if (Serial.available()) {
          char d = (char)Serial.read();
          if (d >= '0' && d <= '9') {
            numBuf[idx++] = d;
          } else {
            break;  // Enter や改行で終了
          }
        }
      }
      numBuf[idx] = '\0';

      if (idx >= 9) {
        uint32_t unix_t = (uint32_t)atol(numBuf);
        rtc.adjust(DateTime(unix_t));
        // JST で確認表示
        DateTime jst_now = DateTime(unix_t + TZ_OFFSET_SEC);
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:%02d",
          jst_now.year(), jst_now.month(), jst_now.day(),
          jst_now.hour(), jst_now.minute(), jst_now.second());
        Serial.printf("[RTC] Unix=%lu → %s JST にセットしました\n", unix_t, buf);
      } else {
        Serial.println(F("[RTC] タイムスタンプが短すぎます（10桁必要）"));
      }

    } else if (c == '?') {
      printHelp();
    }
  }

  // DS3231 は UTC で保存 → JST に変換して表示
  DateTime utc = rtc.now();
  DateTime jst = DateTime(utc.unixtime() + TZ_OFFSET_SEC);

  char timeBuf[32];
  snprintf(timeBuf, sizeof(timeBuf),
    "%04d/%02d/%02d %02d:%02d:%02d",
    jst.year(), jst.month(), jst.day(),
    jst.hour(), jst.minute(), jst.second());

  Serial.print(F("[RTC] "));
  Serial.print(timeBuf);
  Serial.print(F(" JST  UNIX="));
  Serial.print(utc.unixtime());   // UNIX は UTC のまま表示
  Serial.print(F("  Temp="));
  Serial.print(rtc.getTemperature(), 1);
  Serial.println(F(" °C"));

  delay(1000);
}
