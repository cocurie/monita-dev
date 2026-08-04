/**
 * 豊能町 埋設水道管 共振探査 — 掃引正弦波(チャープ)出力テスト / XIAO nRF52840 Sense
 *
 * 【目的】
 *   埋設水道管の共振周波数を探るため、ホワイトノイズの代わりに掃引正弦波(チャープ)を
 *   加振源として使えないか検証する。本ファームは「まず狙った周波数でスイープ波形が
 *   出せるか」を確認する第一段階。加速度センサ側のロックイン検波は未実装（次段階）。
 *
 * 【生成方式: PWM-DDS】
 *   ・位相アキュムレータ(32bit) + サインテーブル(256点, 8bit)によるDDS方式
 *   ・HwPWM0でD0ピンにPWM出力（8bit分解能, clock/1=16MHz → キャリア周波数 16MHz/256 ≈ 62.5kHz）
 *   ・TIMER4のハードウェア割込みでSAMPLE_RATE(20kHz)ごとに位相を進め、PWMデューティを更新
 *   ・D0出力を外付けLPF(RC等、カットオフ目安5kHz程度)でキャリア除去 → パワーアンプ → スピーカーへ
 *
 * 【スイープ仕様】
 *   ・対数(exponential)スイープ: F_START → F_END を SWEEP_TIME 秒かけて掃引し、繰り返す
 *   ・掃引速度Rや瞬時線幅Δf≈√Rの考え方は開発メモ側の議論を参照
 *
 * 【動作確認方法】
 *   ・USBシリアル(115200bps)に、現在の掃引周波数を約100msごとに出力
 *   ・掃引が1周するたびにLED_REDを短く点滅（サイクルの区切りを目視確認するため）
 *   ・D0の波形はオシロ、または前段のPWM出力をUSBオーディオI/F+Audacity/REW等でスペクトル確認
 *
 * 【対象ハード】
 *   XIAO nRF52840 Sense。PWM出力: D0。LPF・パワーアンプ・スピーカーは本ファームの対象外
 *   （回路検討はこの後の段階で確定する）。
 *
 * 【未実装（次段階）】
 *   ・加速度センサ入力の取得
 *   ・参照sin/cosとのミキシング + LPFによるロックイン検波(I/Q)
 *   ・共振カーブ(振幅 vs 周波数)のBLE送信
 */

#include <Arduino.h>
#include <nrf.h>
#include <Adafruit_TinyUSB.h>
#include "HardwarePWM.h"

// ============================================================
// アプリ設定（ここを主に編集する）
// ============================================================

#define PWM_PIN         D0     // TX出力 → 外付けLPF → パワーアンプ
#define SAMPLE_RATE     20000  // PWMデューティ更新レート[Hz]（TIMER4割込みレート）

#define F_START         20.0f    // 掃引開始周波数[Hz]
#define F_END           2000.0f  // 掃引終了周波数[Hz]
#define SWEEP_TIME_S    10.0f    // 1掃引にかける時間[秒]（対数スイープ）

#define SINE_TABLE_SIZE 256    // サインテーブル点数（8bit PWM分解能に合わせる）

// ============================================================
// サインテーブル・DDS状態
// ============================================================

static uint8_t sine_table[SINE_TABLE_SIZE];

// phaseAcc/phaseIncrementはISRとloop()の両方から触るのでvolatile。
// 32bit単位の読み書きはCortex-M上でアトミックなので排他制御は省略している。
static volatile uint32_t phaseAcc = 0;
static volatile uint32_t phaseIncrement = 0;

// 周波数[Hz] → チューニングワード変換 (Δf = freq * 2^32 / SAMPLE_RATE)
static uint32_t freqToTuningWord(float freq_hz) {
  return (uint32_t)((freq_hz * 4294967296.0) / SAMPLE_RATE);
}

// ============================================================
// TIMER4: SAMPLE_RATEごとにPWMデューティを更新するハードウェア割込み
// ============================================================

static void setupSweepTimer() {
  NRF_TIMER4->TASKS_STOP = 1;
  NRF_TIMER4->MODE       = TIMER_MODE_MODE_Timer;
  NRF_TIMER4->BITMODE    = TIMER_BITMODE_BITMODE_32Bit;
  NRF_TIMER4->PRESCALER  = 4; // 16MHz / 2^4 = 1MHz タイマークロック
  NRF_TIMER4->CC[0]      = 1000000UL / SAMPLE_RATE; // 1MHzクロックでのカウント数
  NRF_TIMER4->SHORTS     = TIMER_SHORTS_COMPARE0_CLEAR_Msk; // CC[0]一致で自動クリア(=周期割込み)
  NRF_TIMER4->INTENSET   = TIMER_INTENSET_COMPARE0_Msk;

  NVIC_SetPriority(TIMER4_IRQn, 3);
  NVIC_ClearPendingIRQ(TIMER4_IRQn);
  NVIC_EnableIRQ(TIMER4_IRQn);

  NRF_TIMER4->TASKS_CLEAR = 1;
  NRF_TIMER4->TASKS_START = 1;
}

extern "C" void TIMER4_IRQHandler(void) {
  if (NRF_TIMER4->EVENTS_COMPARE[0]) {
    NRF_TIMER4->EVENTS_COMPARE[0] = 0;

    phaseAcc += phaseIncrement;
    uint8_t idx = (uint8_t)(phaseAcc >> 24); // 上位8bitをテーブルindexに使う
    HwPWM0.writePin(PWM_PIN, sine_table[idx]);
  }
}

// ============================================================
// setup / loop
// ============================================================

void setup() {
  Serial.begin(115200);
  uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart < 3000)) { /* USB Serial接続待ち(最大3秒) */ }

  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, !LED_STATE_ON);

  // サインテーブル生成 (0-255, 中央128がゼロクロス)
  for (int i = 0; i < SINE_TABLE_SIZE; i++) {
    sine_table[i] = (uint8_t)(127.5 + 127.5 * sin(2.0 * PI * i / SINE_TABLE_SIZE));
  }

  // PWM設定: 8bit分解能, clock/1(16MHz) → キャリア 16MHz/256 ≈ 62.5kHz
  HwPWM0.addPin(PWM_PIN);
  HwPWM0.begin();
  HwPWM0.setResolution(8);
  HwPWM0.setClockDiv(PWM_PRESCALER_PRESCALER_DIV_1);

  phaseIncrement = freqToTuningWord(F_START);

  Serial.println("=== pipe_resonance_sweep: PWM-DDS sweep test ===");
  Serial.print("F_START="); Serial.print(F_START);
  Serial.print("Hz, F_END="); Serial.print(F_END);
  Serial.print("Hz, SWEEP_TIME="); Serial.print(SWEEP_TIME_S);
  Serial.println("s (log sweep)");

  setupSweepTimer();
}

void loop() {
  static uint32_t sweepStartMs = millis();
  static uint32_t lastReportMs = 0;

  uint32_t nowMs = millis();
  float t = (nowMs - sweepStartMs) / 1000.0f;

  if (t >= SWEEP_TIME_S) {
    // 1掃引完了 → 先頭に戻り、目視確認用にLEDを短く点滅
    sweepStartMs = nowMs;
    t = 0;
    digitalWrite(LED_RED, LED_STATE_ON);
    delay(20);
    digitalWrite(LED_RED, !LED_STATE_ON);
  }

  // 対数(exponential)スイープ: f(t) = F_START * (F_END/F_START)^(t/SWEEP_TIME_S)
  float f_now = F_START * powf(F_END / F_START, t / SWEEP_TIME_S);
  phaseIncrement = freqToTuningWord(f_now);

  if (nowMs - lastReportMs >= 100) {
    lastReportMs = nowMs;
    Serial.print("t="); Serial.print(t, 2);
    Serial.print("s  f="); Serial.print(f_now, 1);
    Serial.println("Hz");
  }
}
