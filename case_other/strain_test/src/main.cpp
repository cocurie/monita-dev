// ============================================================
// StrainInspector — ひずみFlexモジュール 出荷検査治具 用テストファーム
//
// Monita Flex ver3.20 基板をベースに、4CHすべてをHX711ひずみ測定として使う。
// 通信・スリープ・WDT・RGB LED・DS3231・電池電圧は一切扱わない。
// シリアルモニタからのコマンド入力のみで動作する対話型ベンチツール。
//
// 【コマンド】
//   0 : 4CH測定（単発。結果をシリアルに表示）
//   8 : 4CHゼロ点補正（タレ）
//   9 : 状態確認（タレオフセット・直近測定値・応答状況をキャッシュから表示。
//       ハードウェアへのアクセスは行わない＝即座に返る）
//
// 【HX711が応答しない場合】
//   該当CHにエラーを表示し、そのCHの測定/タレをスキップする。
//   他のCHの処理は継続する（1CHの故障・未接続で全体を止めない）。
//
// 出典・流用元: case01_Flex/v3.20/src/main.cpp
//   （muxSelect/hxBegin/hxReadAvg/hxRead/hxTareWithTimeout はほぼそのまま踏襲）
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include "HX711.h"

// ============================================================
// ピン定義（v3.20 と同一。Flex基板のネットリストで確認済み）
// ============================================================
#define HX711_SCK_PIN   6   // D6: JPコネクタ pin3（PD_SCK_CHx）
#define HX711_DOUT_PIN  7   // D7: JPコネクタ pin4（DOUT_CHx）
#define SW_POWER_PIN    10  // D10: 3V3_SW 電源ゲート（HIGH = ON）
#define TCA9534_ADDR    0x20

static const uint8_t  FW_VERSION   = 1;         // コミットのたびに+1すること
static const char     FW_NAME[]    = "StrainInspector";
static const char     BUILD_INFO[] = __DATE__ " " __TIME__;

// ── ひずみ換算係数 ──────────────────────────────────────────
// 送信値（µε）= HX711生値 / STRAIN_SCALE
// v3.20 のデフォルト実測値をそのまま暫定使用。個体差があるため、
// 治具のシャント抵抗による既知ひずみと比較して必要なら書き換えること。
static const float STRAIN_SCALE = 1110.0f;

// HX711 1回の平均を求めるための生サンプル数（v3.20 と同じ2段方式）
#define SAMPLES_PER_AVG 5
// 平均値を何回取得してメジアンを求めるか
#define MEASURE_COUNT   5
// HX711の1サンプル待ちタイムアウト（ready にならない=未接続/断線とみなす）
#define HX711_SAMPLE_TIMEOUT_MS 1000UL

static HX711 hx;

// ── CHごとの状態（RAM保持のみ。電源を切ると失われる）────────────
static long  s_tareOffset[4]      = {0, 0, 0, 0};
static bool  s_lastValid[4]       = {false, false, false, false};  // 直近の測定/タレが成功したか
static float s_lastStrainUe[4]    = {0, 0, 0, 0};                  // 直近の測定値（µε）
static bool  s_everMeasured[4]    = {false, false, false, false};  // 一度でも測定できたか（"9"の表示用）

// ============================================================
// TCA9534 — SN74LV4052（CH切替マルチプレクサ）の A/B を I2C から駆動
// v3.20 と同じレジスタマップ（Input0 / Output1 / Polarity2 / Config3）
// ============================================================
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

// P0,P1 を出力（MUX A/B）、他は入力。A=0,B=0 で既知状態に初期化する。
static bool tca9534Configure() {
  if (!tca9534WriteReg(0x02, 0x00)) return false;         // Polarity: 反転なし
  if (!tca9534WriteReg(0x03, 0xFC)) return false;         // Config: P0,P1のみ出力
  uint8_t out = 0;
  if (!tca9534ReadReg(0x01, &out)) return false;
  out = (uint8_t)((out & (uint8_t)~0x03U) | 0x00U);
  return tca9534WriteReg(0x01, out);
}

static bool s_tcaOk = false;  // setup()で確定。falseなら全CHが応答しない前提

static bool muxSelect(uint8_t ch) {
  uint8_t idx = (uint8_t)((ch - 1) & 0x03);
  const uint8_t a = idx & 0x01;
  const uint8_t b = (idx >> 1) & 0x01;
  const uint8_t twoBits = (uint8_t)(b | (a << 1));  // P0=B, P1=A

  uint8_t out = 0;
  if (!tca9534ReadReg(0x01, &out)) return false;
  out = (uint8_t)((out & (uint8_t)~0x03U) | (twoBits & 0x03U));
  return tca9534WriteReg(0x01, out);
}

// ============================================================
// HX711 — CH切替・読み取り・タレ（v3.20の実装をほぼそのまま踏襲）
// ============================================================

// 指定チャネル（1〜4）にMUXを合わせ、HX711をbeginしてタレオフセットを復元する
static bool hxBegin(uint8_t ch) {
  if (!muxSelect(ch)) return false;
  delay(10);  // MUX切替の安定待ち
  pinMode(HX711_SCK_PIN, OUTPUT);
  digitalWrite(HX711_SCK_PIN, LOW);
  delay(10);
  hx.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
  hx.set_offset(s_tareOffset[ch - 1]);
  return true;
}

// HX711からSAMPLES_PER_AVG個のサンプル平均を1回取得する（タレ補正済み値）
static bool hxReadAvg(float *outAvg) {
  float sum = 0;
  for (int i = 0; i < SAMPLES_PER_AVG; i++) {
    if (!hx.wait_ready_timeout(HX711_SAMPLE_TIMEOUT_MS)) {
      *outAvg = 0;
      return false;
    }
    sum += hx.get_value();  // read() - tare_offset
  }
  *outAvg = sum / SAMPLES_PER_AVG;
  return true;
}

// 小さな配列のバブルソートで中央値を求める（外れ値に強い簡易ロバスト化）
static float median(float *a, int n) {
  float t[MEASURE_COUNT];
  memcpy(t, a, sizeof(float) * (size_t)n);
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - i - 1; j++)
      if (t[j] > t[j + 1]) { float x = t[j]; t[j] = t[j + 1]; t[j + 1] = x; }
  return t[n / 2];
}

// MEASURE_COUNT回の平均値を取得し、そのメジアンを返す（v3.20と同じ2段方式）
static bool hxRead(float *outRaw) {
  float avgs[MEASURE_COUNT];
  for (int i = 0; i < MEASURE_COUNT; i++) {
    if (!hxReadAvg(&avgs[i])) {
      *outRaw = 0;
      return false;
    }
  }
  *outRaw = median(avgs, MEASURE_COUNT);
  return true;
}

// タレ1回分のオフセットを、サンプルごとにタイムアウトを見ながら求める。
// 戻り値: true=成功（オフセット設定済み）、false=タイムアウト（オフセットは変更しない）
static bool hxTareWithTimeout(uint8_t times = 10) {
  double sum = 0;
  for (uint8_t i = 0; i < times; i++) {
    if (!hx.wait_ready_timeout(HX711_SAMPLE_TIMEOUT_MS)) return false;
    sum += (double)hx.read();
  }
  hx.set_offset((long)(sum / (double)times));
  return true;
}

// ============================================================
// コマンド実装
// ============================================================

static void printUsage() {
  Serial.println();
  Serial.print("=== ");
  Serial.print(FW_NAME);
  Serial.print(" v");
  Serial.print(FW_VERSION);
  Serial.print(" (build ");
  Serial.print(BUILD_INFO);
  Serial.println(") ===");
  Serial.print("STRAIN_SCALE = ");
  Serial.println(STRAIN_SCALE, 2);
  Serial.print("TCA9534 (CH切替MUX): ");
  Serial.println(s_tcaOk ? "OK" : "エラー（全CH応答しない可能性）");
  Serial.println("--- コマンド一覧 ---");
  Serial.println("  0 : 4CH測定（単発）");
  Serial.println("  8 : 4CHゼロ点補正（タレ）");
  Serial.println("  9 : 状態確認（再測定なし）");
  Serial.println("--------------------");
  Serial.println();
}

// '0': 4CH測定。CHごとにHX711応答を確認し、無応答ならエラー表示してスキップする。
static void cmdMeasure() {
  Serial.println("[MEASURE] 開始");
  for (uint8_t ch = 1; ch <= 4; ch++) {
    if (!hxBegin(ch)) {
      Serial.print("[MEASURE] CH"); Serial.print(ch);
      Serial.println(": ERROR（MUX切替失敗。TCA9534未応答）");
      s_lastValid[ch - 1] = false;
      continue;
    }
    float raw;
    if (!hxRead(&raw)) {
      Serial.print("[MEASURE] CH"); Serial.print(ch);
      Serial.println(": ERROR（HX711応答なし。未接続または断線の可能性）");
      s_lastValid[ch - 1] = false;
      continue;
    }
    float ue = raw / STRAIN_SCALE;
    s_lastStrainUe[ch - 1] = ue;
    s_lastValid[ch - 1]    = true;
    s_everMeasured[ch - 1] = true;
    Serial.print("[MEASURE] CH"); Serial.print(ch);
    Serial.print(": ");
    Serial.print(ue, 1);
    Serial.println(" µε");
  }
  Serial.println("[MEASURE] 完了");
}

// '8': 4CHゼロ点補正。CHごとにHX711応答を確認し、無応答ならエラー表示してスキップする
//      （その場合オフセットは変更せず、以前の値を保持する）。
static void cmdTare() {
  Serial.println("[TARE] 開始");
  for (uint8_t ch = 1; ch <= 4; ch++) {
    if (!hxBegin(ch)) {
      Serial.print("[TARE] CH"); Serial.print(ch);
      Serial.println(": ERROR（MUX切替失敗。TCA9534未応答）");
      s_lastValid[ch - 1] = false;
      continue;
    }
    if (!hxTareWithTimeout()) {
      Serial.print("[TARE] CH"); Serial.print(ch);
      Serial.println(": ERROR（HX711応答なし。オフセットは変更していません）");
      s_lastValid[ch - 1] = false;
      continue;
    }
    s_tareOffset[ch - 1] = hx.get_offset();
    s_lastValid[ch - 1]  = true;
    Serial.print("[TARE] CH"); Serial.print(ch);
    Serial.print(": OK（offset=");
    Serial.print(s_tareOffset[ch - 1]);
    Serial.println(")");
  }
  Serial.println("[TARE] 完了");
}

// '9': 状態確認。ハードウェアへは一切アクセスせず、キャッシュ済みの値だけを表示する
//      （'0'/'8' の直近の実行結果を反映。まだ一度も実行していないCHはその旨を表示）。
static void cmdStatus() {
  Serial.println("[STATUS]");
  Serial.print("  FW: "); Serial.print(FW_NAME);
  Serial.print(" v"); Serial.print(FW_VERSION);
  Serial.print(" (build "); Serial.print(BUILD_INFO); Serial.println(")");
  Serial.print("  STRAIN_SCALE: "); Serial.println(STRAIN_SCALE, 2);
  Serial.print("  TCA9534: "); Serial.println(s_tcaOk ? "OK" : "エラー");
  for (uint8_t ch = 1; ch <= 4; ch++) {
    Serial.print("  CH"); Serial.print(ch); Serial.print(": ");
    Serial.print("tare_offset="); Serial.print(s_tareOffset[ch - 1]);
    Serial.print(", 直近値=");
    if (s_everMeasured[ch - 1]) {
      Serial.print(s_lastStrainUe[ch - 1], 1);
      Serial.print(" µε");
    } else {
      Serial.print("未測定");
    }
    Serial.print(", 直近応答=");
    Serial.println(s_lastValid[ch - 1] ? "OK" : "NG");
  }
}

// ============================================================
// setup / loop
// ============================================================

void setup() {
  Serial.begin(115200);
  // XIAO nRF52840はUSB CDCの列挙に時間がかかることがある。
  // ホストが未接続でも起動自体は継続できるよう、待ちは上限付きにする。
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0) < 5000UL) delay(10);

  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);  // 3V3_SW ON
  delay(200);                        // モジュール側の電源安定待ち

  Wire.begin();
  s_tcaOk = tca9534Configure();

  printUsage();
}

void loop() {
  if (!Serial.available()) return;

  int c = Serial.read();
  switch (c) {
    case '0': cmdMeasure(); break;
    case '8': cmdTare();    break;
    case '9': cmdStatus();  break;
    case '\r': case '\n':   break;  // 改行は無視
    default:
      Serial.print("[?] 不明なコマンド: '");
      Serial.print((char)c);
      Serial.println("'");
      printUsage();
      break;
  }
}
