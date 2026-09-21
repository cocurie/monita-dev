#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/one_payload.h"
#include "../src/one_status.h"

using namespace one;

static void expect(const uint8_t actual[19], const uint8_t expected[19]) {
  assert(memcmp(actual, expected, 19) == 0);
}

int main() {
  uint8_t out[19];
  const uint8_t gv1[19] = {0x04,0x0F,0x01,0x05,0x00,0x23,0x00,0x0B,0x00,0x04,0x00,0x8C,0x0F,0,0,0,0,0,0};
  buildPirLoRaPayload({5,14,11,4,3980,0}, 0x0F, 0x01, out); expect(out, gv1);
  assert(encodeBatteryMv(3980) == 0xC4);

  const uint8_t gv2[19] = {0x04,0x0F,0x01,0xFF,0xFF,0xFF,0xFF,0,0,0,0,0x36,0x10,0,0,0,0,0,0};
  buildPirLoRaPayload({0,0,0,0,4150,0}, 0x0F, 0x01, out); expect(out, gv2);
  assert(encodeBatteryMv(4150) == 0xE6);

  const uint8_t gv3[19] = {0x04,0x0F,0x01,0xFF,0xFF,0xFF,0xFF,9,0,0,0,0x74,0x0E,0,0,0,0,0,0};
  buildPirLoRaPayload({0,0,9,0,3700,0}, 0x0F, 0x01, out); expect(out, gv3);

  const uint8_t gv4[19] = {0x04,0x0F,0x01,1,0,8,0,4,0,4,0,0xBD,0x0B,0,0,0,0,0,0};
  buildPirLoRaPayload({1,3,4,4,3005,0}, 0x0F, 0x01, out); expect(out, gv4);

  const uint8_t gv5[19] = {0x04,0x0F,0x01,0xFF,0x7F,0xFF,0x7F,0xFF,0x7F,6,0,0x94,0x11,0,0,0,0,0,0};
  buildPirLoRaPayload({40000,40000,100000,6,4500,0}, 0x0F, 0x01, out); expect(out, gv5);
  assert(encodeBatteryMv(4500) == 0xFE);
  assert(encodeBatteryMv(2800) == 0);
  assert(encodeBatteryMv(0) == 255);

  puts("GV-1..GV-6: PASS");

  // v1.1 充電モジュール CHRG/DONE（LOW=アサート）の判定。
  assert(decodeChargeState(false, false) == ChargeState::NotCharging);
  assert(decodeChargeState(true, false) == ChargeState::Charging);
  assert(decodeChargeState(false, true) == ChargeState::Done);
  assert(decodeChargeState(true, true) == ChargeState::Invalid);
  assert(strcmp(chargeStateName(ChargeState::Charging), "charging") == 0);
  puts("ChargeState: PASS");

  // GV-7: PIR版 CH4 = スキャン回数 | 充電状態<<13（GV-1と同条件で charging）。
  const uint8_t gv7[19] = {0x04,0x0F,0x01,0x05,0x00,0x23,0x00,0x0B,0x00,0x04,0x20,0x8C,0x0F,0,0,0,0,0,0};
  buildPirLoRaPayload({5,14,11,4,3980,1}, 0x0F, 0x01, out); expect(out, gv7);
  assert(packPirScanAndCharge(8191, 0) == 8191);
  assert(packPirScanAndCharge(10000, 3) == 32767);  // スキャン回数は8191で飽和
  assert(packPirScanAndCharge(0, 2) == 16384);
  assert(packPirScanAndCharge(1440, 3) == (1440 | (3 << 13)));

  // GV-8: 標準センサ版 CH4 = 充電状態（done）。
  const uint8_t gv8[19] = {0x04,0x0F,0x05,0xD2,0x04,0xFF,0xFF,0xFF,0xFF,0x02,0x00,0x74,0x0E,0,0,0x07,0,0,0};
  buildSensorLoRaPayload({1234,250,3700,7,2}, 0x0F, 0x05, out); expect(out, gv8);
  puts("GV-7..GV-8 (charge state): PASS");
  return 0;
}
