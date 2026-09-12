#include "SpiBus.h"

namespace spibus {
namespace {
// ★ISR から読まれるので volatile。書くのは main 側だけ。
volatile bool     s_busy      = false;
volatile uint32_t s_claims    = 0;
volatile uint32_t s_maxHoldUs = 0;
uint32_t          s_claimedAt = 0;
}  // namespace

bool busyFromIsr() { return s_busy; }

void claim() {
  // ★割込みを一瞬だけ止めてフラグを立てる。
  //   ここを止めずに書くと、「フラグを立てた直後・SPI転送を始める前」に DRDY 割込みが
  //   入った場合に、ISR が古い値（false）を見て ADC を読み始める可能性がある。
  //   割込み禁止中に ISR は走れないので、この窓を塞げば以降の ISR は必ず true を見る。
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  s_busy = true;
  __set_PRIMASK(primask);

  s_claimedAt = micros();
  s_claims++;
}

void release() {
  const uint32_t held = micros() - s_claimedAt;
  if (held > s_maxHoldUs) s_maxHoldUs = held;

  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  s_busy = false;
  __set_PRIMASK(primask);
}

uint32_t claims()    { return s_claims; }
uint32_t maxHoldUs() { return s_maxHoldUs; }
void resetStats()    { s_claims = 0; s_maxHoldUs = 0; }

}  // namespace spibus
