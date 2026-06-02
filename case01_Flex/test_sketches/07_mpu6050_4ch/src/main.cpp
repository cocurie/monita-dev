/**
 * Monita Flex v3.02 — 検証 Step7: TCA9546A経由 MPU6050 4ch読み取り
 *
 * 確認内容:
 *   TCA9546A（0x70）の CH0〜CH3 を順に切り替えながら
 *   各チャンネル下流の MPU6050 から加速度・ジャイロを読み取る
 *
 * 前提:
 *   Step6（TCA9546A チャンネル切り替え）がパスしていること
 *   MPU6050 を各 CH（CH0〜CH3）に接続すること
 *   全てのMPU6050のAD0ピンはGNDに落としてアドレスを 0x68 に固定する
 *
 * 配線:
 *   XIAO SDA/SCL → TCA9546A SDA/SCL（0x70）
 *   TCA9546A CH0 → MPU6050 #0 SDA/SCL（AD0=GND → 0x68）
 *   TCA9546A CH1 → MPU6050 #1 SDA/SCL（AD0=GND → 0x68）
 *   TCA9546A CH2 → MPU6050 #2 SDA/SCL（AD0=GND → 0x68）
 *   TCA9546A CH3 → MPU6050 #3 SDA/SCL（AD0=GND → 0x68）
 *
 * 期待出力例:
 *   [CH0] MPU6050:
 *     加速度 ax=-0.02g  ay= 0.01g  az= 1.00g
 *     ジャイロ gx=  0.5dps  gy= -0.3dps  gz=  0.1dps
 *     温度: 25.3 ℃
 *   [CH1] MPU6050: (同様)
 *   ...
 *
 * 未接続のチャンネルは「[CHx] MPU6050: 応答なし」と表示（正常）
 *
 * 注意:
 *   TCA9546A は 3V3_SW 側の電源で動く。
 *   D10 HIGH にしてから Wire.begin() すること。
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <MPU6050.h>

static const uint8_t SW_POWER_PIN  = 10;
static const uint8_t TCA9546A_ADDR = 0x70;
static const uint8_t MPU6050_ADDR  = 0x69;  // AD0=HIGH(VCC接続)の場合。AD0=GNDなら0x68

MPU6050 mpu;

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

// MPU6050 の初期化を試みる（現在選択中のチャンネル）
// 戻り値: true=成功, false=応答なし
static bool mpuInit() {
  // 応答確認
  Wire.beginTransmission(MPU6050_ADDR);
  if (Wire.endTransmission() != 0) return false;

  mpu.initialize();
  return mpu.testConnection();
}

// MPU6050 からデータを読み取って出力
static void mpuRead() {
  int16_t ax_raw, ay_raw, az_raw;
  int16_t gx_raw, gy_raw, gz_raw;
  mpu.getMotion6(&ax_raw, &ay_raw, &az_raw, &gx_raw, &gy_raw, &gz_raw);

  // MPU6050 デフォルト: ±2g → LSB=16384, ±250dps → LSB=131
  float ax = ax_raw / 16384.0f;
  float ay = ay_raw / 16384.0f;
  float az = az_raw / 16384.0f;
  float gx = gx_raw / 131.0f;
  float gy = gy_raw / 131.0f;
  float gz = gz_raw / 131.0f;

  // 温度（MPU6050内蔵センサ）
  float temp = mpu.getTemperature() / 340.0f + 36.53f;

  Serial.print(F("    加速度 ax="));
  Serial.print(ax, 2); Serial.print(F("g  ay="));
  Serial.print(ay, 2); Serial.print(F("g  az="));
  Serial.print(az, 2); Serial.println(F("g"));

  Serial.print(F("    ジャイロ gx="));
  Serial.print(gx, 1); Serial.print(F("dps  gy="));
  Serial.print(gy, 1); Serial.print(F("dps  gz="));
  Serial.print(gz, 1); Serial.println(F("dps"));

  Serial.print(F("    温度: "));
  Serial.print(temp, 1);
  Serial.println(F(" ℃"));
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) yield();

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);
  delay(200);

  Wire.begin();

  Serial.println(F("\n[STEP7] TCA9546A経由 MPU6050 4ch読み取り"));
  Serial.println(F("各チャンネル下流のMPU6050から加速度・ジャイロを読み取ります"));
  Serial.println(F("---------------------------------------------------"));

  // TCA9546A 自体の応答確認
  Wire.beginTransmission(TCA9546A_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println(F("[TCA9546A] 応答なし → Step6を先に完了させること"));
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
    Serial.print(F("] MPU6050: "));

    if (!tcaSelect(ch)) {
      Serial.println(F("TCA9546A チャンネル選択失敗"));
      continue;
    }
    delay(10);

    if (!mpuInit()) {
      Serial.println(F("応答なし（未接続 or 配線確認）"));
    } else {
      Serial.println(F("OK"));
      mpuRead();
    }

    tcaDisable();
    delay(50);
  }

  Serial.println(F("--- 3秒後に再読み取り ---"));
  delay(3000);
}
