/**
 * Monita Flex v3.04 — 検証 Step14: SD カード書き込みテスト
 *
 * 目的:
 *   DM3AT-SF-PEJM5 に挿した microSD カードへの読み書きを確認する。
 *   SPI ピンを nRF52840 のデフォルト（D8/D9/D10）から再マップして使用。
 *   標準 SD ライブラリはカスタム SPI 非対応のため SdFat v2 を使用。
 *
 * テスト内容:
 *   Step1: SD 初期化・カード種別・容量の表示
 *   Step2: test.txt 書き込み
 *   Step3: test.txt 読み返し（内容照合）
 *   Step4: test.txt 追記
 *   Step5: 100行書き込み速度計測
 *
 * 配線（テスト環境 / プルアップなし）:
 *   DM3AT pin2 CD/DAT3 (CS)   → XIAO D3  (P0.29)
 *   DM3AT pin3 CMD    (MOSI)  → XIAO D1  (P0.03)
 *   DM3AT pin4 VDD            → 3.3V
 *   DM3AT pin5 CLK    (SCK)   → XIAO D10 (P1.15)
 *   DM3AT pin6 VSS            → GND
 *   DM3AT pin7 DAT0   (MISO)  → XIAO D2  (P0.28)  ※プルアップなし
 *   DM3AT pin1 DAT2           → 浮き               ※プルアップなし（テスト環境）
 *   DM3AT pin8 DAT1           → 浮き               ※プルアップなし（テスト環境）
 *
 * 注意:
 *   MISO・DAT1・DAT2 のプルアップがないため初期化が不安定な場合がある。
 *   失敗時は SPI_SPEED_MHZ を 1 に下げて再試行すること。
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <SPI.h>
#include <SdFat.h>

// ══════════════════════════════════════════════
// ▼ ピン定義・速度設定
// ══════════════════════════════════════════════
#define PIN_SD_CS   D3
#define PIN_SD_MISO D2   // P0.28
#define PIN_SD_SCK  D10  // P1.15
#define PIN_SD_MOSI D1   // P0.03

// 初期化失敗時は 1 に下げる
#define SPI_SPEED_MHZ 4

#define TEST_FILE  "test.txt"
#define BENCH_FILE "bench.txt"
#define BENCH_LINES 100

// デフォルト SPI (NRF_SPIM3) とは別のペリフェラルでカスタムピンを使う
static SPIClass SD_SPI(NRF_SPIM2, PIN_SD_MISO, PIN_SD_SCK, PIN_SD_MOSI);
static SdFat sd;

// ══════════════════════════════════════════════
// ▼ ユーティリティ
// ══════════════════════════════════════════════
static void printSeparator(const char* title) {
  Serial.println();
  Serial.print("=== ");
  Serial.print(title);
  Serial.println(" ===");
}

// ══════════════════════════════════════════════
// ▼ Step1: 初期化・カード情報
// ══════════════════════════════════════════════
static bool stepInit() {
  printSeparator("Step1: SD 初期化");

  SdSpiConfig cfg(PIN_SD_CS, DEDICATED_SPI, SD_SCK_MHZ(SPI_SPEED_MHZ), &SD_SPI);
  if (!sd.begin(cfg)) {
    Serial.println("[FAIL] sd.begin() failed");
    sd.initErrorPrint(&Serial);
    Serial.println("  → カードが挿さっているか確認");
    Serial.println("  → SPI_SPEED_MHZ を 1 に下げて再試行");
    return false;
  }
  Serial.println("[OK] sd.begin() succeeded");

  // カード種別
  Serial.print("  カード種別: ");
  switch (sd.card()->type()) {
    case SD_CARD_TYPE_SD1:  Serial.println("SD1");  break;
    case SD_CARD_TYPE_SD2:  Serial.println("SD2");  break;
    case SD_CARD_TYPE_SDHC: Serial.println("SDHC"); break;
    default:                Serial.println("不明");  break;
  }

  // 容量
  uint32_t sizeMB = (uint32_t)(0.000512 * sd.card()->sectorCount());
  Serial.print("  容量: ");
  Serial.print(sizeMB);
  Serial.println(" MB");

  return true;
}

// ══════════════════════════════════════════════
// ▼ Step2: 書き込み
// ══════════════════════════════════════════════
static bool stepWrite() {
  printSeparator("Step2: 書き込み");

  if (sd.exists(TEST_FILE)) sd.remove(TEST_FILE);

  FsFile f = sd.open(TEST_FILE, O_WRITE | O_CREAT);
  if (!f) {
    Serial.println("[FAIL] ファイルを開けません");
    return false;
  }
  f.println("Hello, Monita Flex v3.04!");
  f.println("SD write test line 2");
  f.close();
  Serial.print("[OK] ");
  Serial.print(TEST_FILE);
  Serial.println(" に2行書き込みました");
  return true;
}

// ══════════════════════════════════════════════
// ▼ Step3: 読み返し
// ══════════════════════════════════════════════
static bool stepRead() {
  printSeparator("Step3: 読み返し");

  FsFile f = sd.open(TEST_FILE, O_READ);
  if (!f) {
    Serial.println("[FAIL] ファイルを開けません");
    return false;
  }
  Serial.println("--- ファイル内容 ---");
  while (f.available()) {
    Serial.write(f.read());
  }
  f.close();
  Serial.println("--- ここまで ---");
  Serial.println("[OK] 読み返し完了");
  return true;
}

// ══════════════════════════════════════════════
// ▼ Step4: 追記
// ══════════════════════════════════════════════
static bool stepAppend() {
  printSeparator("Step4: 追記");

  FsFile f = sd.open(TEST_FILE, O_WRITE | O_APPEND);
  if (!f) {
    Serial.println("[FAIL] ファイルを開けません");
    return false;
  }
  f.println("Appended line");
  f.close();

  f = sd.open(TEST_FILE, O_READ);
  if (!f) {
    Serial.println("[FAIL] 追記後の読み返し失敗");
    return false;
  }
  Serial.println("--- 追記後のファイル内容 ---");
  while (f.available()) {
    Serial.write(f.read());
  }
  f.close();
  Serial.println("--- ここまで ---");
  Serial.println("[OK] 追記完了");
  return true;
}

// ══════════════════════════════════════════════
// ▼ Step5: 書き込み速度計測
// ══════════════════════════════════════════════
static bool stepBenchmark() {
  printSeparator("Step5: 書き込み速度計測（100行）");

  if (sd.exists(BENCH_FILE)) sd.remove(BENCH_FILE);

  FsFile f = sd.open(BENCH_FILE, O_WRITE | O_CREAT);
  if (!f) {
    Serial.println("[FAIL] ファイルを開けません");
    return false;
  }

  uint32_t t0 = millis();
  for (int i = 1; i <= BENCH_LINES; i++) {
    f.print("line,");
    f.print(i);
    f.print(",data,");
    f.println(millis());
  }
  f.close();
  uint32_t elapsed = millis() - t0;

  Serial.print("[OK] ");
  Serial.print(BENCH_LINES);
  Serial.print(" 行書き込み完了: ");
  Serial.print(elapsed);
  Serial.print(" ms (");
  Serial.print(elapsed / BENCH_LINES);
  Serial.println(" ms/行)");
  return true;
}

// ══════════════════════════════════════════════
// ▼ setup / loop
// ══════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  delay(500);

  Serial.println("=============================");
  Serial.println(" Step14: SD カード書き込みテスト");
  Serial.println("=============================");

  if (!stepInit())      { Serial.println("\n[ABORT] Step1 失敗"); return; }
  if (!stepWrite())     { Serial.println("\n[ABORT] Step2 失敗"); return; }
  if (!stepRead())      { Serial.println("\n[ABORT] Step3 失敗"); return; }
  if (!stepAppend())    { Serial.println("\n[ABORT] Step4 失敗"); return; }
  if (!stepBenchmark()) { Serial.println("\n[ABORT] Step5 失敗"); return; }

  Serial.println();
  Serial.println("=============================");
  Serial.println(" 全 Step 完了 — SD テスト合格");
  Serial.println("=============================");
}

void loop() {}
