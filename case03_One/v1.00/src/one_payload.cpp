#include "one_payload.h"

#include <string.h>

namespace one {
namespace {

void putInt16Le(uint8_t *out, int16_t value) {
  const uint16_t u = static_cast<uint16_t>(value);
  out[0] = static_cast<uint8_t>(u & 0xffU);
  out[1] = static_cast<uint8_t>(u >> 8U);
}

void putUint16Le(uint8_t *out, uint32_t value) {
  const uint16_t u = value > 65535U ? 65535U : static_cast<uint16_t>(value);
  out[0] = static_cast<uint8_t>(u & 0xffU);
  out[1] = static_cast<uint8_t>(u >> 8U);
}

void initialiseLoRa(uint8_t deviceId, uint8_t fwVersion,
                    uint8_t out[LORA_PAYLOAD_SIZE]) {
  memset(out, 0, LORA_PAYLOAD_SIZE);
  out[0] = 0x04;
  out[1] = deviceId;
  out[2] = fwVersion;
}

}  // namespace

int16_t saturateInt16(uint32_t value) {
  return value > 32767U ? INT16_MAX : static_cast<int16_t>(value);
}

int16_t saturateSignedInt16(int32_t value) {
  if (value > INT16_MAX) return INT16_MAX;
  if (value < INT16_MIN) return INT16_MIN;
  return static_cast<int16_t>(value);
}

int16_t pirAverageTimes10(uint32_t sumPeople, uint32_t scanCount) {
  if (scanCount == 0U) return MISSING_VALUE;
  // 64 bit化してから10倍し、長期運用時の中間値overflowも防ぐ。
  const uint64_t rounded =
      (static_cast<uint64_t>(sumPeople) * 10ULL + scanCount / 2U) / scanCount;
  return rounded > 32767ULL ? INT16_MAX : static_cast<int16_t>(rounded);
}

uint8_t encodeBatteryMv(uint32_t mv) {
  if (mv == 0U) return 255U;
  if (mv <= 3000U) return 0U;
  const uint32_t rounded = (mv - 3000U + 2U) / 5U;
  return rounded >= 254U ? 254U : static_cast<uint8_t>(rounded);
}

void buildSensorLoRaPayload(const SensorPayloadInput &input,
                            uint8_t deviceId, uint8_t fwVersion,
                            uint8_t out[LORA_PAYLOAD_SIZE]) {
  initialiseLoRa(deviceId, fwVersion, out);
  putInt16Le(out + 3, saturateSignedInt16(input.channel));
  // One標準版は1CH直結。未使用CHは欠測として明示する。
  putInt16Le(out + 5, MISSING_VALUE);
  putInt16Le(out + 7, MISSING_VALUE);
  putInt16Le(out + 9, MISSING_VALUE);
  putUint16Le(out + 11, input.batteryMv);
  out[13] = 0;  // DS3231非搭載
  out[14] = 0;
  out[15] = input.range;
}

void buildPirLoRaPayload(const PirPayloadInput &input,
                         uint8_t deviceId, uint8_t fwVersion,
                         uint8_t out[LORA_PAYLOAD_SIZE]) {
  initialiseLoRa(deviceId, fwVersion, out);
  const bool measured = input.scanCount != 0U;
  putInt16Le(out + 3, measured ? saturateInt16(input.maxPeople) : MISSING_VALUE);
  putInt16Le(out + 5, pirAverageTimes10(input.sumPeople, input.scanCount));
  putInt16Le(out + 7, saturateInt16(input.pirEventCount));
  putInt16Le(out + 9, saturateInt16(input.scanCount));
  putUint16Le(out + 11, input.batteryMv);
  // hour/min/rangeは初期化済みの0固定。
}

void buildSensorSigfoxPayload(const SensorPayloadInput &input,
                              uint8_t out[SIGFOX_PAYLOAD_SIZE]) {
  memset(out, 0, SIGFOX_PAYLOAD_SIZE);
  putInt16Le(out + 0, saturateSignedInt16(input.channel));
  putInt16Le(out + 2, MISSING_VALUE);
  putInt16Le(out + 4, MISSING_VALUE);
  putInt16Le(out + 6, MISSING_VALUE);
  putInt16Le(out + 8, saturateSignedInt16(input.temperatureDeciC));
  putUint16Le(out + 10, input.batteryMv);
}

void buildPirSigfoxPayload(const PirPayloadInput &input,
                           int16_t temperatureDeciC,
                           uint8_t out[SIGFOX_PAYLOAD_SIZE]) {
  memset(out, 0, SIGFOX_PAYLOAD_SIZE);
  const bool measured = input.scanCount != 0U;
  putInt16Le(out + 0, measured ? saturateInt16(input.maxPeople) : MISSING_VALUE);
  putInt16Le(out + 2, pirAverageTimes10(input.sumPeople, input.scanCount));
  putInt16Le(out + 4, saturateInt16(input.pirEventCount));
  putInt16Le(out + 6, saturateInt16(input.scanCount));
  putInt16Le(out + 8, temperatureDeciC);
  putUint16Le(out + 10, input.batteryMv);
}

}  // namespace one

