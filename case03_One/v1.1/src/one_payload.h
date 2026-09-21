#pragma once

#include <stddef.h>
#include <stdint.h>

namespace one {

constexpr size_t LORA_PAYLOAD_SIZE = 19;
constexpr size_t SIGFOX_PAYLOAD_SIZE = 12;
constexpr int16_t MISSING_VALUE = -1;
// v1.1: 充電状態(one::ChargeState 0〜3)はCH4へ載せる（Gateway改修なしでGASへ届く位置）。
// 標準センサ版: CH4 = 充電状態。PIR版: CH4 = スキャン回数(bit0〜12) | 充電状態(bit13〜14)。
constexpr uint32_t PIR_SCAN_COUNT_MAX = 8191;
constexpr uint8_t CHARGE_STATE_SHIFT = 13;

struct SensorPayloadInput {
  int32_t channel;
  int32_t temperatureDeciC;
  uint32_t batteryMv;
  uint8_t range;
  uint8_t chargeState;  // one::ChargeState の値(0〜3)
};

struct PirPayloadInput {
  uint32_t maxPeople;
  uint32_t sumPeople;
  uint32_t pirEventCount;
  uint32_t scanCount;
  uint32_t batteryMv;
  uint8_t chargeState;  // one::ChargeState の値(0〜3)
};

// 仕様書 §2-3。符号なし集計値が負値へ折り返さないよう、必ず飽和させる。
int16_t saturateInt16(uint32_t value);
int16_t saturateSignedInt16(int32_t value);
int16_t pirAverageTimes10(uint32_t sumPeople, uint32_t scanCount);
uint8_t encodeBatteryMv(uint32_t mv);
int16_t packPirScanAndCharge(uint32_t scanCount, uint8_t chargeState);

void buildSensorLoRaPayload(const SensorPayloadInput &input,
                            uint8_t deviceId, uint8_t fwVersion,
                            uint8_t out[LORA_PAYLOAD_SIZE]);
void buildPirLoRaPayload(const PirPayloadInput &input,
                         uint8_t deviceId, uint8_t fwVersion,
                         uint8_t out[LORA_PAYLOAD_SIZE]);
void buildSensorSigfoxPayload(const SensorPayloadInput &input,
                              uint8_t out[SIGFOX_PAYLOAD_SIZE]);
void buildPirSigfoxPayload(const PirPayloadInput &input,
                           int16_t temperatureDeciC,
                           uint8_t out[SIGFOX_PAYLOAD_SIZE]);

}  // namespace one

