/**
 * Monita Deck v1.00 — ブリングアップ用スケッチ（PlatformIO / XIAO nRF52840）
 *
 * 【対象ハード】
 *   Monita Deck 基板 **ver1.00**（2026-09-12 JLCPCB 発注）
 *   正本: 【7】Monita/01_開発/Deck基板/20260906_Monita_Deck_6CH計測ユニット_要件定義.md
 *         ネットリスト BOM_CPL/ver1.00/02_Fusion出力/netlist_ver1.00.txt
 *   回路図・実装と食い違う場合は回路図を正とする。
 *
 * 【このスケッチの範囲】
 *   基板が届いた直後に、順番に確かめるためのもの。計測アルゴリズム（要件 §7.3）は
 *   まだ入っていない。ここで確かめるのは次の5点。
 *
 *     1. TCA9534 が見えるか／初期化順序が正しいか（F-25）
 *     2. CLKIN の 8.000 MHz が出ているか（オシロで確認）
 *     3. ADS131M06 が POR を完了し、ID = 0x26xx を返すか（F-26）
 *     4. 全レジスタのリードバックが一致するか（F-6）
 *     5. DRDY 割込みで 976.56 SPS のフレームが CRC エラーなく取れるか（F-2/F-4/F-5）
 *
 * 【入っているもの】
 *   S1 ADCドライバ / S2 起動確認 / S3 リングバッファ / S4 1秒平均→1分メジアン /
 *   S5 イベント検出（切出し範囲の決定まで）
 *
 * 【まだ入っていないもの】
 *   S6 SD記録・S7 DS3231/時刻復旧・S8 LoRa送信/E220電源制御・S9 ダウンリンク。
 *   段階的に足す。順序は README.md を参照。
 *
 * 【重要な起動順序】要件 F-25 / F-26
 *   TCA9534 は電源投入時に全ポートが入力（ハイインピーダンス）になる。
 *   出力レジスタの初期値が 0xFF のため、**先に方向を出力にすると P0 が一瞬 High になり
 *   VEX が ON になる。** 必ず「出力レジスタ → 方向レジスタ」の順で書くこと。
 *
 *   ADS131M06 は **有効な CLKIN が無いと POR が完了しない。**
 *   CLKIN を出してから SYNC/RESET を解除し、その後にレジスタへ触ること。
 */

#include <Arduino.h>
#include <nrf.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_TinyUSB.h>
#include <ADS131M06.h>
#include <DeckMeasure.h>

// ─────────────────────────────────────────────────────────────
// ピン割当（要件 §5.1.1 / ネットリスト ver1.00 で確認済み）
// ─────────────────────────────────────────────────────────────
static constexpr uint8_t PIN_CS_ADC = D0;   // JP2.1
static constexpr uint8_t PIN_DRDY   = D1;   // JP2.2
static constexpr uint8_t PIN_CLKIN  = D2;   // JP2.3 → U2 pin23 XTAL1/CLKIN
static constexpr uint8_t PIN_CS_SD  = D3;   // JP2.4
// D4=SDA / D5=SCL は Wire が使う
// D6=UART TX / D7=UART RX は Serial1 が使う（E220）
// D8=SCK / D9=MISO / D10=MOSI は SPI が使う

// ─────────────────────────────────────────────────────────────
// TCA9534（U5）— I²C I/O エキスパンダ。要件 §5.1.2
// ─────────────────────────────────────────────────────────────
static constexpr uint8_t TCA9534_ADDR = 0x20;   // A0/A1/A2 = GND

static constexpr uint8_t TCA_REG_INPUT  = 0x00;
static constexpr uint8_t TCA_REG_OUTPUT = 0x01;
static constexpr uint8_t TCA_REG_POLINV = 0x02;
static constexpr uint8_t TCA_REG_CONFIG = 0x03;   // 1 = 入力 / 0 = 出力

// ポート割当
static constexpr uint8_t TCA_P0_EXC_EN     = 0;  // U3(LP5907-2.5) EN。1 = VEX ON
static constexpr uint8_t TCA_P1_LORA_PWR   = 1;  // Q1 ゲート。**0 = ON（P-ch）**
static constexpr uint8_t TCA_P2_LORA_MODE  = 2;  // E220 M0/M1（基板上で短絡）
static constexpr uint8_t TCA_P3_ADC_SYNCRST= 3;  // ADS131M06 SYNC/RESET。0 = リセット
static constexpr uint8_t TCA_P5_TACT_SW    = 5;  // 入力。押下で 0
static constexpr uint8_t TCA_P7_AC_DETECT  = 7;  // 入力。AC 生存で 1

// F-25 の初期値。P0=0(VEX OFF) P1=1(LoRa OFF) P2=0 P3=1(非リセット) P4=0 P5=1 P6=0 P7=1
static constexpr uint8_t TCA_OUTPUT_INIT = 0xAA;
// P5・P7 のみ入力
static constexpr uint8_t TCA_CONFIG_INIT = 0xA0;

static uint8_t s_tcaOutput = TCA_OUTPUT_INIT;   // 出力レジスタのシャドウ

static bool tcaWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool tcaRead(uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(TCA9534_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)TCA9534_ADDR, (uint8_t)1) != 1) return false;
  value = Wire.read();
  return true;
}

static bool tcaSetPort(uint8_t port, bool high) {
  if (high) s_tcaOutput |= (uint8_t)(1u << port);
  else      s_tcaOutput &= (uint8_t)~(1u << port);
  return tcaWrite(TCA_REG_OUTPUT, s_tcaOutput);
}

/** F-25：出力レジスタ → 方向レジスタ の順で初期化する */
static bool tcaBegin() {
  if (!tcaWrite(TCA_REG_OUTPUT, TCA_OUTPUT_INIT)) return false;   // ①先に値
  if (!tcaWrite(TCA_REG_CONFIG, TCA_CONFIG_INIT)) return false;   // ②あとで方向
  s_tcaOutput = TCA_OUTPUT_INIT;
  // 書けたことを読み返して確かめる
  uint8_t cfg = 0;
  return tcaRead(TCA_REG_CONFIG, cfg) && cfg == TCA_CONFIG_INIT;
}

// ─────────────────────────────────────────────────────────────
// CLKIN — nRF52840 PWM で 8.000 MHz を生成（要件 §4.1 案A）
//
// PWM ペリフェラルのクロックは 16 MHz。PRESCALER=DIV_1・COUNTERTOP=2 で
// 周期 2 tick ＝ 8.000 MHz、比較値 1 でデューティ 50% になる。
//
// ★8.192 MHz は 16 MHz 系から作れないため 8.000 MHz とし、
//   fDATA = 8.000e6 / (2 × 4096) = **976.5625 SPS** となる。
//   ノイズは OSR で決まるので 1 kSPS 相当と同一である。
// ─────────────────────────────────────────────────────────────
static constexpr double F_CLKIN_HZ = 8000000.0;
static constexpr uint16_t ADC_OSR_VALUE = 4096;
static constexpr double F_DATA_SPS = F_CLKIN_HZ / (2.0 * ADC_OSR_VALUE);   // 976.5625

static uint16_t s_pwmSeq[1] = { 1 };   // 比較値1（COUNTERTOP=2 の 50%）

static void clkinStart(uint8_t arduinoPin) {
  const uint32_t psel = g_ADigitalPinMap[arduinoPin];

  pinMode(arduinoPin, OUTPUT);
  digitalWrite(arduinoPin, LOW);

  NRF_PWM0->PSEL.OUT[0] = psel;
  NRF_PWM0->PSEL.OUT[1] = 0xFFFFFFFF;
  NRF_PWM0->PSEL.OUT[2] = 0xFFFFFFFF;
  NRF_PWM0->PSEL.OUT[3] = 0xFFFFFFFF;

  NRF_PWM0->ENABLE     = PWM_ENABLE_ENABLE_Enabled << PWM_ENABLE_ENABLE_Pos;
  NRF_PWM0->MODE       = PWM_MODE_UPDOWN_Up        << PWM_MODE_UPDOWN_Pos;
  NRF_PWM0->PRESCALER  = PWM_PRESCALER_PRESCALER_DIV_1 << PWM_PRESCALER_PRESCALER_Pos;
  NRF_PWM0->COUNTERTOP = 2;                       // 16 MHz / 2 = 8.000 MHz
  NRF_PWM0->LOOP       = 0;
  NRF_PWM0->DECODER    = (PWM_DECODER_LOAD_Common       << PWM_DECODER_LOAD_Pos)
                       | (PWM_DECODER_MODE_RefreshCount << PWM_DECODER_MODE_Pos);

  NRF_PWM0->SEQ[0].PTR      = (uint32_t)s_pwmSeq;
  NRF_PWM0->SEQ[0].CNT      = 1;
  NRF_PWM0->SEQ[0].REFRESH  = 0;
  NRF_PWM0->SEQ[0].ENDDELAY = 0;

  NRF_PWM0->TASKS_SEQSTART[0] = 1;
}

// ─────────────────────────────────────────────────────────────
// ADC
// ─────────────────────────────────────────────────────────────
static ADS131M06 adc;
static ADS131M06::Config adcCfg;

// ─────────────────────────────────────────────────────────────
// 計測パイプライン（要件 §7.3）
//   ADC → SampleRing（6秒）
//          ├→ BlockAverager（1秒平均） → MedianWindow<60>（1分メジアン）＝静的変位
//          └→ EventDetector（閾値判定） → 切出し範囲
// ─────────────────────────────────────────────────────────────
static deck::SampleRing        ring;          // 108 kB
static deck::BlockAverager     blockAvg;
static deck::MedianWindow<60>  minuteMedian;
static deck::EventDetector     detector;

// ISR → loop の受け渡し。単一生産者・単一消費者なのでフラグとスロットで足りる
static volatile bool     s_blockReady = false;
static int32_t           s_blockValue[deck::NUM_CH];

static constexpr uint8_t EVQ_LEN = 4;         // 発火が連続したときの取りこぼし防止
static volatile uint8_t  s_evHead = 0, s_evTail = 0;
static deck::EventWindow s_evQueue[EVQ_LEN];
static volatile uint16_t s_evOverflow = 0;    // キューが溢れた回数

static volatile uint32_t s_isrFrames    = 0;
static volatile uint32_t s_isrCrcErrors = 0;
static volatile bool     s_haveSample   = false;
static ADS131M06::Frame  s_lastFrame;

static void onDrdyFalling() {
  // F-4：割込みコンテキストで完結させる。8 MHz・24バイトで約 24 µs。
  // 以下の処理はすべて整数演算 6CH 分で、976 Hz に対して十分軽い。
  ADS131M06::Frame f;
  const bool ok = adc.readFrame(f);
  s_isrFrames++;
  if (!ok) s_isrCrcErrors++;

  // ★CRC 不一致のサンプルはリングにもパイプラインにも入れない。
  //   欠測として数えるだけにする（M-7/M-8 の母数は adc.stats() 側で持つ）
  if (!ok) { s_lastFrame = f; s_haveSample = true; return; }

  const uint32_t idx = ring.count();   // このサンプルの絶対番号（push 前に取る）
  ring.push(f.ch);

  // ① 静的系列
  int32_t avg[deck::NUM_CH];
  if (blockAvg.push(f.ch, avg) && !s_blockReady) {
    memcpy(s_blockValue, avg, sizeof(s_blockValue));
    s_blockReady = true;
  }

  // ② イベント検出
  deck::EventWindow win;
  if (detector.update(idx, f.ch, win)) {
    const uint8_t next = (uint8_t)((s_evHead + 1) % EVQ_LEN);
    if (next != s_evTail) { s_evQueue[s_evHead] = win; s_evHead = next; }
    else                  { s_evOverflow++; }
  }

  s_lastFrame  = f;
  s_haveSample = true;
}

/**
 * 発火したイベントをリングから取り出す。
 * **まだ SD へは書かない（S6）。**ここでは「ポスト側が溜まったか」「上書きされて
 * いないか」だけを判定し、収録可否を確定させる。
 * 収録できなかったときは黙って捨てず、理由を出す。
 */
static void serviceEvents() {
  while (s_evTail != s_evHead) {
    const deck::EventWindow win = s_evQueue[s_evTail];

    // ポストトリガ分がまだ溜まっていなければ、次のループまで待つ
    if (ring.count() < win.endIdx) return;

    const bool haveStart = ring.resident(win.startIdx);
    const bool limited   = detector.rateLimited();

    if (!haveStart) {
      // リングを追い越された。SD書込みが遅いか、発火が密すぎる
      Serial.printf("[EVENT %lu] 切出し失敗：プリ側がリングから溢れた "
                    "(start=%lu now=%lu)\n",
                    (unsigned long)win.seq, (unsigned long)win.startIdx,
                    (unsigned long)ring.count());
    } else if (limited) {
      // R-2：上限に達したら**波形保存のみ停止し、検出カウントは継続する**
      Serial.printf("[EVENT %lu] レート制限により波形は保存しない "
                    "(収録 %u / 上限 %u)\n",
                    (unsigned long)win.seq, detector.recordedThisHour(),
                    detector.config().maxPerHour);
    } else {
      const uint32_t n = win.endIdx - win.startIdx;
      Serial.printf("[EVENT %lu] 切出し可 CH mask 0x%02X  %lu サンプル "
                    "(%.2f 秒 / %lu バイト)  ※SD書込みは S6 で実装\n",
                    (unsigned long)win.seq, win.firedMask, (unsigned long)n,
                    n / deck::F_DATA_SPS,
                    (unsigned long)(n * deck::SampleRing::SAMPLE_BYTES));
      detector.notifyRecorded();
    }
    s_evTail = (uint8_t)((s_evTail + 1) % EVQ_LEN);
  }
}

/**
 * F-26：ADC の起動順序
 *   ① CS_ADC・CS_SD を出力 High
 *   ② CLKIN の PWM を開始
 *   ③ P3 で SYNC/RESET を Low 2048 tCLKIN（8 MHz で 256 µs）以上
 *   ④ レジスタ設定とリードバック
 */
static bool adcStartup() {
  // ① 両方の CS を High にしてから触る（SPI バス共有・F-28）
  pinMode(PIN_CS_ADC, OUTPUT); digitalWrite(PIN_CS_ADC, HIGH);
  pinMode(PIN_CS_SD,  OUTPUT); digitalWrite(PIN_CS_SD,  HIGH);

  // ② CLKIN を出す。これが無いと POR が完了しない
  clkinStart(PIN_CLKIN);
  delay(1);

  // ③ ハードウェアリセット（TCA9534 P3）。2048 tCLKIN = 256 µs 以上
  tcaSetPort(TCA_P3_ADC_SYNCRST, false);
  delayMicroseconds(500);
  tcaSetPort(TCA_P3_ADC_SYNCRST, true);
  delay(5);                        // POR 完了待ち（DRDY が High へ抜けるまで）

  // ④ SPI とレジスタ設定
  SPI.begin();
  return adc.begin(SPI, PIN_CS_ADC, PIN_DRDY, adcCfg, 8000000UL);
}

// ─────────────────────────────────────────────────────────────
static void printHeader() {
  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F(" Monita Deck v1.00  ブリングアップ"));
  Serial.println(F(" 基板 ver1.00 / ADS131M06 6CH"));
  Serial.printf ("  CLKIN %.3f MHz  OSR %u  fDATA %.4f SPS\n",
                 F_CLKIN_HZ / 1e6, ADC_OSR_VALUE, F_DATA_SPS);
  Serial.printf ("  PGA gain 32  外部基準 REFIN=VEX/2=1.25V  1LSB=%.4f nV\n",
                 ADS131M06::lsbVolts(ADS131M06::GAIN_32) * 1e9);
  Serial.println(F("========================================"));
}

void setup() {
  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 3000) { delay(10); }
  printHeader();

  // ── 1. TCA9534 ──
  Wire.begin();
  Wire.setClock(400000);
  if (tcaBegin()) {
    Serial.println(F("[OK ] TCA9534 初期化（出力0xAA → 方向0xA0）"));
  } else {
    Serial.println(F("[NG ] TCA9534 が応答しない。I2C プルアップ R26/R27（4.7k）と "
                     "アドレス A0-A2=GND を確認すること"));
  }

  // ── 2〜4. ADC 起動 ──
  if (adcStartup()) {
    Serial.println(F("[OK ] ADS131M06 起動・レジスタ照合一致"));
  } else {
    Serial.println(F("[NG ] ADS131M06 の起動に失敗"));
    Serial.println(F("       確認: CLKIN(D2) に 8MHz が出ているか / SYNC-RESET(P3) が High か"));
    Serial.println(F("             CS_ADC(D0) の波形 / SPI モード1 / DVDD(FL1 の先) の電圧"));
    adc.verifyRegisters(adcCfg, &Serial);
  }

  // ── 5. VEX を入れて DRDY 割込みを開始 ──
  tcaSetPort(TCA_P0_EXC_EN, true);     // VEX（2.5V）ON
  delay(10);                            // LP5907 の tON は 80〜150µs。ブリッジ安定を待つ
  Serial.println(F("[   ] VEX 2.5V ON"));

  // ── トリガ設定（初期値。運用中はダウンリンクで書き換わる＝S9）──
  deck::TriggerConfig trig;                    // 既定値はヘッダ参照
  if (!trig.fitsInRing()) {
    // ★プリ＋ポストがリング長を超える設定は成立しない（要件 §7.3.3 Codex指摘）
    Serial.println(F("[NG ] トリガ設定がリング長を超えている。既定値を見直すこと"));
  }
  detector.setConfig(trig);
  Serial.printf("[   ] トリガ: 閾値 %u code / 基線からの変化 / プリ %.1fs + ポスト %.1fs "
                "/ 上限 %u件/時\n",
                trig.threshold, trig.preTenth / 10.0, trig.postTenth / 10.0,
                trig.maxPerHour);

  attachInterrupt(digitalPinToInterrupt(PIN_DRDY), onDrdyFalling, FALLING);
  Serial.println(F("[   ] DRDY 割込み開始。1秒ごとに統計を出す"));
  Serial.println();
}

void loop() {
  static uint32_t nextReport = 0;
  static uint32_t prevFrames = 0;
  static uint32_t nextHourRoll = 3600000UL;

  // ── ①静的系列：1秒平均が出たらメジアン窓へ入れる ──
  if (s_blockReady) {
    int32_t avg[deck::NUM_CH];
    noInterrupts(); memcpy(avg, s_blockValue, sizeof(avg)); s_blockReady = false; interrupts();
    minuteMedian.push(avg);

    // 60個そろったら1分値。これが静的変位であり、イベント判定の基線にもなる
    if (minuteMedian.full()) {
      int32_t med[deck::NUM_CH];
      if (minuteMedian.median(med)) {
        detector.setBaseline(med);              // ThreshMode::FromBase の基準
        const double lsb = ADS131M06::lsbVolts(ADS131M06::GAIN_32);
        Serial.print(F("[1分値] uV:"));
        for (uint8_t c = 0; c < deck::NUM_CH; ++c) Serial.printf(" %8.3f", med[c] * lsb * 1e6);
        Serial.println();
      }
    }
  }

  // ── ②イベント：ポストが溜まったものから処理する ──
  serviceEvents();

  // ── レート制限のカウンタを1時間ごとに戻す（R-1〜R-3）──
  // ★本番では DS3231 の時刻で区切る（S7）。ここでは起動からの経過時間で代用
  if (millis() >= nextHourRoll) {
    Serial.printf("[時間集計] 検出 %u 件 / 収録 %u 件\n",
                  detector.detectedThisHour(), detector.recordedThisHour());
    detector.rollHour();
    nextHourRoll += 3600000UL;
  }

  if (millis() < nextReport) return;
  nextReport = millis() + 1000;

  noInterrupts();
  const uint32_t frames = s_isrFrames;
  const uint32_t crcErr = s_isrCrcErrors;
  const bool     have   = s_haveSample;
  ADS131M06::Frame f    = s_lastFrame;
  interrupts();

  const uint32_t rate = frames - prevFrames;
  prevFrames = frames;

  const ADS131M06::Stats& st = adc.stats();
  Serial.printf("%.1f SPS  総 %lu  CRC異常 %lu  取りこぼし %lu  STATUS異常 %lu  "
                "リング %lu  検出 %u/収録 %u  EVQ溢れ %u\n",
                (double)rate, (unsigned long)frames, (unsigned long)crcErr,
                (unsigned long)st.dropped, (unsigned long)st.statusErr,
                (unsigned long)ring.count(),
                detector.detectedThisHour(), detector.recordedThisHour(),
                s_evOverflow);

  if (have) {
    // 生コードと入力換算電圧を並べる。まだ変位へは換算しない（校正係数は F-17 で個体ごと）
    const double lsb = ADS131M06::lsbVolts(ADS131M06::GAIN_32);
    Serial.print(F("  code:"));
    for (uint8_t ch = 0; ch < ADS131M06::NUM_CH; ++ch) Serial.printf(" %8ld", (long)f.ch[ch]);
    Serial.print(F("\n  uV  :"));
    for (uint8_t ch = 0; ch < ADS131M06::NUM_CH; ++ch) Serial.printf(" %8.2f", f.ch[ch] * lsb * 1e6);
    Serial.printf("\n  STATUS 0x%04X  CRC %s\n", f.status, f.crcOk ? "OK" : "NG");
  }

  // タクトSW（P5）と AC 検知（P7）の生読み。F-29 のとおり P7 はベストエフォート
  uint8_t in = 0;
  if (tcaRead(TCA_REG_INPUT, in)) {
    Serial.printf("  SW %s   AC %s\n",
                  (in & (1u << TCA_P5_TACT_SW)) ? "----" : "押下",
                  (in & (1u << TCA_P7_AC_DETECT)) ? "生存" : "断?");
  }
  Serial.println();
}
