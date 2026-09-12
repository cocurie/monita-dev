/**
 * ADS131M06 ドライバ実装
 * 詳細な設計意図はヘッダのコメントを参照。
 */

#include "ADS131M06.h"

// tREGACQ（RESET 後にレジスタが既定値へ落ち着くまで）。データシートは
// 「fICLK の 5 サイクル以上」。8 MHz なら 1 µs 未満だが、余裕をみて 1 ms 待つ。
static constexpr uint32_t T_REGACQ_MS = 1;

// ─────────────────────────────────────────────────────────────
// CRC
// ─────────────────────────────────────────────────────────────
uint16_t ADS131M06::crc16(const uint8_t* data, size_t len, uint16_t seed) {
  uint16_t crc = seed;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

// ─────────────────────────────────────────────────────────────
// フレーム送受信
// ─────────────────────────────────────────────────────────────
void ADS131M06::transferFrame(uint16_t txWord0, const uint16_t* txData,
                              uint8_t nData, uint8_t* rx) {
  uint8_t tx[FRAME_BYTES];
  memset(tx, 0, sizeof(tx));

  // 語0＝コマンド。16bit の値を 24bit 語の上位へ左詰めする
  put24(&tx[0], (uint32_t)txWord0 << 8);

  // 語1以降＝WREG で書く値。通常コマンドでは nData=0
  if (txData != nullptr && nData > 0) {
    for (uint8_t i = 0; i < nData; ++i) {
      put24(&tx[(i + 1) * WORD_BYTES], (uint32_t)txData[i] << 8);
    }
  }

  // ★入力CRC は「コマンド＋データ語」の直後に置き、それ以前の語だけを対象に計算する。
  //   通常コマンドなら語1（対象は語0の3バイト）、単一WREG なら語2（対象は語0+語1の6バイト）。
  //   データシート §8.5.1.7「the command, the command CRC ... and six additional words of zeros」
  //   および §8.5.1.10.8「write this CRC after the register data」。
  if (rxCrcEn_) {
    const uint8_t crcWord    = (uint8_t)(1 + nData);
    const size_t  coveredLen = (size_t)crcWord * WORD_BYTES;
    if (crcWord < FRAME_WORDS) {
      put24(&tx[crcWord * WORD_BYTES], (uint32_t)crc16(tx, coveredLen) << 8);
    }
  }

  uint8_t scratch[FRAME_BYTES];
  uint8_t* buf = rx ? rx : scratch;
  memcpy(buf, tx, FRAME_BYTES);

  // CS の遷移は SCLK が Low の間に行う（データシート §7.6）。
  // モード1（CPOL=0）なので SCLK のアイドルは Low であり、beginTransaction 後に
  // CS を落とせばこの条件を満たす。
  spi_->beginTransaction(settings_);
  digitalWrite(csPin_, LOW);
  spi_->transfer(buf, FRAME_BYTES);   // 送受信同時（buf は送信内容が応答で上書きされる）
  digitalWrite(csPin_, HIGH);
  spi_->endTransaction();
}

uint16_t ADS131M06::command(uint16_t cmd) {
  // コマンドを投げる。この時点の応答は「1つ前のフレーム」に対するものなので捨てる
  transferFrame(cmd, nullptr, 0, nullptr);
  // 次のフレームでコマンドの応答が返る
  uint8_t rx[FRAME_BYTES];
  transferFrame(CMD_NULL, nullptr, 0, rx);
  return (uint16_t)(get24(&rx[0]) >> 8);
}

// ─────────────────────────────────────────────────────────────
// レジスタアクセス
// ─────────────────────────────────────────────────────────────
bool ADS131M06::readReg(uint8_t addr, uint16_t& value) {
  // RREG: 101a aaaa annn nnnn（nnn nnnn = 読む本数-1 = 0）
  const uint16_t cmd = (uint16_t)(0xA000 | ((uint16_t)(addr & 0x3F) << 7));
  value = command(cmd);
  return true;
}

bool ADS131M06::writeReg(uint8_t addr, uint16_t value) {
  // WREG: 011a aaaa annn nnnn（nnn nnnn = 書く本数-1 = 0）
  const uint16_t cmd = (uint16_t)(0x6000 | ((uint16_t)(addr & 0x3F) << 7));
  uint16_t data[1] = { value };          // 語1 に書き込む値を載せる（入力CRCは語2）
  transferFrame(cmd, data, 1, nullptr);

  // 応答 010a aaaa ammm mmmm を確認する。mmm mmmm は実際に書けた本数-1
  uint8_t rx[FRAME_BYTES];
  transferFrame(CMD_NULL, nullptr, 0, rx);
  const uint16_t ack = (uint16_t)(get24(&rx[0]) >> 8);
  const uint16_t expect = (uint16_t)(0x4000 | ((uint16_t)(addr & 0x3F) << 7));
  return (ack & 0xFF80) == (expect & 0xFF80);
}

bool ADS131M06::writeModeWithCrcSwitch(uint16_t mode, bool rxCrcEnAfter) {
  const uint16_t cmd = (uint16_t)(0x6000 | ((uint16_t)(REG_MODE & 0x3F) << 7));
  uint16_t data[1] = { mode };

  transferFrame(cmd, data, 1, nullptr);   // このフレームは「切替前」の設定で送る
  rxCrcEn_ = rxCrcEnAfter;                // ★次のフレームからは「切替後」が効く

  uint8_t rx[FRAME_BYTES];
  transferFrame(CMD_NULL, nullptr, 0, rx);
  const uint16_t ack = (uint16_t)(get24(&rx[0]) >> 8);
  const uint16_t expect = (uint16_t)(0x4000 | ((uint16_t)(REG_MODE & 0x3F) << 7));
  return (ack & 0xFF80) == (expect & 0xFF80);
}

// ─────────────────────────────────────────────────────────────
// 初期化
// ─────────────────────────────────────────────────────────────
bool ADS131M06::begin(SPIClass& spi, uint8_t csPin, uint8_t drdyPin,
                      const Config& cfg, uint32_t spiHz) {
  spi_     = &spi;
  csPin_   = csPin;
  drdyPin_ = drdyPin;
  // ★モード1（CPOL=0 / CPHA=1）。SD カード（モード0）と共有するため、
  //   バスを切り替えるたびに SPISettings を渡し直す（要件 F-28）
  settings_ = SPISettings(spiHz, MSBFIRST, SPI_MODE1);
  crcFallback_ = false;

  // 取りこぼし検出の時間基準。★GC の有無を見て正しい式で求める
  const double sps = dataRateSps(cfg);
  samplePeriodUs_  = (sps > 0.0) ? (uint32_t)(1000000.0 / sps + 0.5) : 0;

  if (beginOnce(cfg)) return true;

  // ★入力CRCを有効にしたまま初期化に失敗したときは、CRCを切って一度だけ再試行する。
  //
  //   【なぜこうするか】無人運用の現場で「初期化できないので何も測れない」が一番困る。
  //   一方、CRCを黙って切って動き続けるのも F-2 違反を隠すことになる。
  //   そこで **動く状態にしたうえで、切り分け結果を crcFallback() で外に出す。**
  //   呼び出し側は起動ログへ大きく出すこと（main.cpp）。
  //   これが true なら、原因は配線でも CLKIN でもなく**入力CRCの形式**だと特定できる。
  if (!cfg.rxCrcEn) return false;

  Config alt = cfg;
  alt.rxCrcEn = false;
  if (!beginOnce(alt)) return false;

  crcFallback_ = true;
  return true;
}

// 1回分の初期化。begin() から、入力CRC有り/無しで最大2回呼ばれる。
bool ADS131M06::beginOnce(const Config& cfg) {
  rxCrcEn_ = false;   // UNLOCK/RESET を送る間はまだ入力CRCを使わない
  seq_     = 0;
  stats_   = Stats();
  haveLastRead_     = false;   // 初期化中のレジスタ操作を欠測と誤判定しないため
  pendingFifoClear_ = false;

  pinMode(csPin_, OUTPUT);
  digitalWrite(csPin_, HIGH);
  pinMode(drdyPin_, INPUT);

  // インタフェースがロックされている可能性に備えて UNLOCK から入る
  command(CMD_UNLOCK);

  if (!resetByCommand()) return false;

  // ID を読んで実在確認。上位バイトは 0x26 固定（表8-14）
  uint16_t id = 0;
  readReg(REG_ID, id);
  if ((id >> 8) != 0x26) return false;

  if (!applyConfig(cfg)) return false;
  return verifyRegisters(cfg);
}

bool ADS131M06::resetByCommand() {
  // ★RESET だけは汎用の command() を使わない。
  //   RESET はフレームの末尾で成立し、**その直後 tREGACQ の間は通信してはいけない**
  //   （データシート §8.4.1.3）。command() は RESET フレームの直後に応答取得用の
  //   NULL フレームを続けて出してしまうため、禁止区間に通信することになる。
  //   2026-09-12 の実装は delay() が応答取得の「後」にあり、順序が逆だった。
  transferFrame(CMD_RESET, nullptr, 0, nullptr);

  // ★RESET でレジスタは既定値へ戻る＝入力CRCも無効になる。
  //   ドライバ側の状態を合わせておかないと、次のフレームに不要なCRC語を載せてしまう。
  rxCrcEn_ = false;

  delay(T_REGACQ_MS);   // ここで待つ。応答を取りに行く前

  uint8_t rx[FRAME_BYTES];
  transferFrame(CMD_NULL, nullptr, 0, rx);
  const uint16_t ack = (uint16_t)(get24(&rx[0]) >> 8);
  // 0xFF26 = リセット完了。0x0011 が返る場合はフレームが完結せずリセットされていない
  return ack == RESET_ACK;
}

bool ADS131M06::applyConfig(const Config& cfg) {
  bool ok = true;

  // ── MODE ──
  // bit13 REG_CRC_EN / bit12 RX_CRC_EN / bit11 CRC_TYPE(0=CCITT) / bit10 RESET
  // bit9:8 WLENGTH(01=24bit) / bit4 TIMEOUT / bit3:2 DRDY_SEL / bit1 DRDY_HiZ / bit0 DRDY_FMT
  uint16_t mode = 0;
  if (cfg.regCrcEn) mode |= (1u << 13);
  if (cfg.rxCrcEn)  mode |= (1u << 12);
  mode |= (0x1u << 8);              // WLENGTH = 24bit
  mode |= (1u << 4);                // TIMEOUT 有効（フレーム途中の停止を検出できる）
  if (cfg.drdyFmt) mode |= (1u << 0);
  // DRDY_SEL = 00b：全CHの変換が終わったときに DRDY を出す（本機は全CH使用）
  // ★MODE の書き込みは入力CRCの有効/無効が切り替わる特別なフレーム。
  //   レジスタは DIN へシフトされた時点で書かれるので、応答を読む「次のフレーム」には
  //   もう新しい設定が効いている。writeReg() のままだと切替が1フレーム遅れる。
  ok &= writeModeWithCrcSwitch(mode, cfg.rxCrcEn);

  // ── CLOCK ──
  // bit13:8 CHn_EN / bit7 XTAL_DIS / bit6 EXTREF_EN / bit4:2 OSR / bit1:0 PWR
  uint16_t clock = 0;
  clock |= (uint16_t)(cfg.chEnMask & 0x3F) << 8;
  if (cfg.disableXtal) clock |= (1u << 7);   // ★CLKIN 外部供給。内蔵発振器は止める
  if (cfg.extRef)      clock |= (1u << 6);   // ★REFIN = VEX/2 を使う。忘れるとレシオメトリックが成立しない
  clock |= (uint16_t)(cfg.osr & 0x07) << 2;
  clock |= (uint16_t)(cfg.pwr & 0x03);
  ok &= writeReg(REG_CLOCK, clock);

  // ── GAIN1（CH0〜CH3）/ GAIN2（CH4〜CH5）──
  uint16_t gain1 = (uint16_t)((cfg.gain[0] & 7)       | ((cfg.gain[1] & 7) << 4)
                            | ((cfg.gain[2] & 7) << 8) | ((cfg.gain[3] & 7) << 12));
  uint16_t gain2 = (uint16_t)((cfg.gain[4] & 7)       | ((cfg.gain[5] & 7) << 4));
  ok &= writeReg(REG_GAIN1, gain1);
  ok &= writeReg(REG_GAIN2, gain2);

  // ── CFG（グローバルチョップ）──
  // bit12:9 GC_DLY / bit8 GC_EN。電流検出（CD_*）は使わないので 0
  uint16_t cfgReg = (uint16_t)((cfg.gcDelay & 0x0F) << 9);
  if (cfg.globalChop) cfgReg |= (1u << 8);
  ok &= writeReg(REG_CFG, cfgReg);

  // ── CHn_CFG ──
  // bit15:6 PHASEn（★初期値0。同期試験の結果に応じて設定する＝要件 §4.2）
  // bit2 DCBLKn_DIS / bit1:0 MUXn
  for (uint8_t ch = 0; ch < NUM_CH; ++ch) {
    const uint8_t addr = (uint8_t)(REG_CH0_CFG + ch * CH_REG_STRIDE);
    ok &= writeReg(addr, (uint16_t)(cfg.mux[ch] & 0x03));
  }

  // ★OCAL / GCAL は書かない。生コードを保存して後処理で補正する（要件 F-8）

  return ok;
}

bool ADS131M06::verifyRegisters(const Config& cfg, Stream* log) {
  struct Item { uint8_t addr; uint16_t expect; const char* name; };

  uint16_t mode = 0;
  if (cfg.regCrcEn) mode |= (1u << 13);
  if (cfg.rxCrcEn)  mode |= (1u << 12);
  mode |= (0x1u << 8) | (1u << 4);
  if (cfg.drdyFmt) mode |= (1u << 0);

  uint16_t clock = (uint16_t)((cfg.chEnMask & 0x3F) << 8)
                 | (uint16_t)((cfg.osr & 7) << 2) | (uint16_t)(cfg.pwr & 3);
  if (cfg.disableXtal) clock |= (1u << 7);
  if (cfg.extRef)      clock |= (1u << 6);

  uint16_t gain1 = (uint16_t)((cfg.gain[0] & 7)       | ((cfg.gain[1] & 7) << 4)
                            | ((cfg.gain[2] & 7) << 8) | ((cfg.gain[3] & 7) << 12));
  uint16_t gain2 = (uint16_t)((cfg.gain[4] & 7)       | ((cfg.gain[5] & 7) << 4));
  uint16_t cfgReg = (uint16_t)((cfg.gcDelay & 0x0F) << 9);
  if (cfg.globalChop) cfgReg |= (1u << 8);

  const Item items[] = {
    { REG_MODE,  mode,   "MODE"  },
    { REG_CLOCK, clock,  "CLOCK" },
    { REG_GAIN1, gain1,  "GAIN1" },
    { REG_GAIN2, gain2,  "GAIN2" },
    { REG_CFG,   cfgReg, "CFG"   },
  };

  bool ok = true;
  for (const Item& it : items) {
    uint16_t got = 0;
    readReg(it.addr, got);
    // MODE の bit10（RESET）は状態フラグであり書いた値と一致しないため比較から外す
    const uint16_t mask = (it.addr == REG_MODE) ? (uint16_t)~(1u << 10) : 0xFFFF;
    if ((got & mask) != (it.expect & mask)) {
      ok = false;
      if (log) {
        log->printf("[ADS131M06] %s 不一致: 期待 0x%04X / 実際 0x%04X\n",
                    it.name, it.expect, got);
      }
    }
  }
  for (uint8_t ch = 0; ch < NUM_CH; ++ch) {
    uint16_t got = 0;
    readReg((uint8_t)(REG_CH0_CFG + ch * CH_REG_STRIDE), got);
    if ((got & 0x03) != (cfg.mux[ch] & 0x03)) {
      ok = false;
      if (log) log->printf("[ADS131M06] CH%u_CFG.MUX 不一致: 期待 %u / 実際 %u\n",
                           ch, cfg.mux[ch] & 3, got & 3);
    }
  }
  return ok;
}

// ─────────────────────────────────────────────────────────────
// データ読み出し
// ─────────────────────────────────────────────────────────────
bool ADS131M06::readFrame(Frame& out) {
  // ── 中断からの復帰処理（データシート §8.5.1.9.1）──
  // 出力FIFOはCHあたり2段ある。**1つでも読み落とすと両方埋まり、以後DRDYの挙動が
  // 不定になる。**「SYNC/RESET を叩く」か「2フレームを素早く読む」のどちらかが要る。
  // 本機の SYNC/RESET は I²Cエキスパンダの先にあり ISR から叩けないので、後者を使う。
  // ここで古い方を1つ捨て、直後の本読み出しが2つ目になる。
  if (pendingFifoClear_) {
    uint8_t dump[FRAME_BYTES];
    transferFrame(CMD_NULL, nullptr, 0, dump);
    stats_.fifoClears++;
    pendingFifoClear_ = false;
  }

  // ── 取りこぼしの検出（F-5）──
  // ★STATUS の DRDY ビットでは検出できない。データシート §8.5.1.9.1 のとおり
  //   DRDY フラグは「2つとも読むまで」立ったままなので、1つ落としても 0x3F のままである。
  //   （2026-09-12 の実装は `drdy != 0x3F` を欠測として数えていた。機能していない）
  //   実際に効くのは**読み出し間隔**である。1周期を大きく超えていれば、その差分が欠測。
  out.gapBefore = 0;
  const uint32_t nowUs = micros();
  if (haveLastRead_ && samplePeriodUs_ > 0) {
    const uint32_t dt = nowUs - lastReadUs_;           // オーバーフローしても差は正しい
    if (dt > samplePeriodUs_ + samplePeriodUs_ / 2) {  // 1.5周期を超えたら取りこぼし
      const uint32_t slots = (dt + samplePeriodUs_ / 2) / samplePeriodUs_;
      out.gapBefore = (slots > 0) ? (slots - 1) : 0;
      stats_.dropped += out.gapBefore;
      stats_.gaps++;
      pendingFifoClear_ = true;   // 次の読み出しの前にFIFOを空にする
    }
  }
  lastReadUs_   = nowUs;
  haveLastRead_ = true;

  uint8_t rx[FRAME_BYTES];
  transferFrame(CMD_NULL, nullptr, 0, rx);

  out.status = (uint16_t)(get24(&rx[0]) >> 8);

  for (uint8_t ch = 0; ch < NUM_CH; ++ch) {
    uint32_t raw = get24(&rx[(ch + 1) * WORD_BYTES]);
    // 24bit 2の補数 → 符号付き32bit（F-7）
    out.ch[ch] = (raw & 0x800000u) ? (int32_t)(raw | 0xFF000000u) : (int32_t)raw;
  }

  out.crcRx   = (uint16_t)(get24(&rx[7 * WORD_BYTES]) >> 8);
  out.crcCalc = crc16(rx, 7 * WORD_BYTES);
  out.crcOk   = (out.crcRx == out.crcCalc);
  out.seq     = seq_++;

  stats_.frames++;
  if (!out.crcOk) stats_.crcErrors++;
  if (out.status & (ST_CRC_ERR | ST_REG_MAP)) stats_.statusErr++;
  if (out.status & ST_F_RESYNC) stats_.resyncs++;

  lastDrdyBits_ = (uint16_t)(out.status & ST_DRDY_MASK);

  return out.crcOk;
}

bool ADS131M06::standby() { return command(CMD_STANDBY) == CMD_STANDBY; }
bool ADS131M06::wakeup()  { return command(CMD_WAKEUP)  == CMD_WAKEUP;  }
