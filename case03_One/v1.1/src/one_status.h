#pragma once

#include <stdint.h>

// Arduinoに依存しない判定だけを置く（test/host_payload_test.cpp からホスト試験する）。
namespace one {

// ソーラー充電モジュール(CN3063)の状態。CHRG/DONEはオープンドレインで、
// モジュール上のショットキーで分離してOneへ出ている。LOW=アサート。
enum class ChargeState : uint8_t {
  NotCharging = 0,  // CHRG=H, DONE=H（日照なし、またはモジュール未接続）
  Charging = 1,     // CHRG=L, DONE=H
  Done = 2,         // CHRG=H, DONE=L
  Invalid = 3,      // CHRG=L, DONE=L（通常は起きない。夜間の回り込み等を疑う・要実測）
};

inline ChargeState decodeChargeState(bool chrgLow, bool doneLow) {
  if (chrgLow && doneLow) return ChargeState::Invalid;
  if (chrgLow) return ChargeState::Charging;
  if (doneLow) return ChargeState::Done;
  return ChargeState::NotCharging;
}

inline const char *chargeStateName(ChargeState state) {
  switch (state) {
    case ChargeState::NotCharging: return "idle";
    case ChargeState::Charging: return "charging";
    case ChargeState::Done: return "done";
    case ChargeState::Invalid: return "invalid";
  }
  return "invalid";
}

}  // namespace one
