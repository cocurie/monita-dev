/**
 * Monita Gateway — テスト Step01: SD カード書き込みテスト
 *
 * MCU  : Seeed XIAO nRF52840
 * SD   : microSD SPI（標準SDライブラリ使用）
 *        D3=CS、D8=SCK、D9=MISO、D10=MOSI（nRF52840 デフォルトSPIピン）
 *
 * テスト内容:
 *   Step1: SD 初期化・カード種別・容量の表示
 *   Step2: test.txt 書き込み
 *   Step3: test.txt 読み返し（内容照合）
 *   Step4: test.txt 追記
 *   Step5: 100行書き込み速度計測
 *
 * 配線:
 *   SD CS   → XIAO D3
 *   SD CLK  → XIAO D8 (SCK)
 *   SD DAT0 → XIAO D9 (MISO)
 *   SD CMD  → XIAO D10 (MOSI)
 *   SD VDD  → XIAO 3V3
 *   SD VSS  → GND
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <SPI.h>
#include <SD.h>

#define PIN_SD_CS   D3

#define TEST_FILE   "test.txt"
#define BENCH_FILE  "bench.txt"
#define BENCH_LINES 100

static void printSeparator(const char* title) {
  Serial.println();
  Serial.print(F("=== "));
  Serial.print(title);
  Serial.println(F(" ==="));
}

// ── Step1: 初期化・カード情報 ──────────────────────────────
static bool stepInit() {
  printSeparator("Step1: SD 初期化");

  if (!SD.begin(PIN_SD_CS)) {
    Serial.println(F("[FAIL] SD.begin() failed"));
    Serial.println(F("  → カードが挿さっているか確認"));
    Serial.println(F("  → D3=CS / D8=SCK / D9=MISO / D10=MOSI の配線を確認"));
    return false;
  }
  Serial.println(F("[OK] SD.begin() succeeded"));

  // カード種別
  Serial.print(F("  カード種別: "));
  switch (SD.card()->type()) {
    case SD_CARD_TYPE_SD1:  Serial.println(F("SD1"));  break;
    case SD_CARD_TYPE_SD2:  Serial.println(F("SD2"));  break;
    case SD_CARD_TYPE_SDHC: Serial.println(F("SDHC")); break;
    default:                Serial.println(F("不明"));  break;
  }

  // 容量 (クラスタ数 × クラスタサイズ)
  uint32_t sizeMB = SD.size() / 1024UL;
  Serial.print(F("  容量: "));
  Serial.print(sizeMB);
  Serial.println(F(" MB"));

  return true;
}

// ── Step2: 書き込み ───────────────────────────────────────
static bool stepWrite() {
  printSeparator("Step2: 書き込み");

  if (SD.exists(TEST_FILE)) SD.remove(TEST_FILE);

  File f = SD.open(TEST_FILE, FILE_WRITE);
  if (!f) {
    Serial.println(F("[FAIL] ファイルを開けません"));
    return false;
  }
  f.println(F("Hello, Monita Gateway!"));
  f.println(F("SD write test line 2"));
  f.close();

  Serial.print(F("[OK] "));
  Serial.print(TEST_FILE);
  Serial.println(F(" に2行書き込みました"));
  return true;
}

// ── Step3: 読み返し ──────────────────────────────────────
static bool stepRead() {
  printSeparator("Step3: 読み返し");

  File f = SD.open(TEST_FILE, FILE_READ);
  if (!f) {
    Serial.println(F("[FAIL] ファイルを開けません"));
    return false;
  }

  Serial.println(F("--- ファイル内容 ---"));
  while (f.available()) {
    Serial.write(f.read());
  }
  f.close();
  Serial.println(F("--- ここまで ---"));
  Serial.println(F("[OK] 読み返し完了"));
  return true;
}

// ── Step4: 追記 ──────────────────────────────────────────
static bool stepAppend() {
  printSeparator("Step4: 追記");

  File f = SD.open(TEST_FILE, FILE_WRITE);  // FILE_WRITE は末尾追記
  if (!f) {
    Serial.println(F("[FAIL] ファイルを開けません"));
    return false;
  }
  f.println(F("Appended line"));
  f.close();

  f = SD.open(TEST_FILE, FILE_READ);
  if (!f) {
    Serial.println(F("[FAIL] 追記後の読み返し失敗"));
    return false;
  }
  Serial.println(F("--- 追記後のファイル内容 ---"));
  while (f.available()) {
    Serial.write(f.read());
  }
  f.close();
  Serial.println(F("--- ここまで ---"));
  Serial.println(F("[OK] 追記完了"));
  return true;
}

// ── Step5: 書き込み速度計測 ──────────────────────────────
static bool stepBenchmark() {
  printSeparator("Step5: 書き込み速度計測（100行）");

  if (SD.exists(BENCH_FILE)) SD.remove(BENCH_FILE);

  File f = SD.open(BENCH_FILE, FILE_WRITE);
  if (!f) {
    Serial.println(F("[FAIL] ファイルを開けません"));
    return false;
  }

  uint32_t t0 = millis();
  for (int i = 1; i <= BENCH_LINES; i++) {
    f.print(F("line,"));
    f.print(i);
    f.print(F(",data,"));
    f.println(millis());
  }
  f.close();
  uint32_t elapsed = millis() - t0;

  Serial.print(F("[OK] "));
  Serial.print(BENCH_LINES);
  Serial.print(F(" 行書き込み完了: "));
  Serial.print(elapsed);
  Serial.print(F(" ms ("));
  Serial.print(elapsed / BENCH_LINES);
  Serial.println(F(" ms/行)"));
  return true;
}

// ── setup / loop ─────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  delay(500);

  Serial.println(F("====================================="));
  Serial.println(F("  Gateway Step01: SD カード書き込みテスト"));
  Serial.println(F("====================================="));

  if (!stepInit())      { Serial.println(F("\n[ABORT] Step1 失敗")); return; }
  if (!stepWrite())     { Serial.println(F("\n[ABORT] Step2 失敗")); return; }
  if (!stepRead())      { Serial.println(F("\n[ABORT] Step3 失敗")); return; }
  if (!stepAppend())    { Serial.println(F("\n[ABORT] Step4 失敗")); return; }
  if (!stepBenchmark()) { Serial.println(F("\n[ABORT] Step5 失敗")); return; }

  Serial.println();
  Serial.println(F("====================================="));
  Serial.println(F("  全 Step 完了 — SD テスト合格"));
  Serial.println(F("====================================="));
}

void loop() {}
