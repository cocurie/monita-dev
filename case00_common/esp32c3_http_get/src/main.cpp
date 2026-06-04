/**
 * W-3: HTTP GET 外部疎通確認
 * XIAO ESP32C3 — WiFi接続 → 外部サーバーへHTTP GETリクエスト
 *
 * 確認ポイント:
 *   - ステータスコード 200 が返るか
 *   - レスポンスボディが受信できるか
 *   - 繰り返し送信しても安定して動作するか
 *   - エラー時のコード・メッセージが分かるか
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include "config.h"

// ----- 関数プロトタイプ -----
bool connectWiFi();
void httpGet(const char* url);

// ----- グローバル変数 -----
WiFiMulti wifiMulti;
int requestCount = 0;

// ================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("================================");
    Serial.println("  XIAO ESP32C3  W-3");
    Serial.println("  HTTP GET 外部疎通確認");
    Serial.println("================================");

    if (!connectWiFi()) {
        Serial.println("[ERROR] WiFi接続失敗。処理を停止します。");
        while (1) delay(1000);
    }

    // 初回リクエスト
    Serial.println();
    httpGet(GET_URL_BASIC);
}

// ================================================
void loop() {
    delay(REQUEST_INTERVAL_MS);

    // WiFi 再接続チェック
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] 切断を検出 → 再接続試行");
        if (!connectWiFi()) return;
    }

    httpGet(GET_URL_BASIC);
}

// ================================================
// HTTP GET リクエスト送信
// ================================================
void httpGet(const char* url) {
    requestCount++;
    Serial.printf("\n[HTTP] #%d  GET %s\n", requestCount, url);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(10000);  // タイムアウト 10秒

    unsigned long startMs = millis();
    int httpCode = http.GET();
    unsigned long elapsed = millis() - startMs;

    if (httpCode > 0) {
        // ステータスコード表示
        Serial.printf("[HTTP] ステータスコード : %d", httpCode);
        if (httpCode == HTTP_CODE_OK) {
            Serial.println("  ✅ OK");
        } else {
            Serial.printf("  ⚠️  予期しないコード\n");
        }

        Serial.printf("[HTTP] 応答時間       : %lu ms\n", elapsed);
        Serial.printf("[HTTP] Content-Length : %d bytes\n", http.getSize());

        // レスポンスボディの先頭200文字を表示
        String body = http.getString();
        Serial.println("[HTTP] レスポンス（先頭200文字）:");
        Serial.println("       ----------------------------------------");
        Serial.println("       " + body.substring(0, 200));
        Serial.println("       ----------------------------------------");

    } else {
        // エラー詳細
        Serial.printf("[HTTP] ❌ 送信失敗  エラーコード: %d\n", httpCode);
        Serial.printf("[HTTP] エラー内容: %s\n", http.errorToString(httpCode).c_str());
        Serial.println("       確認事項:");
        Serial.println("       - WiFiは接続されているか");
        Serial.println("       - ファイアウォールでHTTP(80番)がブロックされていないか");
        Serial.println("       - URLが正しいか");
    }

    http.end();
}

// ================================================
// WiFi 接続（WiFiMulti：複数SSID自動選択）
// ================================================
bool connectWiFi() {
    wifiMulti.addAP(WIFI_SSID_1, WIFI_PASSWORD_1);
    wifiMulti.addAP(WIFI_SSID_2, WIFI_PASSWORD_2);

    Serial.print("[WiFi] 接続先を探しています");

    int retry = 0;
    while (wifiMulti.run() != WL_CONNECTED && retry < WIFI_RETRY_MAX) {
        delay(500);
        Serial.print(".");
        retry++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] ✅ 接続成功  SSID: %s  IP: %s  RSSI: %d dBm\n",
                      WiFi.SSID().c_str(),
                      WiFi.localIP().toString().c_str(),
                      WiFi.RSSI());
        return true;
    } else {
        Serial.println("[WiFi] ❌ 接続失敗（登録済みSSIDが見つかりません）");
        return false;
    }
}
