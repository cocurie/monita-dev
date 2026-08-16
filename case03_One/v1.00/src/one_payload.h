#pragma once

#include <stddef.h>
#include <stdint.h>

namespace one {

constexpr size_t LORA_PAYLOAD_SIZE = 19;
constexpr size_t SIGFOX_PAYLOAD_SIZE = 12;
constexpr int16_t MISSING_VALUE = -1;

struct SensorPayloadInput {
  int32_t channel;
  int32_t temperatureDeciC;
  uint32_t batteryMv;
  uint8_t range;
};

struct PirPayloadInput {
  uint32_t maxPeople;
  uint32_t sumPeople;
  uint32_t pirEventCount;
  uint32_t scanCount;
  uint32_t batteryMv;
};

// 仕様書 §2-3。符号なし集計値が負値へ折り返さないよう、必ず飽和させる。
int16_t saturateInt16(uint32_t value);
int16_t saturateSignedInt16(int32_t value);
int16_t pirAverageTimes10(uint32_t sumPeople, uint32_t scanCount);
uint8_t encodeBatteryMv(uint32_t mv);

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

