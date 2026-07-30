/**
 * 検証 Step24: スリープ電流ベースライン測定（最小構成）
 *
 * 【背景】
 *   v3.10 本番ファームで deepSleep() を FreeRTOS の ulTaskNotifyTake() 化し、
 *   さらに LED の PWM ペリフェラル停止漏れを修正した結果、スリープ電流は
 *   約1mA → 74µA まで改善した。しかし依然として 2.84ms 周期・約2.5〜3mA の
 *   鋭いスパイクが残っており、原因が特定できていない。
 *
 * 【このスケッチの目的】
 *   「Adafruit nRF52 コア／FreeRTOS を使う限り避けられないベースライン」なのか、
 *   「本番ファームのどれかの初期化処理が原因」なのかを切り分ける。
 *
 * 【使い方】
 *   1. TEST_STAGE = 0 でビルド・書き込み、PPK2 でスリープ電流を測定する
 *      → ここで既に 2.84ms スパイクが出るなら、コア/FreeRTOS のベースライン。
 *        本番ファーム側では対処できない（対策はコア設定の変更が必要）。
 *      → 出ないなら本番ファーム側に原因がある。TEST_STAGE を上げて二分探索する。
 *   2. TEST_STAGE を 1, 2, 3... と上げて、スパイクが出始めた段階が原因。
 *
 * 【測定方法】
 *   USB は必ず外し、PPK2 から給電した状態で測定すること
 *   （USB 接続時は数mA 上乗せされ、比較にならない）。
 *   セットアップ直後に BOOT_WAIT_MS だけ待ってからスリープに入るので、
 *   その後の平坦な区間を測定する。
 */

#include <Arduino.h>
#include <nrf.h>

// ============================================================
// テスト段階（0 から順に上げて二分探索する）
//   0: 何もしない（コア + FreeRTOS のみ）             ← まずここを測る
//   1: + Serial（USB CDC）を begin する
//   2: + Wire（I2C, TWIM）を begin する
//   3: + Serial1（UART, UARTE）を begin して end する
//   4: + analogRead（SAADC）を 1 回実行する
//   5: + InternalFS（LittleFS）を begin する
//   6: + WDT を起動する
//   7: + HX711 ピンを本番ファームと同じ状態にする
//        （D6=OUTPUT/LOW, D7=INPUT。3V3_SW が OFF の 4052 MUX へ繋がっている）
//   8: + D0 を INPUT_PULLUP にし attachInterrupt(FALLING) を設定（GPIOTE を使う）
//   9: + D1（SPARE_GPIO_PIN）を INPUT_PULLUP にする
// ============================================================
#define TEST_STAGE 8

// スリープに入る前の待機時間（PPK2 で「起動区間」と「スリープ区間」を
// 見分けやすくするためのマーカー代わり）
#define BOOT_WAIT_MS 3000

// ── TWIM（I2C）完全停止ワークアラウンドの適用 ──────────────────
// 1 にすると、スリープ前に Wire.end() ＋ TWIM0 の強制パワーサイクルを行う。
//
// 【背景】測定の結果、TEST_STAGE=2（Wire.begin() を追加）で
//   スリープ電流が 18µA → 305µA（約17倍）に悪化することが判明した。
//   Adafruit nRF52 コアの Wire.end() は `ENABLE = Disabled` を書くだけで、
//   nRF52 の既知の問題により TWIM ペリフェラルが完全には停止しない。
//   Nordic が案内しているワークアラウンド（ペリフェラルのベースアドレス
//   +0xFFC への書き込みによる強制パワーサイクル）で完全に電源を落とす。
//
// TEST_STAGE >= 2 のときに効果を確認すること。
#define APPLY_TWIM_POWER_FIX 1

#if TEST_STAGE >= 1
#include <Adafruit_TinyUSB.h>
#endif
#if TEST_STAGE >= 2
#include <Wire.h>
#endif
#if TEST_STAGE >= 5
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
#endif

// 本番ファーム（v3.10）と同じピン定義
static const uint8_t SW_POWER_PIN   = 10;  // D10: 3V3_SW 制御
static const uint8_t SIGFOX_TX_PIN  = 8;
static const uint8_t SIGFOX_RX_PIN  = 9;
static const uint8_t BATT_ANALOG_PIN = A3;
static const uint8_t HX711_SCK_PIN  = 6;   // D6: 4052 MUX 経由で各 CH の PD_SCK へ
static const uint8_t HX711_DOUT_PIN = 7;   // D7: 4052 MUX 経由で各 CH の DOUT へ
static const uint8_t USER_BUTTON_PIN = 0;  // D0: タクトスイッチ（GNDショート、内部プルアップ）
static const uint8_t SPARE_GPIO_PIN  = 1;  // D1: 予備 GPIO

#if TEST_STAGE >= 8
// 本番ファームと同じくボタン割り込みを設定する（中身は何もしない）
static volatile bool s_btnFlag = false;
static void btnISR() { s_btnFlag = true; }
#endif

// nRF52 ペリフェラルの強制パワーサイクル（Nordic 案内のワークアラウンド）。
// ENABLE=0 だけでは内部が完全停止せず電流を消費し続けるため、
// ベースアドレス +0xFFC に 0 → 読み戻し → 1 を書いてリセットする。
// 実行後、そのペリフェラルは再初期化（Wire.begin() 等）が必要になる。
static inline void nrfPeripheralPowerCycle(uint32_t baseAddr) {
  *(volatile uint32_t *)(baseAddr + 0xFFC) = 0;
  (void)*(volatile uint32_t *)(baseAddr + 0xFFC);
  *(volatile uint32_t *)(baseAddr + 0xFFC) = 1;
}

// Wire（TwoWire）が使うのは NRF_TWIM0（ベースアドレス 0x40003000）。
// framework-arduinoadafruitnrf52/libraries/Wire/Wire_nRF52.cpp:406 で確認済み。
#define TWIM0_BASE_ADDR 0x40003000UL

// 指定した Arduino ピンを「入力バッファ切断・プルなし」の最小消費状態にする。
//
// 【なぜ必要か】Wire.begin() は SDA/SCL を
//   DIR=Input / INPUT=Connect / PULL=**Pullup** に設定する
//   （Wire_nRF52.cpp:55-64）。Wire.end() はこれを元に戻さない。
// Flex基板の I2C 外部プルアップ（R9/R10, 4.7kΩ）は 3V3_SW に接続されており、
// スリープ中は 3V3_SW = 0V になる。このとき nRF52 の内部プルアップ（約13kΩ）
// から 外部4.7kΩ を通って GND へ電流が流れ続ける:
//   3.3V ÷ (13kΩ + 4.7kΩ) ≒ 186µA/本 × 2本 ≒ 372µA
// これがスリープ電流悪化の主因と考えられるため、明示的に切り離す。
static inline void nrfPinDisconnect(uint8_t arduinoPin) {
  uint32_t p = g_ADigitalPinMap[arduinoPin];
  NRF_GPIO_Type *port = (p < 32) ? NRF_P0 : NRF_P1;
  port->PIN_CNF[p & 0x1F] =
      ((uint32_t)GPIO_PIN_CNF_DIR_Input        << GPIO_PIN_CNF_DIR_Pos)
    | ((uint32_t)GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos)
    | ((uint32_t)GPIO_PIN_CNF_PULL_Disabled    << GPIO_PIN_CNF_PULL_Pos)
    | ((uint32_t)GPIO_PIN_CNF_DRIVE_S0S1       << GPIO_PIN_CNF_DRIVE_Pos)
    | ((uint32_t)GPIO_PIN_CNF_SENSE_Disabled   << GPIO_PIN_CNF_SENSE_Pos);
}

void setup() {
  // ── LED を確実に消灯する ──────────────────────────────────
  // 本番ファームで判明した通り、analogWrite() は PWM ペリフェラルを
  // 起動したままにするため、ここでは最初から digitalWrite のみを使う。
#ifdef LED_RED
  pinMode(LED_RED,   OUTPUT); digitalWrite(LED_RED,   HIGH);  // アクティブLOW → HIGH=消灯
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, HIGH);
  pinMode(LED_BLUE,  OUTPUT); digitalWrite(LED_BLUE,  HIGH);
#endif

  // 3V3_SW は OFF（下流の周辺 IC に給電しない）
  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, LOW);

#if TEST_STAGE >= 1
  Serial.begin(115200);
  delay(500);
  Serial.print("[STAGE] TEST_STAGE=");
  Serial.println(TEST_STAGE);
  Serial.flush();
#endif

#if TEST_STAGE >= 2
  Wire.begin();
#endif

#if TEST_STAGE >= 3
  Serial1.setPins(SIGFOX_RX_PIN, SIGFOX_TX_PIN);
  Serial1.begin(9600);
  delay(100);
  Serial1.end();   // 本番ファームの deepSleep() と同じく閉じる
#endif

#if TEST_STAGE >= 4
  analogReadResolution(12);
  (void)analogRead(BATT_ANALOG_PIN);
#endif

#if TEST_STAGE >= 5
  InternalFS.begin();
#endif

#if TEST_STAGE >= 6
  // 本番ファームと同じ 25 分タイムアウト。一度 START すると停止不可。
  NRF_WDT->CONFIG  = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);
  NRF_WDT->CRV     = (uint32_t)((uint64_t)(25UL * 60UL * 1000UL) * 32768ULL / 1000ULL);
  NRF_WDT->RREN    = WDT_RREN_RR0_Msk;
  NRF_WDT->TASKS_START = 1;
#endif

#if TEST_STAGE >= 7
  // 本番ファームの hxBegin() と同じピン状態を再現する。
  // HX711 ライブラリの begin() は PD_SCK=OUTPUT / DOUT=INPUT に設定し、
  // 本番ファームはスリープ前にこれを戻していない。
  // D6/D7 は 4052 MUX（U4）に繋がっており、U4 は 3V3_SW（スリープ中 0V）で
  // 給電されている。電源が落ちた IC の入力へ信号を駆動すると ESD 保護ダイオード
  // 経由で電流が流れるため、これが残存電流の原因かどうかを確認する。
  pinMode(HX711_SCK_PIN, OUTPUT);
  digitalWrite(HX711_SCK_PIN, LOW);
  pinMode(HX711_DOUT_PIN, INPUT);
#endif

#if TEST_STAGE >= 8
  // 本番ファームと同じボタン割り込み設定。
  // nRF52 の attachInterrupt は GPIOTE ペリフェラルを使う。GPIOTE のエッジ検出
  // （IN event）モードは低消費電力設計では避けるべきとされることがあり、
  // これが残存電流の原因かどうかを確認する。
  pinMode(USER_BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(USER_BUTTON_PIN), btnISR, FALLING);
#endif

#if TEST_STAGE >= 9
  // 本番ファームの予備 GPIO 設定。外部が 0V に引かれていれば
  // 内部プルアップ（約13kΩ）経由で 3.3V/13kΩ ≒ 254µA が流れうる。
  pinMode(SPARE_GPIO_PIN, INPUT_PULLUP);
#endif

#if (TEST_STAGE >= 2) && APPLY_TWIM_POWER_FIX
  // ── I2C（TWIM）をスリープ前に完全に切り離す ──────────────────
  // Wire.end() は ENABLE=0 を書くだけで、以下が残ってしまうため個別に対処する:
  //   (1) TWIM ペリフェラルが完全停止しない → 強制パワーサイクル
  //   (2) IRQ が有効なまま                  → NVIC_DisableIRQ
  //   (3) SDA/SCL の内部プルアップが残る    → ピンを切断（これが電流の主因と推定）
  Wire.end();
  NVIC_DisableIRQ(SPIM0_SPIS0_TWIM0_TWIS0_SPI0_TWI0_IRQn);
  nrfPeripheralPowerCycle(TWIM0_BASE_ADDR);
  nrfPinDisconnect(PIN_WIRE_SDA);
  nrfPinDisconnect(PIN_WIRE_SCL);
#endif

  // 起動区間とスリープ区間を PPK2 波形上で見分けるための待機
  delay(BOOT_WAIT_MS);
}

void loop() {
  // FreeRTOS のブロッキング待機でスリープする（本番ファームの deepSleep() と同じ考え方）。
  // Tickless Idle が発動し、CPU が低消費電力状態に入るはず。
  // ここで PPK2 の波形を観測する。
  vTaskDelay(pdMS_TO_TICKS(60000));

#if TEST_STAGE >= 6
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;  // WDT 給餌（リセットさせない）
#endif
}
