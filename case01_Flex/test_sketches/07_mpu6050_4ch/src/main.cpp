/**
 * Monita Flex v3.2 — 検証 Step7: TCA9546A経由 DS3231 4ch読み取り
 *
 * 確認内容:
 *   TCA9546A（0x70）の CH0〜CH3 を順に切り替えながら
 *   各チャンネル下流の DS3231 から時刻・温度を読み取る
 *
 * 前提:
 *   DS3231 を接続するチャンネル（CH0〜CH3）に配線すること
 *   DS3231 のアドレスは固定 0x68
 *
 * 配線:
 *   XIAO SDA/SCL → TCA9546A SDA/SCL（0x70）
 *   TCA9546A CH0 → DS3231 #0 SDA/SCL
 *   TCA9546A CH1 → DS3231 #1 SDA/SCL（未接続でも「応答なし」で継続）
 *   TCA9546A CH2 → DS3231 #2 SDA/SCL
 *   TCA9546A CH3 → DS3231 #3 SDA/SCL
 *
 * 期待出力例:
 *   [CH0] DS3231: OK
 *     時刻: 2026/06/04 15:30:00 JST
 *     温度: 27.5 ℃
 *   [CH1] DS3231: 応答なし（未接続）
 *   ...
 *
 * 注意:
 *   TCA9546A は 3V3_SW 側の電源で動く。
 *   D10 HIGH にしてから Wire.begin() すること。
 */

#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>

static const uint8_t SW_POWER_PIN  = 10;
static const uint8_t TCA9546A_ADDR = 0x70;
static const uint8_t DS3231_ADDR   = 0x68;

static const int32_t TZ_OFFSET_SEC = 9 * 3600L;  // JST = UTC+9

RTC_DS3231 rtc;

// TCA9546A: チャンネル ch（0〜3）を有効にする
static bool tcaSelect(uint8_t ch) {
  Wire.beginTransmission(TCA9546A_ADDR);
  Wire.write(1 << ch);
  return Wire.endTransmission() == 0;
}

// TCA9546A: 全チャンネル切断
static void tcaDisable() {
  Wire.beginTransmission(TCA9546A_ADDR);
  Wire.write(0x00);
  Wire.endTransmission();
}

// DS3231 の応答確認
static bool dsPresent() {
  Wire.beginTransmission(DS3231_ADDR);
  return Wire.endTransmission() == 0;
}

// DS3231 から時刻・温度を読み取って出力
static void dsRead() {
  if (!rtc.begin(&Wire)) {
    Serial.println(F("    rtc.begin() 失敗"));
    return;
  }

  DateTime utc = rtc.now();
  DateTime jst = DateTime(utc.unixtime() + TZ_OFFSET_SEC);

  char buf[32];
  snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:%02d",
    jst.year(), jst.month(), jst.day(),
    jst.hour(), jst.minute(), jst.second());

  Serial.print(F("    時刻: "));
  Serial.print(buf);
  Serial.println(F(" JST"));

  Serial.print(F("    温度: "));
  Serial.print(rtc.getTemperature(), 1);
  Serial.println(F(" ℃"));
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000);

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  Wire.begin();

  Serial.println(F("\n[STEP7] TCA9546A経由 DS3231 4ch読み取り"));
  Serial.println(F("各チャンネル下流の DS3231 から時刻・温度を読み取ります"));
  Serial.println(F("---------------------------------------------------"));

  // TCA9546A 自体の応答確認
  Wire.beginTransmission(TCA9546A_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println(F("[TCA9546A] 応答なし → 配線/電源(3V3_SW)を確認"));
    while (true) delay(1000);
  }
  Serial.println(F("[TCA9546A] 応答OK"));
  tcaDisable();
  delay(100);
}

static uint32_t cycleCount = 0;

void loop() {
  cycleCount++;
  Serial.print(F("\n=== 読み取りサイクル #"));
  Serial.print(cycleCount);
  Serial.println(F(" ==="));

  for (uint8_t ch = 0; ch < 4; ch++) {
    Serial.print(F("[CH"));
    Serial.print(ch);
    Serial.print(F("] DS3231: "));

    if (!tcaSelect(ch)) {
      Serial.println(F("TCA9546A チャンネル選択失敗"));
      continue;
    }
    delay(10);

    if (!dsPresent()) {
      Serial.println(F("応答なし（未接続）"));
    } else {
      Serial.println(F("OK"));
      dsRead();
    }

    tcaDisable();
    delay(50);
  }

  Serial.println(F("--- 3秒後に再読み取り ---"));
  delay(3000);
}
