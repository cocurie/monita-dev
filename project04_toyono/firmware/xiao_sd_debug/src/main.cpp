/**
 * Monita Flex v3.04 — SD デバッグファーム（再作成版）
 *
 * シリアル(115200bps)でコマンドを送って各ステップを個別に実行する。
 *
 *   1  TCA9534 初期化 + レジスタ読み戻し
 *   2  3V3_SW (P2) ON/OFF トグル → SD VDD(J2 pin4) をテスターで実測
 *   3  SD CS (P3) HIGH/LOW トグル → SD pin2 を確認
 *   4  ダミークロック送信（CS=LOW固定, 0xFF x10）→ MISO 受信値ダンプ
 *   5  CMD0 手動送信 → 送受信バイト逐次ダンプ
 *   6  SdFat フル初期化（CS常時LOW固定 / BLEなし）
 *   9  現在のピン状態表示
 *   0  ヘルプ
 *
 * 配線 (v3.04):
 *   I2C(ビットバン): SDA=D4  SCL=D5   TCA9534 addr=0x20
 *   SPI: SCK=D10  MOSI=D1  MISO=D2
 *   SD CS は TCA9534 P3 が能動駆動
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <SPI.h>
#include <SdFat.h>

// ── I2C (ビットバン) ──────────────────
#define I2C_SDA       D4
#define I2C_SCL       D5
#define TCA9534_ADDR  0x20

static void sdaHi() { pinMode(I2C_SDA, INPUT_PULLUP); delayMicroseconds(5); }
static void sdaLo() { pinMode(I2C_SDA, OUTPUT); digitalWrite(I2C_SDA, LOW); delayMicroseconds(5); }
static void sclHi() { digitalWrite(I2C_SCL, HIGH); delayMicroseconds(5); }
static void sclLo() { digitalWrite(I2C_SCL, LOW);  delayMicroseconds(5); }

static void i2cInit() { pinMode(I2C_SCL, OUTPUT); sclHi(); sdaHi(); delayMicroseconds(50); }
static void i2cStart() { sdaHi(); sclHi(); sdaLo(); sclLo(); }
static void i2cStop()  { sdaLo(); sclHi(); sdaHi(); }

static bool i2cWriteByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    if ((b >> i) & 1) sdaHi(); else sdaLo();
    sclHi(); sclLo();
  }
  sdaHi(); sclHi();
  bool ack = (digitalRead(I2C_SDA) == LOW);
  sclLo();
  return ack;
}

static uint8_t i2cReadByte(bool sendAck) {
  uint8_t b = 0;
  sdaHi();
  for (int i = 7; i >= 0; i--) {
    sclHi();
    b = (uint8_t)((b << 1) | (digitalRead(I2C_SDA) ? 1 : 0));
    sclLo();
  }
  if (sendAck) sdaLo(); else sdaHi();
  sclHi(); sclLo(); sdaHi();
  return b;
}

static bool tca9534WriteReg(uint8_t reg, uint8_t val) {
  i2cStart();
  bool ok = i2cWriteByte((TCA9534_ADDR << 1) | 0x00);
  ok &= i2cWriteByte(reg);
  ok &= i2cWriteByte(val);
  i2cStop();
  return ok;
}

static bool tca9534ReadReg(uint8_t reg, uint8_t &out) {
  i2cStart();
  bool ok = i2cWriteByte((TCA9534_ADDR << 1) | 0x00);
  ok &= i2cWriteByte(reg);
  i2cStart();
  ok &= i2cWriteByte((TCA9534_ADDR << 1) | 0x01);
  out = i2cReadByte(false);
  i2cStop();
  return ok;
}

static uint8_t s_tca9534Out = 0x04; // P2=1(3V3_SW ON) P3=0(CS LOW)

static bool tca9534SetBit(uint8_t bit, uint8_t val) {
  if (val) s_tca9534Out |=  (1u << bit);
  else     s_tca9534Out &= ~(1u << bit);
  return tca9534WriteReg(0x01, s_tca9534Out);
}

static bool tca9534Init() {
  if (!tca9534WriteReg(0x02, 0x00)) return false;
  if (!tca9534WriteReg(0x03, 0xF3)) return false;
  s_tca9534Out = 0x04; // P2=HIGH(3V3_SW ON), P3=LOW(CS assert)
  return tca9534WriteReg(0x01, s_tca9534Out);
}

// ── SPI ──────────────────────────────
#define PIN_SD_MISO  D2
#define PIN_SD_SCK   D10
#define PIN_SD_MOSI  D1

static SPIClass SD_SPI(NRF_SPIM2, PIN_SD_MISO, PIN_SD_SCK, PIN_SD_MOSI);
static bool s_spiStarted = false;

static void spiStartIfNeeded() {
  if (!s_spiStarted) { SD_SPI.begin(); s_spiStarted = true; }
}

static uint8_t spiXfer(uint8_t b) {
  SD_SPI.beginTransaction(SPISettings(1000000UL, MSBFIRST, SPI_MODE0));
  uint8_t r = SD_SPI.transfer(b);
  SD_SPI.endTransaction();
  return r;
}

// ── コマンド ──────────────────────────

static void printHelp() {
  Serial.println();
  Serial.println("=== SD デバッグメニュー ===");
  Serial.println(" 1  TCA9534 初期化 + レジスタ読み戻し");
  Serial.println(" 2  3V3_SW(P2) トグル → J2 pin4 をテスターで実測");
  Serial.println(" 3  CS(P3) トグル → J2 pin2 を確認");
  Serial.println(" 4  ダミークロック(0xFF x10, CS=LOW固定) → MISO ダンプ");
  Serial.println(" 5  CMD0 手動送信 → 送受信バイトダンプ");
  Serial.println(" 6  SdFat フル初期化（CS常時LOW / BLEなし）");
  Serial.println(" 9  ピン状態表示");
  Serial.println("---------------------------------------------------");
}

static void cmd1() {
  Serial.println("[1] TCA9534 初期化...");
  if (!tca9534Init()) { Serial.println("    [ERROR] ACK無し"); return; }
  Serial.println("    [OK]  P2=HIGH(3V3_SW ON)  P3=LOW(CS assert)");
  uint8_t in = 0;
  if (tca9534ReadReg(0x00, in)) {
    Serial.print("    [READBACK] Input=0x"); Serial.println(in, HEX);
    Serial.print("      P2(3V3_SW)="); Serial.println((in & 0x04) ? "HIGH" : "LOW");
    Serial.print("      P3(CS)    ="); Serial.println((in & 0x08) ? "HIGH(deassert)" : "LOW(assert)");
  } else {
    Serial.println("    [ERROR] 読み戻し失敗");
  }
}

static void cmd2() {
  bool next = !(s_tca9534Out & 0x04);
  Serial.print("[2] 3V3_SW(P2) → "); Serial.println(next ? "HIGH(ON)" : "LOW(OFF)");
  tca9534SetBit(2, next ? 1 : 0);
  Serial.println("    → J2 pin4(SD VDD)をテスターで実測: HIGH=約3.3V / LOW=約0V");
}

static void cmd3() {
  bool next = !(s_tca9534Out & 0x08);
  Serial.print("[3] CS(P3) → "); Serial.println(next ? "HIGH(deassert)" : "LOW(assert)");
  tca9534SetBit(3, next ? 1 : 0);
}

static void cmd4() {
  Serial.println("[4] ダミークロック(CS=LOW固定, 0xFF x10)");
  tca9534SetBit(3, 0);
  delay(2);
  spiStartIfNeeded();
  Serial.print("    MISO受信: ");
  for (int i = 0; i < 10; i++) {
    uint8_t r = spiXfer(0xFF);
    Serial.print("0x"); if (r < 0x10) Serial.print("0"); Serial.print(r, HEX); Serial.print(" ");
  }
  Serial.println();
  Serial.println("    全て0xFF → カード応答なし / 0xFF以外あり → MISO到達中");
}

static void cmd5() {
  Serial.println("[5] CMD0 手動送信");
  spiStartIfNeeded();
  tca9534SetBit(3, 1); delay(5);
  Serial.print("    [プリクロック CS=HIGH]: ");
  for (int i = 0; i < 10; i++) {
    uint8_t r = spiXfer(0xFF);
    Serial.print("0x"); if (r < 0x10) Serial.print("0"); Serial.print(r, HEX); Serial.print(" ");
  }
  Serial.println();
  tca9534SetBit(3, 0); delay(2);
  uint8_t cmd0[6] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
  Serial.print("    [CMD0送信]: ");
  for (int i = 0; i < 6; i++) {
    uint8_t r = spiXfer(cmd0[i]);
    Serial.print("TX=0x"); if (cmd0[i]<0x10) Serial.print("0"); Serial.print(cmd0[i], HEX);
    Serial.print("/RX=0x"); if (r<0x10) Serial.print("0"); Serial.print(r, HEX); Serial.print(" ");
  }
  Serial.println();
  Serial.print("    [R1応答]: ");
  uint8_t resp = 0xFF;
  for (int i = 0; i < 8; i++) {
    resp = spiXfer(0xFF);
    Serial.print("0x"); if (resp < 0x10) Serial.print("0"); Serial.print(resp, HEX); Serial.print(" ");
    if ((resp & 0x80) == 0) break;
  }
  Serial.println();
  if      (resp == 0x01) Serial.println("    [OK] R1=0x01 カード正常応答！");
  else if (resp == 0xFF) Serial.println("    [NG] 応答なし(0xFF) → 電源/CS/SCK/カードを確認");
  else                   { Serial.print("    [?] 想定外: 0x"); Serial.println(resp, HEX); }
  tca9534SetBit(3, 1);
}

static SdFat sd;

static void cmd6() {
  Serial.println("[6] SdFat フル初期化（CS常時LOW / BLEなし）");
  tca9534SetBit(3, 0); // CS 永続 LOW
  delay(500);
  pinMode(D9, OUTPUT); digitalWrite(D9, HIGH);
  SdSpiConfig cfg(D9, DEDICATED_SPI, SD_SCK_MHZ(4), &SD_SPI);
  if (sd.begin(cfg)) {
    Serial.print("    [OK] init成功  capacity=");
    Serial.print((uint32_t)(0.000512f * sd.card()->sectorCount()));
    Serial.println("MB");
  } else {
    Serial.print("    [NG] init失敗  err=0x");
    Serial.print(sd.card()->errorCode(), HEX);
    Serial.print("/0x");
    Serial.println(sd.card()->errorData(), HEX);
  }
  tca9534SetBit(3, 1);
}

static void cmd9() {
  Serial.println("[9] 現在の状態");
  Serial.print("    TCA9534出力キャッシュ: 0x"); Serial.println(s_tca9534Out, HEX);
  Serial.print("    P2(3V3_SW)="); Serial.println((s_tca9534Out & 0x04) ? "HIGH" : "LOW");
  Serial.print("    P3(CS)    ="); Serial.println((s_tca9534Out & 0x08) ? "HIGH(deassert)" : "LOW(assert)");
  Serial.print("    SPI開始済み: "); Serial.println(s_spiStarted ? "Yes" : "No");
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();
  i2cInit();
  Serial.println("\n=== Monita Flex v3.04 SD デバッグファーム ===");

  // ── BLE 有無切り分けテスト: 本番 initSd() と全く同じ手順を自動実行 ──
  // 本ファームは bluefruit.h を一切リンクしていない。cold boot でこれが
  // 成功すれば、本番の初期化失敗原因は BLE(SoftDevice/リンカスクリプト)側にある。
  Serial.println("[AUTO] BLE無し cold boot SD init テスト開始");
  if (!tca9534Init()) {
    Serial.println("[AUTO] TCA9534 init failed");
  } else {
    Serial.println("[AUTO] TCA9534 OK  P2=HIGH(3V3_SW ON)  P3=LOW(CS assert)");
    pinMode(PIN_SD_SCK,  OUTPUT); digitalWrite(PIN_SD_SCK,  LOW);
    pinMode(PIN_SD_MOSI, OUTPUT); digitalWrite(PIN_SD_MOSI, HIGH);
    pinMode(PIN_SD_MISO, INPUT_PULLUP);
    uint32_t t0 = millis();
    while (!digitalRead(PIN_SD_MISO) && millis() - t0 < 3000) delay(10);
    Serial.print("[AUTO] MISO="); Serial.print(digitalRead(PIN_SD_MISO) ? "HIGH(OK)" : "LOW(still!)");
    Serial.print("  after "); Serial.print(millis() - t0); Serial.println("ms");
    delay(500);
    pinMode(D9, OUTPUT); digitalWrite(D9, HIGH);
    SdSpiConfig cfg(D9, DEDICATED_SPI, SD_SCK_MHZ(4), &SD_SPI);
    if (sd.begin(cfg)) {
      Serial.print("[AUTO] SD init OK  capacity=");
      Serial.print((uint32_t)(0.000512f * sd.card()->sectorCount()));
      Serial.println("MB");
    } else {
      Serial.print("[AUTO] SD init FAILED  err=0x");
      Serial.print(sd.card()->errorCode(), HEX);
      Serial.print("/0x");
      Serial.println(sd.card()->errorData(), HEX);
    }
  }
  Serial.println("[AUTO] テスト終了\n");

  printHelp();
}

void loop() {
  if (Serial.available()) {
    switch (Serial.read()) {
      case '0': printHelp(); break;
      case '1': cmd1(); break;
      case '2': cmd2(); break;
      case '3': cmd3(); break;
      case '4': cmd4(); break;
      case '5': cmd5(); break;
      case '6': cmd6(); break;
      case '9': cmd9(); break;
    }
  }
}
