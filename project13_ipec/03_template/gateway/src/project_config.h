#pragma once
#include <stdint.h>
// ★2026-09-23 FW15: 案件ごとの設定をここへ集約（既定値は FW14 と同じ）。
// GAS に記録する Gateway 名。案件の機器台帳から転記。
static const char* GATEWAY_NAME = "ipec_gw";
// GAS デプロイ URL の /s/ 以降（末尾 /exec を含む）。
static const char* GAS_SCRIPT_ID = "AKfycbzqMGKs8rnr1G__hStt3tDRYTeU5492u_-3Z828tQUQ-pAOgVc_PqnZSOGLxU6A8uAx/exec";
// GAS の群別 Drive ファイル ID を転記。
static const char* DOWNLINK_DRIVE_FILE_ID = "16cKUzyfeTFDsa6i-Xc9i7GQSQpXir1Sy";
// SIM 契約先が指定する APN。
static const char* APN = "iot.1nce.net";
// SIM の契約先名（Gateway 起動ログのシートに出す。機器台帳と合わせる）。
static const char* SIM_NAME = "1NCE";
// InfluxDB の接続 URL のホスト部分。
static const char* INFLUX_HOST = "us-east-1-1.aws.cloud2.influxdata.com";
// InfluxDB 管理画面の組織名。
static const char* INFLUX_ORG = "Co-Crea";
// InfluxDB 管理画面の保存先 bucket。
static const char* INFLUX_BUCKET = "LTE_test";
// 顧客から指定された HTTPS 接続先のホスト。
static const char* IPEC_HOST = "jg9v8fcum8.execute-api.ap-northeast-1.amazonaws.com";
// 顧客指定 URL のパス。
static const char* IPEC_PATH = "/dev";
// InfluxDB の書き込み API パス。
static const char* INFLUX_PATH = "/api/v2/write";

// ★2026-09-23 FW15: ビルド時の群・送信先指定も維持する。
#define CUSTOMER_SINK_NONE 0
#define CUSTOMER_SINK_INFLUX 1
#define CUSTOMER_SINK_IPEC 2
// 顧客の受信サービスに合わせて選ぶ。NONE は GAS と予約通信のみ。
// ★2026-09-23 FW19: Gateway の RTC は網時刻（日本時間）で合わせている。RTClib の unixtime() は
//   RTC の値を UTC とみなして秒に直すので、そのままだと9時間未来の時刻になる。
//   InfluxDB・iPEC へ送る時刻（UNIX 秒＝UTC 基準）はこの値を引いてから送る。海外案件なら変える。
#ifndef RTC_UTC_OFFSET_SEC
#define RTC_UTC_OFFSET_SEC (9L * 3600L)
#endif

#ifndef CUSTOMER_SINK
#define CUSTOMER_SINK CUSTOMER_SINK_INFLUX
#endif
// 子機 DEVICE_ID 上位3bitと同じ群番号を機器台帳から設定。
#ifndef GATEWAY_GROUP_ID
#define GATEWAY_GROUP_ID 1   // ★2026-09-24 0→1（同じ場所で別構成の検証が群0を使っているため）
#endif
// GAS へ計測データを保存する案件は1。
#ifndef SEND_TO_GAS
#define SEND_TO_GAS 1
#endif
// GAS の子機設定機能を利用する案件は1。
#ifndef ENABLE_DOWNLINK
#define ENABLE_DOWNLINK 1
#endif
// 検証時は1、本番運用時は0。案件の通信周期に合わせる。
#ifndef TEST_MODE
#define TEST_MODE 1
#endif
#if TEST_MODE
static const uint32_t SEND_INTERVAL_MS = 3UL * 60UL * 1000UL;
#else
static const uint32_t SEND_INTERVAL_MS = 60UL * 60UL * 1000UL;
#endif
static_assert(GATEWAY_GROUP_ID >= 0 && GATEWAY_GROUP_ID <= 7, "group must be 0..7");
