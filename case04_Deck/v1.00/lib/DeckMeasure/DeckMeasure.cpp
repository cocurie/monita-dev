/**
 * DeckMeasure 実装。設計意図はヘッダのコメントを参照。
 */

#include "DeckMeasure.h"

namespace deck {

// ─────────────────────────────────────────────────────────────
// TriggerConfig — ダウンリンク 13 バイトとの相互変換
//
// バイト配置（要件 §7.3.7 の表の順）。Gateway 改修範囲と一致させること。
//   [0] 収録有効/無効
//   [1..2] 閾値（ビッグエンディアン）
//   [3] 閾値の定義
//   [4] 対象CHマスク（下位6bit）
//   [5] 判定方式（上位4bit = N / 下位4bit = 方式）
//   [6] 継続時間 [ms]
//   [7] 不感時間 [×10ms]
//   [8] プリトリガ [×0.1s]
//   [9] ポストトリガ [×0.1s]
//   [10..11] 時間あたり最大収録件数（ビッグエンディアン）
//   [12] 静的計測の周期 [分]
// ─────────────────────────────────────────────────────────────
bool TriggerConfig::fromBytes(const uint8_t b[13]) {
  TriggerConfig t;
  t.enabled      = (b[0] != 0);
  t.threshold    = (uint16_t)((uint16_t)b[1] << 8 | b[2]);
  if (b[3] > (uint8_t)ThreshMode::Rate) return false;
  t.threshMode   = (ThreshMode)b[3];
  t.chMask       = (uint8_t)(b[4] & 0x3F);
  const uint8_t mode = (uint8_t)(b[5] & 0x0F);
  if (mode > (uint8_t)DecisionMode::NofM) return false;
  t.decision     = (DecisionMode)mode;
  t.nRequired    = (uint8_t)(b[5] >> 4);
  t.durationMs   = b[6];
  t.deadTimeMs10 = b[7];
  t.preTenth     = b[8];
  t.postTenth    = b[9];
  t.maxPerHour   = (uint16_t)((uint16_t)b[10] << 8 | b[11]);
  t.staticMin    = b[12];

  // ── 受理前の妥当性検査 ──
  if (t.chMask == 0) return false;                       // 監視CHが無い
  if (t.staticMin == 0) return false;
  if (t.decision == DecisionMode::NofM &&
      (t.nRequired == 0 || t.nRequired > NUM_CH)) return false;
  // ★プリ＋ポストがリングに収まらない設定は受理しない（要件 §7.3.3）
  if (!t.fitsInRing()) return false;
  if (t.preTenth < 5 || t.preTenth > 30) return false;   // 0.5〜3.0 秒
  if (t.postTenth < 10) return false;                    // 1.0 秒以上

  *this = t;
  return true;
}

void TriggerConfig::toBytes(uint8_t b[13]) const {
  b[0]  = enabled ? 1 : 0;
  b[1]  = (uint8_t)(threshold >> 8);
  b[2]  = (uint8_t)threshold;
  b[3]  = (uint8_t)threshMode;
  b[4]  = (uint8_t)(chMask & 0x3F);
  b[5]  = (uint8_t)(((nRequired & 0x0F) << 4) | ((uint8_t)decision & 0x0F));
  b[6]  = durationMs;
  b[7]  = deadTimeMs10;
  b[8]  = preTenth;
  b[9]  = postTenth;
  b[10] = (uint8_t)(maxPerHour >> 8);
  b[11] = (uint8_t)maxPerHour;
  b[12] = staticMin;
}

// ─────────────────────────────────────────────────────────────
// EventDetector
// ─────────────────────────────────────────────────────────────
bool EventDetector::chOverThreshold(uint8_t c, const int32_t ch[NUM_CH]) const {
  switch (cfg_.threshMode) {
    case ThreshMode::Absolute:
      return (uint32_t)abs(ch[c]) >= cfg_.threshold;

    case ThreshMode::FromBase:
      // ★既定。基線からの変化量で見る。絶対値だと温度で基線が動いたときに
      //   発火数が変わってしまう（要件 §7.3.7）。基線が未設定のうちは発火させない
      if (!haveBaseline_) return false;
      return (uint32_t)abs((int64_t)ch[c] - baseline_[c]) >= cfg_.threshold;

    case ThreshMode::Rate:
      // 1サンプル間の変化量。ノイズに弱いので durationMs と併用する前提
      if (!havePrev_) return false;
      return (uint32_t)abs((int64_t)ch[c] - prev_[c]) >= cfg_.threshold;
  }
  return false;
}

bool EventDetector::update(uint32_t idx, const int32_t ch[NUM_CH], EventWindow& win) {
  // 前値は Rate 判定に使うので、早期 return の前に必ず更新する
  int32_t prevSnapshot[NUM_CH];
  memcpy(prevSnapshot, prev_, sizeof(prevSnapshot));
  const bool hadPrev = havePrev_;
  memcpy(prev_, ch, sizeof(prev_));
  havePrev_ = true;
  (void)prevSnapshot; (void)hadPrev;

  if (!cfg_.enabled) { over_ = false; return false; }
  if (idx < deadUntil_) { over_ = false; return false; }

  // ── 各CHの判定 ──
  uint8_t firedMask = 0;
  uint8_t nOver = 0, nWatched = 0;
  for (uint8_t c = 0; c < NUM_CH; ++c) {
    if (!(cfg_.chMask & (1u << c))) continue;
    nWatched++;
    if (chOverThreshold(c, ch)) { firedMask |= (uint8_t)(1u << c); nOver++; }
  }

  bool cond = false;
  switch (cfg_.decision) {
    case DecisionMode::Or:   cond = (nOver >= 1); break;
    case DecisionMode::And:  cond = (nWatched > 0 && nOver == nWatched); break;
    case DecisionMode::NofM: cond = (nOver >= cfg_.nRequired); break;
  }

  if (!cond) { over_ = false; return false; }

  // ── 継続時間条件（スパイク除去）──
  if (!over_) { over_ = true; overSince_ = idx; }
  if (idx - overSince_ < durSamples_) return false;

  // ── 発火 ──
  // ★発火時点ではイベントの始まりはすでに過去にある。閾値を超えたと分かるのは
  //   床版が沈み始めた後であり、そこから記録を始めても基線が撮れない。
  //   だから overSince_（超え始め）を基準にプリトリガを遡る。
  const uint32_t trig = overSince_;
  const uint32_t pre  = cfg_.preSamples();

  win.triggerIdx = trig;
  win.startIdx   = (trig > pre) ? (trig - pre) : 0;
  win.endIdx     = trig + cfg_.postSamples();
  win.firedMask  = firedMask;
  win.seq        = seq_++;

  if (detected_ < 0xFFFF) detected_++;   // R-3：検出件数は上限に関係なく数える

  over_      = false;
  deadUntil_ = idx + deadSamples_;
  return true;
}

void EventDetector::rollHour() {
  detected_ = 0;
  recorded_ = 0;
}

}  // namespace deck
