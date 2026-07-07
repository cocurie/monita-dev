/**
 * 検証 Step17: Sigfox モジュール（BRKLSM100）の Device ID / PAC 取得
 *
 * 【対象ハード】
 *   Monita Flex v3.03 基板（Sigfox 専用）。XIAO nRF52840 Sense。
 *
 * 【目的】
 *   Sigfox バックエンド（Sigfox Buildなど）にデバイス登録する際に必要な
 *   Device ID と PAC（Porting Authorization Code）をシリアルモニタに表示する。
 *
 * 【新品モジュールの初期設定】
 *   BRKLSM100(muRata/SEONGJI LSM100A) は工場出荷時「LoRa モード」。
 *   この状態では AT$ID/AT$PAC/AT$SF など Sigfox 系 "$" コマンドが AT_ERROR になる。
 *   ID/PAC 取得前に configureSigfox() で以下を実行し「Sigfox モード＋日本 RC3C」へ
 *   恒久設定する（設定は NVM 保持されるため一度実行すれば以後の電源断でも保たれる）:
 *     AT+MODE=0 → Sigfox モードへ切替（再起動）
 *     AT$RC=3C  → 日本リージョン RC3C（既定 RC1=868MHz は日本の基地局に届かない）
 *     ATZ       → リセットして反映
 *
 * 【AT コマンド】（LSM100A User Manual 準拠。引数なし）
 *   AT$ID   → 32bit Device ID
 *   AT$PAC  → 8byte PAC
 *
 * 【使い方】
 *   1. 基板に書き込み後、シリアルモニタ（115200bps）を開く
 *   2. 起動後、自動的に Sigfox モード設定 → AT$ID / AT$PAC を送信し結果を表示する
 *   3. D0 ボタン押下で再取得できる
 */

#include <Arduino.h>
// USB CDC（Serial）。XIAO nRF52840 Sense はコア同梱の TinyUSB を明示的に使う
#include <Adafruit_TinyUSB.h>

#define SIGFOX_TX_PIN 8    // D8: XIAO TX → BRKLSM100 RX
#define SIGFOX_RX_PIN 9    // D9: XIAO RX ← BRKLSM100 TX
#define SIGFOX_BAUD   9600

#define SW_POWER_PIN  10   // D10: MOSFET_GATE → 3V3_SW ON/OFF
#define USER_BUTTON_PIN 0  // D0: タクトスイッチ（GND ショート、内部プルアップ）

// 改行付きで AT を送り、waitMs ミリ秒の間に届いた応答をすべて読んで返す
static String sendAT(const String &cmd, int waitMs = 2000) {
  // 送信前に受信バッファの残バイト（前コマンドの応答・起動バナー残渣）を破棄
  while (Serial1.available()) Serial1.read();

  Serial.print(">> ");
  Serial.println(cmd);

  Serial1.print(cmd + "\r");

  unsigned long start = millis();
  String response = "";
  while (millis() - start < (unsigned long)waitMs) {
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      response += c;
    }
  }

  Serial.print("<< ");
  Serial.println(response);
  return response;
}

// AT を投げて OK が返る（モジュール準備完了）まで待つ。成功時 true。
static bool waitReady(uint32_t timeoutMs) {
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (sendAT("AT", 1000).indexOf("OK") >= 0) return true;
    Serial.println("[Sigfox] waiting for module ready...");
  }
  return false;
}

// 新品/LoRa モードのモジュールを「Sigfox モード＋日本 RC3C」に恒久設定する。
//   工場出荷時は LoRa モードで、その状態では AT$I=10/11(ID/PAC) や AT$SF などの
//   Sigfox 系 "$" コマンドが AT_ERROR になる。
//   AT+MODE=0 … Sigfox モードへ切替（"After a MCU reset" で反映＝モジュール再起動）
//   AT$RC=3C  … 日本リージョン RC3C（既定は RC1=868MHz。日本は RC3=923MHz）
//   ATZ       … リセットして設定を反映・NVM 保持
// 設定は NVM に保持されるため、以後は電源を切っても保たれる（一度実行すれば OK）。
static void configureSigfox() {
  Serial.println("----- Sigfox 初期設定（Sigfox モード＋日本 RC3C）-----");

  Serial.print("[CFG] 現在のモード応答: ");
  Serial.println(sendAT("AT+MODE=?", 2000));

  Serial.println("[CFG] AT+MODE=0 → Sigfox モードへ切替（再起動）");
  sendAT("AT+MODE=0", 5000);
  delay(2000);                 // 起動バナー出力待ち
  if (!waitReady(12000)) {
    Serial.println("[CFG] 切替後の再起動待ちタイムアウト");
    return;
  }

  Serial.println("[CFG] AT$RC=3C → 日本リージョン設定");
  sendAT("AT$RC=3C", 4000);

  Serial.println("[CFG] ATZ → リセットして設定を反映");
  sendAT("ATZ", 3000);
  delay(2000);
  if (!waitReady(12000)) {
    Serial.println("[CFG] リセット後の再起動待ちタイムアウト");
    return;
  }

  Serial.print("[CFG] RC 確認応答 (AT$RC=?): ");
  Serial.println(sendAT("AT$RC=?", 2000));   // 3C なら日本設定 OK
  Serial.println("[CFG] 初期設定 完了");
}

static void readSigfoxIds() {
  Serial.println("----- Sigfox Device ID / PAC -----");

  // モジュール準備完了待ち（AT ping）
  if (!waitReady(10000)) {
    Serial.println("[Sigfox] module not ready: timeout");
    return;
  }

  // 新品モジュールは LoRa モードで AT$I が通らないため、先に Sigfox モード＋RC3C へ設定
  configureSigfox();

  // LSM100A User Manual: Device ID は AT$ID（引数なし）、PAC は AT$PAC（引数なし）。
  // AT$I=10 / AT$I=11 は別モジュール系統の書式でこのモジュールには存在しない。
  String idResp  = sendAT("AT$ID", 2000);
  String pacResp = sendAT("AT$PAC", 2000);

  Serial.println("----- 結果 -----");
  Serial.print("Device ID: ");
  Serial.println(idResp);
  Serial.print("PAC      : ");
  Serial.println(pacResp);
  Serial.println("--------------------------------");
}

void setup() {
  pinMode(SW_POWER_PIN, OUTPUT);
  digitalWrite(SW_POWER_PIN, HIGH);  // 3V3_SW ON

  pinMode(USER_BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  delay(2000);  // USB CDC 安定待ち
  Serial.println("=== Step17: Sigfox Device ID / PAC 取得 ===");

  Serial1.setPins(SIGFOX_RX_PIN, SIGFOX_TX_PIN);
  Serial1.begin(SIGFOX_BAUD);
  delay(3000);  // BRKLSM100 コールドスタート待ち

  readSigfoxIds();

  Serial.println("[INFO] D0 ボタンを押すと再取得します");
}

void loop() {
  if (digitalRead(USER_BUTTON_PIN) == LOW) {
    delay(20);  // チャタリング除去
    if (digitalRead(USER_BUTTON_PIN) == LOW) {
      readSigfoxIds();
      while (digitalRead(USER_BUTTON_PIN) == LOW) {
        delay(10);
      }
    }
  }
}
