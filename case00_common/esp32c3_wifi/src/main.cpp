/**
 * Step 1: WiFi 接続確認
 * Step 2: NTP 同期・時刻取得
 *
 * XIAO ESP32C3 — WiFi接続 → NTP同期 → 時刻をシリアル出力
 *
 * 確認ポイント（Step 2）:
 *   - NTP同期が完了するか
 *   - 現在のJST時刻が正しく表示されるか
 *   - 毎秒カウントアップし続けるか（内蔵RTCのドリフト確認）
 *   - 1時間後に定期同期が走るか
 */

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "config.h"

// ----- NTP設定 -----
#define NTP_SERVER1          "ntp.nict.jp"      // 日本標準時（NICT）
#define NTP_SERVER2          "time.google.com"  // バックアップ
#define JST_OFFSET_SEC       (9 * 3600)         // UTC+9
#define NTP_SYNC_INTERVAL_MS (60 * 60 * 1000UL) // 定期同期：1時間

// ----- 関数プロトタイプ -----
bool connectWiFi();
bool syncNTP();
void printCurrentTime();
void printWiFiStatus();

// ----- グローバル変数 -----
unsigned long lastNtpSync = 0;

// ================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("================================");
    Serial.println("  XIAO ESP32C3 WiFi Step 1+2");
    Serial.println("  WiFi接続 + NTP同期・時刻取得");
    Serial.println("================================");

    // --- Step 1: WiFi接続 ---
    if (!connectWiFi()) {
        Serial.println("[ERROR] WiFi接続失敗。処理を停止します。");
        while (1) delay(1000);
    }
    printWiFiStatus();

    // --- Step 2: NTP同期 ---
    if (syncNTP()) {
        lastNtpSync = millis();
    }
}

// ================================================
void loop() {
    // 毎秒：現在時刻を出力
    printCurrentTime();

    // 定期NTP再同期（1時間ごと）
    if (millis() - lastNtpSync > NTP_SYNC_INTERVAL_MS) {
        Serial.println("[NTP] 定期同期タイミング → 再同期実行");
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] 切断を検出 → 再接続試行");
            connectWiFi();
        }
        if (syncNTP()) {
            lastNtpSync = millis();
        }
    }

    delay(1000);
}

// ================================================
// WiFi 接続（タイムアウト付き）
// ================================================
bool connectWiFi() {
    Serial.printf("[WiFi] SSID: %s に接続中", WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < WIFI_RETRY_MAX) {
        delay(500);
        Serial.print(".");
        retry++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[WiFi] ✅ 接続成功!");
        return true;
    } else {
        Serial.println("[WiFi] ❌ 接続失敗");
        Serial.printf("       WiFi.status() = %d\n", WiFi.status());
        return false;
    }
}

// ================================================
// NTP同期
// configTime() でESP32内蔵RTCをNTPサーバーと同期する
// 戻り値: true = 同期成功 / false = タイムアウト
// ================================================
bool syncNTP() {
    Serial.printf("[NTP] サーバーに接続中（%s）...\n", NTP_SERVER1);

    // NTPサーバーを設定してESP32内蔵RTCを同期開始
    configTime(JST_OFFSET_SEC, 0, NTP_SERVER1, NTP_SERVER2);

    // 同期完了を待つ（最大10秒）
    struct tm timeinfo;
    int retry = 0;
    while (!getLocalTime(&timeinfo) && retry < 20) {
        delay(500);
        Serial.print(".");
        retry++;
    }
    Serial.println();

    if (getLocalTime(&timeinfo)) {
        Serial.println("[NTP] ✅ 同期成功!");
        Serial.print("[NTP] 現在時刻: ");
        Serial.println(&timeinfo, "%Y-%m-%d %H:%M:%S JST");
        return true;
    } else {
        Serial.println("[NTP] ❌ 同期失敗（タイムアウト）");
        Serial.println("      NTPサーバーへの到達確認が必要です");
        Serial.println("      → ファイアウォール（UDP 123番ポート）の制限がないか確認");
        return false;
    }
}

// ================================================
// 現在時刻をシリアル出力（毎秒呼び出し）
// ================================================
void printCurrentTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("[TIME] ⚠️  時刻取得失敗（NTP未同期）");
        return;
    }

    // Unix時刻（エポック秒）も同時に表示 → クラウド送信フォーマットの確認用
    time_t now;
    time(&now);

    Serial.printf("[TIME] %04d-%02d-%02d %02d:%02d:%02d JST  (epoch: %lld)\n",
                  timeinfo.tm_year + 1900,
                  timeinfo.tm_mon  + 1,
                  timeinfo.tm_mday,
                  timeinfo.tm_hour,
                  timeinfo.tm_min,
                  timeinfo.tm_sec,
                  (long long)now);
}

// ================================================
// WiFi接続情報を出力
// ================================================
void printWiFiStatus() {
    Serial.println("--------------------------------");
    Serial.printf("  SSID      : %s\n", WiFi.SSID().c_str());
    Serial.printf("  IPアドレス: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("  RSSI      : %d dBm", WiFi.RSSI());
    if      (WiFi.RSSI() >= -60) Serial.println("  (良好)");
    else if (WiFi.RSSI() >= -80) Serial.println("  (普通)");
    else                         Serial.println("  (弱い)");
    Serial.println("--------------------------------");
}
