// NimBLEWrapper.cpp
#include <NimBLEDevice.h>
#include <vector>
#include <cstring>  // For strcmp

#include <regex>
#include <string>
#include <stdio.h>
#include <stdlib.h>

void handleUartCommunication(String& input);
void handleDeviceSelection(String& input);

extern NimBLEAdvertisedDevice* myDevice;
extern std::vector<NimBLEAdvertisedDevice*> foundDevices;

extern boolean doConnect;
extern boolean connected;
extern boolean doScan;

extern "C" {
    // デバイスアドレスに基づいて myDevice を設定する関数
    void setMyDeviceByAddress(const char* address) {
        for (size_t i = 0; i < foundDevices.size(); ++i) {
            if (strcmp(foundDevices[i]->getAddress().toString().c_str(), address) == 0  && !connected) {
                myDevice = foundDevices[i];
                doScan = false;
                doConnect = true;
                break;
            }
        }
    
    }

    // 他に必要なラッパー関数を追加
}

// C言語から呼び出すためのラッパー関数を作成
// UART通信を処理するラッパー関数
extern "C" void handleUartCommunicationWrapper(const char* input) {
    String inputStr(input);  // Cの文字列をArduinoのStringに変換
    handleUartCommunication(inputStr);
}

extern "C" void rescan() {
    String command="9999\n";
    handleDeviceSelection(command);
}

// C++関数をCコードから呼び出せるようにextern "C"でラップします
//正規化・+10.000~-10.000の範囲・デシマルなし6桁に変更
extern "C" {
    int validate_and_format_value(const char* value, char* output);
}

std::string formatToSixDigits(const char* value) {
    double numValue = atof(value);
    //int mVValue = static_cast<int>(numValue * 1000); //V入力のとき
    int mVValue = static_cast<int>(numValue);//mV入力のとき
    char buffer[7];
    snprintf(buffer, sizeof(buffer), "%06d", mVValue);
    return std::string(buffer);
}

int validate_and_format_value(const char* value, char* output) {
    //std::regex pattern(R"(^[-+]?\d{1,2}(\.\d{0,3})?$)");//V入力のとき
    std::regex pattern(R"(^[-+]?\d{1,6}$)");//mV入力のとき
    
    if (std::regex_match(value, pattern)) {
        // 数値に変換して範囲をチェック
        double numValue = atof(value);
        //if (numValue >= -10.0 && numValue <= 10.0) {//V入力のとき
        if (numValue >= -999999 && numValue <= 999999) {//mV入力のとき
            std::string formattedValue = formatToSixDigits(value);
            strncpy(output, formattedValue.c_str(), 7);  // 6桁+終端文字
            return 1;  // 有効な入力
        } else {
            strncpy(output, "000000", 7);
            return 0;  // 無効な入力 (範囲外)
        }
    } else {
        strncpy(output, "000000", 7);
        return 0;  // 無効な入力 (正規表現にマッチしない)
    }
}

bool isValidRTCFormat(const char* rtcValue) {
    // 長さチェック
    if (strlen(rtcValue) != 12) {
        return false;
    }
    // 数字のみかをチェック
    for (int i = 0; i < 12; i++) {
        if (!isDigit(rtcValue[i])) {
            return false;
        }
    }
    return true;
}

bool normalizeRTCValue(const char* rtcValue, char* normalizedRTC) {
    // フォーマットのチェック
    if (strlen(rtcValue) != 12) {
        return false; // 無効なフォーマット
    }
    // 数字のみかをチェック
    for (int i = 0; i < 12; i++) {
        if (!isDigit(rtcValue[i])) {
            return false; // 無効なフォーマット
        }
    }

    int yy = (rtcValue[0] - '0') * 10 + (rtcValue[1] - '0');
    int mm = (rtcValue[2] - '0') * 10 + (rtcValue[3] - '0');
    int dd = (rtcValue[4] - '0') * 10 + (rtcValue[5] - '0');
    int hh = (rtcValue[6] - '0') * 10 + (rtcValue[7] - '0');
    int mi = (rtcValue[8] - '0') * 10 + (rtcValue[9] - '0');
    int ss = (rtcValue[10] - '0') * 10 + (rtcValue[11] - '0');

    // 年 (yy): 24 ~ 30
    if (yy < 24) yy = 24;
    if (yy > 30) yy = 30;

    // 月 (mm): 1 ~ 12
    if (mm < 1) mm = 1;
    if (mm > 12) mm = 12;

    // 日 (dd): 1 ~ 31
    if (dd < 1) dd = 1;
    if (dd > 31) dd = 31;

    // 時 (hh): 1 ~ 24
    if (hh < 1) hh = 1;
    if (hh > 24) hh = 24;

    // 分 (mi): 0 ~ 59
    if (mi < 0) mi = 0;
    if (mi >= 60) mi = 59;

    // 秒 (ss): 0 ~ 59
    if (ss < 0) ss = 0;
    if (ss >= 60) ss = 59;

    snprintf(normalizedRTC, 13, "%02d%02d%02d%02d%02d%02d", yy, mm, dd, hh, mi, ss);
    return true; // 正常に処理が完了
}
extern "C" {
    _Bool wrapperNormalizeRTCValue(const char* rtcValue, char* normalizedRTC) {
        return normalizeRTCValue(rtcValue, normalizedRTC);
    }
}
