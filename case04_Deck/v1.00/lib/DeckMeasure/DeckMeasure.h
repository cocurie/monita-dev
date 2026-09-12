/**
 * DeckMeasure — Monita Deck の計測アルゴリズム（要件定義 §7.3）
 *
 * ①長期静的変位と②準静的イベント波形を、**同一の連続ストリームから振り分けて**取得する。
 * 車両通過は①にとってはノイズ、②にとっては信号であり、両者は排他ではない。
 *
 *   常時：ADS131M06 1295.34 SPS × 6CH → SampleRing（6秒・3バイトpacked・140 kB）
 *     ├─→ BlockAverager  1秒ごとの平均 → MedianWindow 60個 → 1分値（静的変位）
 *     └─→ EventDetector  閾値判定 → 発火でリングから前1.5秒＋後3.5秒を切り出す
 *
 * 【設計上の要点】
 *
 * ★リングは 3 バイト packed でなければ入らない。
 *   `int32_t[7770][6]` は 186 kB。nRF52840 の 237 kB に対し、SDバッファ・
 *   スタック・LoRaと合わせて破綻する（要件 §7.3.3・Codexレビュー指摘）。
 *   3バイト packed なら 140 kB に収まる。
 *
 * ★イベント切出し用の複製バッファは持たない。
 *   リング6秒に対し切出しは5秒。発火後はリングから直接SDへ流し、
 *   残り1秒の余裕で書込みが追い越されないようにする。
 *
 * ★1秒平均の加算器は int64_t。
 *   24bit × 977サンプルは 32bit 符号付きの範囲を超える（Codexレビュー指摘）。
 *
 * ★メジアンは「車両を除く仕組み」である。
 *   現行 Flex の「5回平均の5回のメジアン」を 1 kSPS へ拡張したもの。
 *   静穏区間の検出はしない（高速道路の実交通量では静穏区間がほぼ無い）。
 *   メジアンは車両の存在時間割合が50%未満なら正しく基線を返すため、より頑健である。
 *
 * ★デバイスは基線窓を決めない。
 *   十分な長さの生波形を残せば、窓の取り方はすべて後処理で変更できる（要件 §7.3.3）。
 */

#pragma once

#include <Arduino.h>

namespace deck {

static constexpr uint8_t  NUM_CH = 6;

// ★実データレート。**グローバルチョップ有効なので fCLKIN/(2×OSR) ではない。**
//   fDATA = 1 / (tGC_DLY + 3 × OSR × tMOD)          （SBAS949A 式8）
//         = 4.000 MHz / (16 + 3 × 1024) = 1295.3368 SPS
//   （fMOD = fCLKIN/2 = 4.000 MHz、GC_DLY = 0011b = 16 tMOD、OSR = 1024）
//
//   ★ここと ADS131M06::Config の OSR は必ず同時に直すこと。ドライバ側の
//     ADS131M06::dataRateSps() が設定から同じ値を計算するので、起動時に
//     この定数と一致するかを照合してログへ出している（main.cpp）。
static constexpr double   F_DATA_SPS      = 1295.3368;
static constexpr uint16_t SAMPLES_PER_SEC = 1295;     // 1秒ブロックの丸め（0.99974秒）

// リング長。ちょうど6秒ぶん。7770 × 6CH × 3B = 139,860 B
// ★プリ+ポストの上限はここで決まる：(pre+post) ≦ (7770 − 1295) / 1295 = 5.0 秒
//   （SD書込みが追い越さないよう 1 秒ぶんを必ず残す）
static constexpr uint16_t RING_SAMPLES = 7770;
static constexpr uint32_t RING_BYTES   = (uint32_t)RING_SAMPLES * NUM_CH * 3;

// ─────────────────────────────────────────────────────────────
// SampleRing — 3バイト packed のリングバッファ
//
// 絶対サンプル番号（0 から単調増加）で読み書きする。イベント切出しは
// 「発火番号 − プリ長」から「発火番号 + ポスト長」を要求し、まだ上書き
// されていなければ取り出せる。上書き済みなら false を返す（＝収録失敗を
// 検出できる。黙って壊れたデータを書かない）。
// ─────────────────────────────────────────────────────────────
//
// ★サンプル番号は 64 bit。32 bit だと 2^32 / 1295 SPS ≒ 38.4 日で一周し、
//   resident()・不感時間・ポスト待ちの大小比較がすべて壊れる（Codexレビュー指摘）。
//   常設計測なので「再起動しない限り一周しない」が必要条件である。
//
// ★読み出し（main 側）と書き込み（ISR 側）は割込み禁止にせず、
//   「確認 → コピー → 再確認」で整合を取る（get / getRaw のコメント参照）。
//   18 バイト×数千サンプルのコピー中ずっと割込みを止めると、それ自体が欠測を生む。
// ─────────────────────────────────────────────────────────────
class SampleRing {
public:
  /** ISR から呼ぶ。3バイト packed で格納する（ADCのワイヤ形式と同じ MSB first） */
  void push(const int32_t ch[NUM_CH]) {
    uint8_t* p = &buf_[(size_t)head_ * SAMPLE_BYTES];
    for (uint8_t c = 0; c < NUM_CH; ++c) {
      const uint32_t v = (uint32_t)ch[c];
      *p++ = (uint8_t)(v >> 16);
      *p++ = (uint8_t)(v >> 8);
      *p++ = (uint8_t)v;
    }
    setValid(head_, true);
    advance(1);   // ★中身を書き終えてから番号を進める。get() の再確認はこの順序に依存する
  }

  /**
   * 欠測を n サンプルぶん「欠測として」記録し、番号を進める。
   *
   * ★これが無いと**サンプル番号が時刻を表さなくなる。**CRC異常やSD書込み中の
   *   取りこぼしを詰めて格納すると、継続時間・プリ/ポスト長・変化率の判定が
   *   実時間より伸び、保存波形にも欠測位置が残らない（Codexレビュー指摘）。
   *   番号は必ず実経過ぶん進め、中身は「無効」と印を付ける。
   */
  void pushGap(uint32_t n) {
    // リング1周ぶんを超える印付けは無意味なので打ち切る（長い中断でも ISR を長引かせない）
    const uint32_t mark = (n < RING_SAMPLES) ? n : RING_SAMPLES;
    for (uint32_t i = 0; i < mark; ++i) { setValid(head_, false); advance(1); }
    if (n > mark) advance(n - mark);
  }

  /**
   * これまでに書き込んだ総サンプル数（＝次に書く絶対番号）。
   *
   * ★64 bit の読み出しは Cortex-M4 では2命令に分かれ、その間に ISR が入ると
   *   上位と下位が食い違った値になる。2回読んで一致するまで繰り返す。
   *   772 µs 周期の ISR が数命令の間に2回入ることはないので、ループは高々2周で抜ける。
   */
  uint64_t count() const {
    uint64_t a, b;
    do { a = count_; b = count_; } while (a != b);
    return a;
  }

  /** 絶対番号 idx がまだリング内に残っているか */
  bool resident(uint64_t idx) const {
    const uint64_t n = count();
    return idx < n && (n - idx) <= RING_SAMPLES;
  }

  /** 絶対番号 idx のサンプルが有効か（欠測でないか）。上書き済みかどうかは見ない */
  bool valid(uint64_t idx) const {
    const uint32_t slot = (uint32_t)(idx % RING_SAMPLES);
    return ((valid_[slot >> 3] >> (slot & 7)) & 1) != 0;
  }

  /**
   * 1サンプル取り出す。上書き済み・欠測なら false。
   *
   * ★main 側から呼ぶ。コピーの途中で ISR が同じスロットを上書きすると、
   *   **新旧のバイトが混ざった値を「正常」として返してしまう**（Codexレビュー指摘）。
   *   そこでコピーの後にもう一度 resident() を確かめる。
   *     - スロット idx を上書きする push() は、書き終えてから count を idx+RING+1 へ進める。
   *     - ISR は main を追い越すが途中で止まらないので、コピー中に上書きが起きていれば、
   *       再確認の時点で必ず count が進んでおり resident() が false になる。
   *   有効ビットも同じスロットの上書きでしか変わらないので、再確認は resident() だけでよい。
   *   （コピー直後に上書きされた場合も false になるが、捨てる側に倒れるだけで害はない）
   */
  bool get(uint64_t idx, int32_t out[NUM_CH]) const {
    uint8_t raw[SAMPLE_BYTES];
    if (!getRaw(idx, raw)) return false;
    const uint8_t* p = raw;
    for (uint8_t c = 0; c < NUM_CH; ++c) {
      uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
      out[c] = (v & 0x800000u) ? (int32_t)(v | 0xFF000000u) : (int32_t)v;
      p += 3;
    }
    return true;
  }

  /**
   * 生バイト列をそのまま取り出す（SDへストリーミングするため）。
   * 1サンプル = 18バイト。out は 18 バイト以上であること。
   * 整合の取り方は get() と同じ（確認 → コピー → 再確認）。
   */
  bool getRaw(uint64_t idx, uint8_t* out18) const {
    if (!resident(idx) || !valid(idx)) return false;
    memcpy(out18, &buf_[(size_t)(idx % RING_SAMPLES) * SAMPLE_BYTES], SAMPLE_BYTES);
    return resident(idx);   // ★再確認。コピー中に上書きされていたら捨てる
  }

  static constexpr uint8_t SAMPLE_BYTES = NUM_CH * 3;   // 18

private:
  void setValid(uint32_t slot, bool v) {
    uint8_t& b = valid_[slot >> 3];
    const uint8_t m = (uint8_t)(1u << (slot & 7));
    if (v) b |= m; else b = (uint8_t)(b & ~m);
  }

  // head_ は count_ % RING_SAMPLES と常に等しい。ISR で 64 bit の剰余（libgcc 呼び出し）を
  // 毎サンプル計算しないために別に持つ。ISR 側だけが書く。
  void advance(uint32_t n) {
    head_ = (uint16_t)((head_ + (n % RING_SAMPLES)) % RING_SAMPLES);
    count_ += n;
  }

  // 平坦な配列にする。多次元だとポインタ型が uint8_t(*)[3] になって扱いにくい
  uint8_t buf_[(size_t)RING_SAMPLES * SAMPLE_BYTES];
  // 有効/欠測のビットマップ。1サンプル1ビットなので 972 B しか要らない。
  // センチネル値（0x800000 等）で表すと実データと区別できないため、別に持つ。
  uint8_t valid_[(RING_SAMPLES + 7) / 8] = {0};
  uint16_t head_ = 0;
  volatile uint64_t count_ = 0;   // ISR 側だけが書く。main 側は count() で読むこと
};

// ─────────────────────────────────────────────────────────────
// BlockAverager — 1秒（977サンプル）ごとの平均。①静的系列の入口
//
// ★加算器は int64_t。24bit × 977 は int32_t に入らない。
// ─────────────────────────────────────────────────────────────
class BlockAverager {
public:
  explicit BlockAverager(uint16_t blockSize = SAMPLES_PER_SEC) : blockSize_(blockSize) {}

  /** ブロックが埋まったら out に平均を入れて true を返す */
  bool push(const int32_t ch[NUM_CH], int32_t out[NUM_CH]) {
    for (uint8_t c = 0; c < NUM_CH; ++c) acc_[c] += ch[c];
    if (++n_ < blockSize_) return false;
    for (uint8_t c = 0; c < NUM_CH; ++c) {
      out[c] = (int32_t)(acc_[c] / (int64_t)blockSize_);
      acc_[c] = 0;
    }
    n_ = 0;
    return true;
  }

  void reset() { for (auto& a : acc_) a = 0; n_ = 0; }

private:
  int64_t  acc_[NUM_CH] = {0};
  uint16_t n_ = 0;
  uint16_t blockSize_;
};

// ─────────────────────────────────────────────────────────────
// MedianWindow — 直近 N 個のブロック平均のメジアン。①静的変位の本体
//
// ★これが「車両を除く仕組み」である。平均はノイズ低減、メジアンは外れ値除去。
//   車両の存在時間割合が50%未満であれば、メジアンは正しく基線を返す。
// ─────────────────────────────────────────────────────────────
template <uint8_t N>
class MedianWindow {
public:
  void push(const int32_t ch[NUM_CH]) {
    for (uint8_t c = 0; c < NUM_CH; ++c) hist_[c][idx_] = ch[c];
    idx_ = (uint8_t)((idx_ + 1) % N);
    if (filled_ < N) filled_++;
  }

  bool full() const { return filled_ >= N; }
  uint8_t count() const { return filled_; }

  /** 現在のメジアン。まだ1個も入っていなければ false */
  bool median(int32_t out[NUM_CH]) const {
    if (filled_ == 0) return false;
    int32_t tmp[N];
    for (uint8_t c = 0; c < NUM_CH; ++c) {
      memcpy(tmp, hist_[c], sizeof(int32_t) * filled_);
      // 挿入ソート。N=60・毎分1回なので十分に速い
      for (uint8_t i = 1; i < filled_; ++i) {
        const int32_t v = tmp[i];
        int8_t j = (int8_t)(i - 1);
        while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; --j; }
        tmp[j + 1] = v;
      }
      // 偶数個のときは中央2つの平均をとる
      out[c] = (filled_ & 1) ? tmp[filled_ / 2]
                             : (int32_t)(((int64_t)tmp[filled_ / 2 - 1] + tmp[filled_ / 2]) / 2);
    }
    return true;
  }

  void reset() { idx_ = 0; filled_ = 0; }

private:
  int32_t hist_[NUM_CH][N] = {{0}};
  uint8_t idx_ = 0;
  uint8_t filled_ = 0;
};

// ─────────────────────────────────────────────────────────────
// TriggerConfig — ダウンリンクで遠隔変更される 13 バイト（要件 §7.3.7 / M-11）
//
// バイト順は Gateway 改修範囲と一致させること。変更値と反映時刻は
// 必ずログに記録する（KPIの判定方法）。
// ─────────────────────────────────────────────────────────────
enum class ThreshMode : uint8_t {
  Absolute   = 0,   // 絶対値
  FromBase   = 1,   // 基線からの変化量（★既定。温度で基線が動いても発火数が変わらない）
  Rate       = 2,   // 変化率
};

enum class DecisionMode : uint8_t {
  Or     = 0,       // いずれか1CH
  And    = 1,       // 対象CH全部
  NofM   = 2,       // N個以上（nRequired を使う）
};

struct TriggerConfig {
  bool         enabled      = true;
  uint16_t     threshold    = 200;     // ADCコード（ThreshMode に応じた解釈）
  ThreshMode   threshMode   = ThreshMode::FromBase;
  uint8_t      chMask       = 0x3F;    // 監視するCH（6bit）
  DecisionMode decision     = DecisionMode::Or;
  uint8_t      nRequired    = 2;       // NofM のときの N
  uint8_t      durationMs   = 20;      // 何ms超え続けたら発火（スパイク除去）
  uint8_t      deadTimeMs10 = 50;      // 不感時間（×10ms。50 = 500ms）
  uint8_t      preTenth     = 15;      // プリトリガ（×0.1秒。15 = 1.5秒）
  uint8_t      postTenth    = 35;      // ポストトリガ（×0.1秒。35 = 3.5秒）
  uint16_t     maxPerHour   = 200;     // レート制限（R-1）
  uint8_t      staticMin    = 1;       // 静的計測の周期（分）

  /**
   * ★プリ＋ポストがリング長を超えていないか検査する（要件 §7.3.3 Codex指摘）。
   * リング6秒に対しプリ3秒＋ポスト8秒は両立しない。**受理前に必ず呼ぶこと。**
   * SDへ書く間の余裕として 1 秒を残す。
   */
  bool fitsInRing() const {
    const uint32_t need = (uint32_t)(preTenth + postTenth) * SAMPLES_PER_SEC / 10;
    return need + SAMPLES_PER_SEC <= RING_SAMPLES;
  }

  uint32_t preSamples()  const { return (uint32_t)preTenth  * SAMPLES_PER_SEC / 10; }
  uint32_t postSamples() const { return (uint32_t)postTenth * SAMPLES_PER_SEC / 10; }

  /** ダウンリンク 13 バイトから復元する。範囲外なら false（設定は変更しない） */
  bool fromBytes(const uint8_t b[13]);
  /** ダウンリンク 13 バイトへ詰める */
  void toBytes(uint8_t b[13]) const;
};

// ─────────────────────────────────────────────────────────────
// EventDetector — 閾値判定と切出し範囲の決定
// ─────────────────────────────────────────────────────────────
struct EventWindow {
  uint64_t triggerIdx = 0;   // 発火した絶対サンプル番号
  uint64_t startIdx   = 0;   // 切出し開始（= triggerIdx - preSamples）
  uint64_t endIdx     = 0;   // 切出し終了（含まない）
  uint8_t  firedMask  = 0;   // 発火に寄与したCH
  uint32_t seq        = 0;   // イベント連番
};

// ★update() は ISR から呼ばれる。**setConfig() / setBaseline() / rollHour() を main 側から
//   呼ぶときは、必ず割込み禁止で囲むこと。**これらは複数バイトの状態を書き換えるので、
//   途中で ISR が入ると「半分だけ新しい設定」「CH1〜3 だけ新しい基線」で判定したり、
//   ISR の detected_++ とリセットがぶつかって件数が狂ったりする（Codexレビュー指摘）。
//   ライブラリはホストでも試験するため、割込み禁止はライブラリ内ではなく呼び出し側（main.cpp）で行う。
class EventDetector {
public:
  /** 設定を入れる。ISR 内で浮動小数を使わないよう、サンプル数へ先に換算しておく */
  void setConfig(const TriggerConfig& c) {
    cfg_ = c;
    durSamples_  = (uint32_t)((double)c.durationMs   * F_DATA_SPS / 1000.0 + 0.5);
    deadSamples_ = (uint32_t)((double)c.deadTimeMs10 * 10.0 * F_DATA_SPS / 1000.0 + 0.5);
  }
  const TriggerConfig& config() const { return cfg_; }

  /** 基線を与える（1分メジアン）。ThreshMode::FromBase で使う */
  void setBaseline(const int32_t base[NUM_CH]) {
    memcpy(baseline_, base, sizeof(baseline_));
    haveBaseline_ = true;
  }

  /**
   * 毎サンプル呼ぶ。発火したら true を返し win を埋める。
   * @param idx このサンプルの絶対番号（SampleRing::count() を push 前に取ったもの）
   */
  bool update(uint64_t idx, const int32_t ch[NUM_CH], EventWindow& win);

  /** 1時間の区切りでカウンタを回す。R-2：上限に達しても検出カウントは続ける */
  void rollHour();

  uint16_t detectedThisHour() const { return detected_; }
  uint16_t recordedThisHour() const { return recorded_; }
  bool     rateLimited()      const { return recorded_ >= cfg_.maxPerHour; }
  /** 収録した（SDへ書けた）ことを通知する */
  void notifyRecorded() { if (recorded_ < 0xFFFF) recorded_++; }

  uint32_t eventSeq() const { return seq_; }

private:
  TriggerConfig cfg_;
  int32_t  baseline_[NUM_CH] = {0};
  bool     haveBaseline_ = false;
  int32_t  prev_[NUM_CH] = {0};
  bool     havePrev_ = false;

  uint64_t overSince_ = 0;     // 条件成立が始まった絶対番号
  bool     over_ = false;
  uint64_t deadUntil_ = 0;     // この番号までは再発火しない
  uint32_t durSamples_  = 0;   // durationMs をサンプル数へ換算したもの（ISR で使う）
  uint32_t deadSamples_ = 0;   // deadTimeMs10 を同上
  uint32_t seq_ = 0;
  uint16_t detected_ = 0;
  uint16_t recorded_ = 0;

  /**
   * 1CH分の閾値判定。
   * ★prev / havePrev は**呼び出し側が退避した前サンプル**を渡すこと。メンバの prev_ を
   *   見てはいけない。update() は早期 return が多く、前値の更新を先頭でまとめて行う都合上、
   *   判定時点の prev_ は既に「今のサンプル」に書き換わっている（＝差分が常に 0 になる）。
   *   2026-09-12 の実装はこれで Rate モードが一度も発火しなかった。
   */
  bool chOverThreshold(uint8_t c, const int32_t ch[NUM_CH],
                       const int32_t prev[NUM_CH], bool havePrev) const;
};

}  // namespace deck
