#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/one_payload.h"

using namespace one;

static void expect(const uint8_t actual[19], const uint8_t expected[19]) {
  assert(memcmp(actual, expected, 19) == 0);
}

int main() {
  uint8_t out[19];
  const uint8_t gv1[19] = {0x04,0x0F,0x01,0x05,0x00,0x23,0x00,0x0B,0x00,0x04,0x00,0x8C,0x0F,0,0,0,0,0,0};
  buildPirLoRaPayload({5,14,11,4,3980}, 0x0F, 0x01, out); expect(out, gv1);
  assert(encodeBatteryMv(3980) == 0xC4);

  const uint8_t gv2[19] = {0x04,0x0F,0x01,0xFF,0xFF,0xFF,0xFF,0,0,0,0,0x36,0x10,0,0,0,0,0,0};
  buildPirLoRaPayload({0,0,0,0,4150}, 0x0F, 0x01, out); expect(out, gv2);
  assert(encodeBatteryMv(4150) == 0xE6);

  const uint8_t gv3[19] = {0x04,0x0F,0x01,0xFF,0xFF,0xFF,0xFF,9,0,0,0,0x74,0x0E,0,0,0,0,0,0};
  buildPirLoRaPayload({0,0,9,0,3700}, 0x0F, 0x01, out); expect(out, gv3);

  const uint8_t gv4[19] = {0x04,0x0F,0x01,1,0,8,0,4,0,4,0,0xBD,0x0B,0,0,0,0,0,0};
  buildPirLoRaPayload({1,3,4,4,3005}, 0x0F, 0x01, out); expect(out, gv4);

  const uint8_t gv5[19] = {0x04,0x0F,0x01,0xFF,0x7F,0xFF,0x7F,0xFF,0x7F,6,0,0x94,0x11,0,0,0,0,0,0};
  buildPirLoRaPayload({40000,40000,100000,6,4500}, 0x0F, 0x01, out); expect(out, gv5);
  assert(encodeBatteryMv(4500) == 0xFE);
  assert(encodeBatteryMv(2800) == 0);
  assert(encodeBatteryMv(0) == 255);

  puts("GV-1..GV-6: PASS");
  return 0;
}
