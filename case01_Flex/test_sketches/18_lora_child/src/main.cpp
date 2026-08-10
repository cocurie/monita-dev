/**
 * Monita LoRa 検証 Step18: 子機（送信側）— Flex ver3.10 基板 E220-900T22S(JP) 送信テスト
 *
 * 【目的】
 *   Flex ver3.10 実機（本番ファーム v3.10_lora/src/main.cpp と同じLoRa送信経路：
 *   TCA9534経由のM0/M1制御・設定確認/書込・フレーム化）を使って、Gateway ver1.1
 *   （19_lora_parent）への電波強度連続受信テスト用に、ダミーのセンサー値を
 *   周期送信するだけの最小構成。HX711/MPU/Sigfox等、LoRa送信に無関係な機能は
 *   一切含まない。
 *
 * 【対象ハード】Flex ver3.10 基板（ブレッドボードのXIAO+E220直結ではない）
 *   XIAO D8 (Serial1 TX) → BRKLSM100/E220 RXD（本番ファームと共用のUARTネット）
 *   XIAO D9 (Serial1 RX) ← E220 TXD
 *   E220 M0・M1          → TCA9534(U6, I2C 0x20) の P2 で共通駆動
 *                           （基板上でM0/M1短絡・v3.10新規配線。ver3.10.sch確認済み）
 *
 * 【動作】
 *   2秒ごとに、gateway_v1.1本番ファームと同じフレーム形式で送信する:
 *     [0]SYNC=0xAA [1]LEN [2..LEN+1]MSDペイロード [LEN+2]チェックサム(単純和)
 *   MSDペイロードは本番同一レイアウト（19バイト）:
 *     [0]PktType(0x04) [1]DeviceID [2]FWVersion [3-10]CH1-4(int16 LE、ダミー値)
 *     [11-12]BATT(mV、ダミー値) [13]Hour [14]Min [15-18]CH1-4 Range(ダミー値)
 *   起動毎（および送信毎）にE220の設定確認（config read）を行い、想定値と違えば
 *   書き込む（本番の loraCheckAndConfigure() と同一ロジック）。
 *
 * 【前提・注意】
 *   - 19_lora_parent（Gateway ver1.1向け）と対で使う。
 *   - E220のレジスタ想定値は本番ファーム・Gateway側と共通（[[e220_register_map_and_bug]]
 *     参照）: ADDH=00 ADDL=00 REG0=68 REG1=01 REG2=00 REG3=80。
 *   - 以前のブレッドボード版（M0/M1をGND直結し、テキストプレフィックス送信）とは
 *     配線・プロトコルとも非互換。Flex ver3.10基板ではM0/M1はTCA9534経由必須。
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_TinyUSB.h>  // USB CDC（Serial）。USE_TINYUSBビルド時に必要

#define LORA_TX_PIN 8   // D8: Serial1 TX（本番と同じSigfox/LoRa共用ネット）
#define LORA_RX_PIN 9   // D9: Serial1 RX
#define LORA_BAUD   9600
#define SEND_INTERVAL_MS 2000UL

// ★2026-08-07追加（切り分け用）: 1 にすると送信の合間に受信バイトをそのままダンプする。
//   Flex基板が1台しかなく「Gatewayの送信」と「Flexの受信」の検証が循環していたため、
//   送信が実証済みのこのスケッチに受信ダンプだけを足して双方向テストできるようにした。
//   相手は 19_lora_parent（TX_PING_ENABLED=1 で定期送信するようにしたもの）。
//   切り分けが済んだら 0 に戻してよい。
#define RX_DUMP_ENABLED 1
static uint32_t s_rxDumpBytes = 0;

// D10 = MOSFET_GATE → 3V3_SW ON/OFF（HIGHで周辺レール給電。本番 v3.10_lora/main.cpp と同一）
// ★これをONにしないとTCA9534・E220に電源が入らず、Wire（I2C）アクセスが
//   応答なしのまま永久にハングする（Adafruit nRF52コアのWireはタイムアウトを
//   持たないため。[[nrf52_i2c_buslock_recovery]]参照）。ハング中はSerial出力も
//   LED点灯も一切起きないため、「シリアルモニタに何も出ない」症状の典型原因。
#define SW_POWER_PIN 10

// ★書き込み前に必ず変更: 複数台テストなら子機ごとに重複しないユニークな値にする
static const uint8_t DEVICE_ID = 0x0E;
static const uint8_t FW_VERSION_DUMMY = 1;

// ── TCA9534（U6）— LoRa M0/M1 共通駆動（本番 v3.10_lora/main.cpp から移植） ──
#define TCA9534_ADDR 0x20

static bool tca9534WriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool tca9534ReadReg(uint8_t reg, uint8_t *out) {
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TCA9534_ADDR, (uint8_t)1) != 1) return false;
  *out = Wire.read();
  return true;
}

// P0〜P2 を出力（P0/P1=MUX A/B、本テストでは未使用、P2=LoRa M0/M1共通）、P3〜P7 を入力
static bool tca9534Configure() {
  if (!tca9534WriteReg(0x02, 0x00)) return false;   // Polarity: 反転なし
  if (!tca9534WriteReg(0x03, 0xF8)) return false;   // Config: P0-2出力/P3-7入力
  uint8_t out = 0;
  if (!tca9534ReadReg(0x01, &out)) return false;
  out = (uint8_t)((out & (uint8_t)~0x07U) | 0x00U);  // A=0,B=0,P2=0（E220 Mode0）で初期化
  return tca9534WriteReg(0x01, out);
}

// TCA9534 P2（=E220 M0・M1 共通駆動）を high/low に駆動する
// （両LOW=Mode0通常送受信／両HIGH=Mode3設定モード。本番 loraSetMode() と同一）
static bool loraSetMode(bool high) {
  uint8_t out = 0;
  if (!tca9534ReadReg(0x01, &out)) return false;
  out = (uint8_t)((out & (uint8_t)~0x04U) | (high ? 0x04U : 0U));  // P2 = bit2
  if (!tca9534WriteReg(0x01, out)) return false;
  delay(100);  // M0/M1切替後の安定待ち
  return true;
}
static inline bool loraModeNormal() { return loraSetMode(false); }
static inline bool loraModeConfig() { return loraSetMode(true); }

// ── E220設定確認・書込（本番 loraCheckAndConfigure() から移植） ──
#define LORA_CFG_REG_START 0x00
#define LORA_CFG_REG_LEN   6
static const uint8_t LORA_CFG_ADDH = 0x00;
static const uint8_t LORA_CFG_ADDL = 0x00;
static const uint8_t LORA_CFG_REG0 = 0x68;  // UART9600bps + エア速度(SF7/BW125kHz)
static const uint8_t LORA_CFG_REG1 = 0x01;  // 送信出力13dBm
static const uint8_t LORA_CFG_REG2 = 0x00;  // チャンネル0
static const uint8_t LORA_CFG_REG3 = 0x80;  // RSSIバイト有効化ON/透過送信モード（Gatewayと統一）

static bool loraReadConfig(uint8_t *out6) {
  unsigned long drainStart = millis();
  while (Serial1.available()) {
    Serial1.read();
    if (millis() - drainStart > 300UL) break;
  }
  Serial1.write((uint8_t)0xC1);
  Serial1.write((uint8_t)LORA_CFG_REG_START);
  Serial1.write((uint8_t)LORA_CFG_REG_LEN);

  const int respLen = 3 + LORA_CFG_REG_LEN;
  uint8_t resp[3 + LORA_CFG_REG_LEN];
  int idx = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 500UL && idx < respLen) {
    if (Serial1.available()) resp[idx++] = (uint8_t)Serial1.read();
  }
  if (idx < respLen) return false;
  if (resp[0] != 0xC1) return false;
  memcpy(out6, &resp[3], LORA_CFG_REG_LEN);
  return true;
}

static void loraWriteConfig() {
  Serial1.write((uint8_t)0xC0);
  Serial1.write((uint8_t)LORA_CFG_REG_START);
  Serial1.write((uint8_t)LORA_CFG_REG_LEN);
  Serial1.write(LORA_CFG_ADDH);
  Serial1.write(LORA_CFG_ADDL);
  Serial1.write(LORA_CFG_REG0);
  Serial1.write(LORA_CFG_REG1);
  Serial1.write(LORA_CFG_REG2);
  Serial1.write(LORA_CFG_REG3);
  delay(200);
  unsigned long t0 = millis();
  while (millis() - t0 < 300UL) { while (Serial1.available()) Serial1.read(); }
}

static void loraPrintRegs(const char* label, const uint8_t regs[LORA_CFG_REG_LEN]) {
  Serial.print(F("[LORA] ")); Serial.print(label); Serial.print(F(": "));
  for (int i = 0; i < LORA_CFG_REG_LEN; i++) {
    if (regs[i] < 0x10) Serial.print('0');
    Serial.print(regs[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
}

static bool loraCheckAndConfigure() {
  if (!loraModeConfig()) return false;

  uint8_t cur[LORA_CFG_REG_LEN] = {0};
  bool readOk = loraReadConfig(cur);
  bool matches = readOk &&
      cur[0] == LORA_CFG_ADDH && cur[1] == LORA_CFG_ADDL &&
      cur[2] == LORA_CFG_REG0 && cur[3] == LORA_CFG_REG1 &&
      cur[4] == LORA_CFG_REG2 && cur[5] == LORA_CFG_REG3;

  Serial.print(F("[LORA] config read "));
  Serial.println(!readOk ? F("失敗") : (matches ? F("一致") : F("不一致→書込")));
  if (readOk) {
    loraPrintRegs("実測値(読込)", cur);
    uint8_t expected[LORA_CFG_REG_LEN] = {LORA_CFG_ADDH, LORA_CFG_ADDL, LORA_CFG_REG0,
                                           LORA_CFG_REG1, LORA_CFG_REG2, LORA_CFG_REG3};
    loraPrintRegs("期待値      ", expected);
  }

  if (!readOk) { loraModeNormal(); return false; }

  if (!matches) {
    bool verifyOk = false;
    for (int attempt = 1; attempt <= 2 && !verifyOk; attempt++) {
      loraWriteConfig();
      uint8_t verify[LORA_CFG_REG_LEN] = {0};
      bool verifyReadOk = loraReadConfig(verify);
      verifyOk = verifyReadOk &&
          verify[0] == LORA_CFG_ADDH && verify[1] == LORA_CFG_ADDL &&
          verify[2] == LORA_CFG_REG0 && verify[3] == LORA_CFG_REG1 &&
          verify[4] == LORA_CFG_REG2 && verify[5] == LORA_CFG_REG3;

      Serial.print(F("[LORA] config write 確認("));
      Serial.print(attempt); Serial.print(F("/2): "));
      Serial.println(verifyOk ? F("OK") : F("NG"));
      if (verifyReadOk) loraPrintRegs("書込後の実測値", verify);
      else              Serial.println(F("[LORA] 書込後の読込自体に失敗（応答なし）"));
    }
    if (!verifyOk) {
      Serial.println(F("[LORA] 2回とも書込確認NG（配線・電源を確認）"));
      loraModeNormal();
      return false;
    }
  }

  return loraModeNormal();
}

// 透過モードでフレームを送信する（[SYNC][LEN][payload...][checksum]）
static void loraSendFrame(const uint8_t *msd, uint8_t msdLen) {
  uint8_t sum = (uint8_t)(0xAAU + msdLen);
  Serial1.write((uint8_t)0xAA);
  Serial1.write(msdLen);
  for (uint8_t i = 0; i < msdLen; i++) {
    Serial1.write(msd[i]);
    sum = (uint8_t)(sum + msd[i]);
  }
  Serial1.write(sum);
}

// ★診断用: 送信の瞬間を目視確認するためのLED点灯。
static void ledInit() {
#ifdef LED_RED
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  digitalWrite(LED_RED, HIGH);
  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(LED_BLUE, HIGH);
#endif
}
static void ledGreen(bool on) {
#ifdef LED_GREEN
  digitalWrite(LED_GREEN, on ? LOW : HIGH);
#endif
}

static uint32_t s_counter = 0;
static uint32_t s_lastSendMs = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) yield();

  ledInit();

  Serial.println(F("\n[Step18] LoRa子機（送信側）— Flex ver3.10 基板テスト起動"));
  Serial.print(F("DeviceID=0x")); Serial.println(DEVICE_ID, HEX);

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);  // 3V3_SW ON（TCA9534・E220へ給電）
  delay(50);  // 電源安定待ち

  Wire.begin();
  if (!tca9534Configure()) {
    Serial.println(F("✗ TCA9534初期化に失敗（I2C配線・電源を確認してください）"));
  }

  Serial1.setPins(LORA_RX_PIN, LORA_TX_PIN);
  Serial1.begin(LORA_BAUD);
  delay(500);  // E220 起動待ち

  Serial.print(F("[LoRa] UART開始 baud=")); Serial.println(LORA_BAUD);
  Serial.println(F("[LoRa] 送信開始（2秒間隔、Flex本番と同じフレーム形式）"));
}

void loop() {
  uint32_t now = millis();
  if (now - s_lastSendMs >= SEND_INTERVAL_MS) {
    s_lastSendMs = now;
    s_counter++;

    if (!loraCheckAndConfigure()) {
      Serial.println(F("[LORA] config check失敗、送信スキップ"));
      return;
    }

    // ダミーのセンサー値（電波強度確認が目的のため実測値である必要はない）
    int16_t ch[4] = {
      (int16_t)(100 + (s_counter % 50)),
      (int16_t)(200 + (s_counter % 50)),
      (int16_t)(300 + (s_counter % 50)),
      (int16_t)(400 + (s_counter % 50)),
    };
    uint16_t battDummy = 3300;  // mV
    uint8_t hourDummy = (uint8_t)((s_counter / 30) % 24);
    uint8_t minDummy  = (uint8_t)((s_counter) % 60);
    uint8_t rangeDummy[4] = {10, 10, 10, 10};

    uint8_t msd[19];
    msd[0] = 0x04;  // Pkt type: Monita Flex v3.10 LoRa（本番と同一値）
    msd[1] = DEVICE_ID;
    msd[2] = FW_VERSION_DUMMY;
    for (int i = 0; i < 4; i++) {
      msd[3 + i * 2]     = (uint8_t)(ch[i] & 0xFF);
      msd[3 + i * 2 + 1] = (uint8_t)((ch[i] >> 8) & 0xFF);
    }
    msd[11] = (uint8_t)(battDummy & 0xFF);
    msd[12] = (uint8_t)((battDummy >> 8) & 0xFF);
    msd[13] = hourDummy;
    msd[14] = minDummy;
    for (int i = 0; i < 4; i++) msd[15 + i] = rangeDummy[i];

    ledGreen(true);
    loraSendFrame(msd, sizeof(msd));
    delay(300);  // 送信完了待ち（本番と同じ固定ディレイ）
    ledGreen(false);

    Serial.print(F("[TX] #")); Serial.print(s_counter);
    Serial.print(F(" DeviceID=0x")); Serial.print(DEVICE_ID, HEX);
    Serial.print(F(" CH=")); for (int i = 0; i < 4; i++) { Serial.print(ch[i]); Serial.print(' '); }
    Serial.println();
  }

#if RX_DUMP_ENABLED
  // ★2026-08-07追加: 送信の合間に受信バイトをそのままダンプする（切り分け用）。
  //
  // 【なぜこのスケッチに足すのか】
  //   Flex基板が1台しかないため、「Gatewayの送信」と「Flexの受信」を
  //   別々に検証できず切り分けが循環していた。このスケッチは
  //   「Flexからの送信が確実に電波に乗る」ことが実証済みなので、
  //   ここに受信ダンプだけを足せば、Gateway側(19_lora_parent + TX追加)が
  //   受信できているかを見ながら、同時にFlexの受信可否も確認できる。
  //   フレーム解析はせず生バイトをそのまま出す（届いてさえいれば必ず見える）。
  while (Serial1.available()) {
    uint8_t b = (uint8_t)Serial1.read();
    s_rxDumpBytes++;
    Serial.print(F("[RX raw] 0x"));
    if (b < 0x10) Serial.print('0');
    Serial.print(b, HEX);
    Serial.print(F("  (累計="));
    Serial.print(s_rxDumpBytes);
    Serial.println(F(")"));
  }
#endif

  yield();
}
