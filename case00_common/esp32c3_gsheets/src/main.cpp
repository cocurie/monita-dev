/**
 * W-4: Google Sheets への送信検証
 * XIAO ESP32C3 — センサーダミーデータを Google Apps Script 経由でシートに書き込む
 *
 * 確認ポイント:
 *   - スプレッドシートに行が追加されるか
 *   - タイムスタンプ・device_id・各CH値が正しい列に入るか
 *   - 繰り返し送信しても安定して書き込まれるか
 *
 * 方式：HTTP GET + URLクエリパラメーター
 *   GASへのPOSTはリダイレクト時のSSL処理でESP32との相性が悪いため
 *   GAS側を doGet() に変更し、GET + クエリパラメーターで送信する
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include "config.h"

// ----- 関数プロトタイプ -----
bool connectWiFi();
bool syncNTP();
bool postToSheets(float ch1, float ch2, float ch3,
                  float ch4, float ch5, float ch6,
                  float ch7, float ch8);
String getTimestamp();

// ----- グローバル変数 -----
int postCount = 0;

// ================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("================================");
    Serial.println("  XIAO ESP32C3  W-4");
    Serial.println("  Google Sheets POST 検証");
    Serial.println("================================");

    if (!connectWiFi()) {
        Serial.println("[ERROR] WiFi接続失敗。処理を停止します。");
        while (1) delay(1000);
    }

    // タイムスタンプに使うためNTP同期
    syncNTP();

    // 初回送信
    Serial.println();
    postToSheets(
        1.2345,   // ch1 ひずみ（ダミー）
        0.8901,   // ch2 ひずみ（ダミー）
        0.3456,   // ch3 ひずみ（ダミー）
        3.52,     // ch4 電圧 V（ダミー）
       -1.24,     // ch5 電圧 V（ダミー）
        0.4567,   // ch6 4ゲージ（ダミー）
        0.5678,   // ch7 4ゲージ（ダミー）
        24.5      // ch8 温度 °C（ダミー）
    );
}

// ================================================
void loop() {
    delay(POST_INTERVAL_MS);

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] 切断を検出 → 再接続試行");
        if (!connectWiFi()) return;
    }

    // ダミーデータに少しばらつきを加えて送信
    postToSheets(
        1.2345 + random(-100, 100) * 0.0001,
        0.8901 + random(-100, 100) * 0.0001,
        0.3456 + random(-100, 100) * 0.0001,
        3.52   + random(-50,   50) * 0.01,
       -1.24   + random(-50,   50) * 0.01,
        0.4567 + random(-100, 100) * 0.0001,
        0.5678 + random(-100, 100) * 0.0001,
        24.5   + random(-20,   20) * 0.1
    );
}

// ================================================
// Google Sheets へ送信（GET + URLクエリパラメーター）
// ================================================
bool postToSheets(float ch1, float ch2, float ch3,
                  float ch4, float ch5, float ch6,
                  float ch7, float ch8) {
    postCount++;
    String ts = getTimestamp();

    // GAS の URL にクエリパラメーターとしてデータを付加
    // timestamp の ":" は URL エンコード不要（GASが自動解釈する）
    char url[768];
    snprintf(url, sizeof(url),
        "%s?timestamp=%s&device_id=%s"
        "&ch1=%.4f&ch2=%.4f&ch3=%.4f"
        "&ch4=%.4f&ch5=%.4f"
        "&ch6=%.4f&ch7=%.4f&ch8=%.2f",
        GAS_URL, ts.c_str(), DEVICE_ID,
        ch1, ch2, ch3,
        ch4, ch5,
        ch6, ch7, ch8
    );

    Serial.printf("\n[GET] #%d  %s\n", postCount, ts.c_str());

    // HTTPS 接続
    WiFiClientSecure client;
    client.setInsecure();  // SSL証明書検証をスキップ（検証用途のため）

    HTTPClient http;
    http.begin(client, url);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000);

    unsigned long startMs = millis();
    int httpCode = http.GET();
    unsigned long elapsed = millis() - startMs;
    String response = http.getString();

    Serial.printf("[GET] ステータス: %d  応答時間: %lums\n", httpCode, elapsed);
    Serial.printf("[GET] GASレスポンス: %s\n", response.c_str());

    if (httpCode == HTTP_CODE_OK) {
        Serial.println("[GET] ✅ Sheets への書き込み成功");
        http.end();
        return true;
    } else {
        Serial.printf("[GET] ❌ 失敗（ステータス: %d）\n", httpCode);
        Serial.println("       確認事項:");
        Serial.println("       - GAS を doGet() に更新・再デプロイしたか");
        Serial.println("       - GASのデプロイ設定が「全員がアクセス可能」になっているか");
        http.end();
        return false;
    }
}

// ================================================
// 現在時刻を ISO8601 形式で返す（NTP同期済みの場合）
// ================================================
String getTimestamp() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        // NTP未同期の場合は起動からの経過秒を代用
        return "1970-01-01T00:00:" + String(millis() / 1000);
    }
    char buf[25];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    return String(buf);
}

// ================================================
// WiFi 接続
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
        Serial.printf("[WiFi] ✅ 接続成功  IP: %s\n",
                      WiFi.localIP().toString().c_str());
        return true;
    }
    Serial.println("[WiFi] ❌ 接続失敗");
    return false;
}

// ================================================
// NTP 同期
// ================================================
bool syncNTP() {
    configTime(9 * 3600, 0, "ntp.nict.jp", "time.google.com");
    struct tm timeinfo;
    int retry = 0;
    while (!getLocalTime(&timeinfo) && retry < 20) {
        delay(500); retry++;
    }
    if (getLocalTime(&timeinfo)) {
        Serial.println(&timeinfo, "[NTP]  ✅ 同期完了: %Y-%m-%d %H:%M:%S JST");
        return true;
    }
    Serial.println("[NTP]  ⚠️  同期失敗（タイムスタンプは暫定値になります）");
    return false;
}
