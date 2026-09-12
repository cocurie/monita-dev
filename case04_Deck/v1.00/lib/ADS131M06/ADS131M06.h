/**
 * ADS131M06 — 6CH 同時サンプリング 24bit ΔΣ ADC ドライバ
 *
 * 【対象】
 *   TI ADS131M06IRSNR（WQFN-32 4×4）／ Monita Deck 基板 ver1.00 の U2
 *   データシート: SBAS949A（2020-02 / 2021-02改訂）
 *
 * 【本機での構成】要件定義 §3〜§4
 *   - CLKIN = MCU の PWM で 8.000 MHz を供給（水晶なし）。fMOD = 4.000 MHz
 *   - **グローバルチョップ有効。OSR 1024 → fDATA = 1295.34 SPS**
 *     ★`fCLKIN/(2×OSR)` はグローバルチョップ**無効時**の式である。有効時は
 *       `tGC = tGC_DLY + 3 × OSR × tMOD`（SBAS949A 式8）で、およそ 1/3 になる。
 *       この取り違えで一度 976.56 SPS 前提の定数を組んだ（実際は 325 SPS だった）。
 *   - PGA ゲイン 32（ノイズはゲイン32以上で頭打ち。選択はFSR余裕だけの問題）
 *   - **外部リファレンス REFIN = VEX/2 = 1.25 V**（レシオメトリック測定）
 *     → CLOCK レジスタの **EXTREF_EN を必ず 1 にする**。これを忘れると内蔵1.2Vが使われ、
 *       VEX のドリフトが相殺されなくなる（設計の根幹が失われる）
 *   - 内蔵水晶発振器は使わないので **XTAL_DIS = 1**（データシート §8.3.5「使わないときは止める」）
 *   - グローバルチョップ有効（オフセットドリフト除去）
 *
 * 【SPI】データシート §7.6
 *   - **モード1（CPOL=0 / CPHA=1）**。「CS の遷移は SCLK が Low の間に行うこと」
 *   - SCLK 上限 25 MHz（DVDD 2.7〜3.6V）。本機は SD と同一バスのため 8 MHz を使う
 *   - 語長 24bit（WLENGTH=01b・既定）。1フレーム = STATUS 1語 + データ6語 + CRC 1語 = **8語 24バイト**
 *   - **コマンドの応答は「次のフレーム」に出る。** RREG は 2フレームかかる（本ドライバが吸収する）
 *
 * 【CRC】データシート §8.3.12
 *   - CCITT（x^16+x^12+x^5+1 = 0x1021）、シード 0xFFFF
 *   - 対象は「CRC語を除くフレーム内の全語」＝ 先頭21バイト
 *   - 出力CRCは常時付与され無効化できない。入力CRCは RX_CRC_EN で任意
 *
 * 【要件との対応】要件定義 §7.1
 *   F-1 フレーム単位の読み書き / 語長設定           → readFrame(), Config::wordLength
 *   F-2 入出力CRC・レジスタマップCRC / エラー計数    → Stats::crcErrors ほか
 *   F-3 PWR・OSR・CHごとPGA・グローバルチョップ等   → Config
 *   F-4 DRDY割込みコンテキストで完結                → readFrame() は ISR から呼べる（8MHzで約24µs）
 *   F-5 サンプル連番と欠測検出                      → Frame::seq, Stats::dropped
 *   F-6 起動時レジスタリードバック照合              → verifyRegisters()
 *   F-7 24bit 2の補数 → int32_t 変換                 → readFrame() 内で符号拡張
 *   F-8 **ADCのOCAL/GCALは使わない**（生コード保存） → 本ドライバは校正レジスタを書かない
 */

#pragma once

#include <Arduino.h>
#include <SPI.h>

class ADS131M06 {
public:
  static constexpr uint8_t  NUM_CH      = 6;
  static constexpr uint8_t  FRAME_WORDS = 8;   // STATUS + 6CH + CRC
  static constexpr uint8_t  WORD_BYTES  = 3;   // WLENGTH = 24bit
  static constexpr uint8_t  FRAME_BYTES = FRAME_WORDS * WORD_BYTES;  // 24

  // ── レジスタアドレス（データシート 表8-14）──
  enum Reg : uint8_t {
    REG_ID          = 0x00,  // 読み出し専用。上位バイトは 0x26
    REG_STATUS      = 0x01,
    REG_MODE        = 0x02,
    REG_CLOCK       = 0x03,
    REG_GAIN1       = 0x04,  // CH0〜CH3
    REG_GAIN2       = 0x05,  // CH4〜CH5
    REG_CFG         = 0x06,  // グローバルチョップ・電流検出
    REG_THRSHLD_MSB = 0x07,
    REG_THRSHLD_LSB = 0x08,
    REG_CH0_CFG     = 0x09,  // 以降 CH あたり 5 レジスタ（CFG/OCAL_MSB/OCAL_LSB/GCAL_MSB/GCAL_LSB）
    REG_REGMAP_CRC  = 0x3E,
  };
  static constexpr uint8_t CH_REG_STRIDE = 5;

  // ── コマンド（データシート 表8-11）──
  enum Cmd : uint16_t {
    CMD_NULL    = 0x0000,
    CMD_RESET   = 0x0011,  // 応答 0xFF26
    CMD_STANDBY = 0x0022,
    CMD_WAKEUP  = 0x0033,
    CMD_LOCK    = 0x0555,
    CMD_UNLOCK  = 0x0655,
  };
  static constexpr uint16_t RESET_ACK = 0xFF26;

  // ── CLOCK.OSR[2:0]（fDATA = fCLKIN / (2 × OSR)）──
  enum Osr : uint8_t {
    OSR_128 = 0, OSR_256, OSR_512, OSR_1024 /*既定*/, OSR_2048,
    OSR_4096, OSR_8192, OSR_16256,
  };
  // ── CLOCK.PWR[1:0] ──
  enum Pwr : uint8_t { PWR_VLP = 0, PWR_LP = 1, PWR_HR = 2 /*既定・本機採用*/ };
  // ── GAINn.PGAGAINx[2:0] ──
  enum Gain : uint8_t {
    GAIN_1 = 0, GAIN_2, GAIN_4, GAIN_8, GAIN_16,
    GAIN_32 /*本機採用*/, GAIN_64, GAIN_128,
  };
  // ── CHx_CFG.MUXx[1:0]。自己診断に使う ──
  enum Mux : uint8_t {
    MUX_AIN    = 0,  // 通常（AINxP / AINxN）
    MUX_SHORT  = 1,  // 入力短絡。オフセット測定用
    MUX_DC_POS = 2,  // 正のDCテスト信号
    MUX_DC_NEG = 3,  // 負のDCテスト信号
  };

  // ── STATUS レジスタのビット ──
  static constexpr uint16_t ST_LOCK      = 1u << 15;
  static constexpr uint16_t ST_F_RESYNC  = 1u << 14;
  static constexpr uint16_t ST_REG_MAP   = 1u << 13;
  static constexpr uint16_t ST_CRC_ERR   = 1u << 12;
  static constexpr uint16_t ST_CRC_TYPE  = 1u << 11;
  static constexpr uint16_t ST_RESET     = 1u << 10;
  static constexpr uint16_t ST_DRDY_MASK = 0x003F;  // DRDY0〜DRDY5

  struct Config {
    // ★本機採用 OSR_1024。グローバルチョップ有効で 1295.34 SPS（要件の 1 kSPS を満たす）。
    //   OSR_4096 にすると GC 有効時は 325 SPS まで落ち、1 kSPS 要件を満たさない。
    //   ノイズは gain32 で OSR4096:0.77 / OSR2048:1.00 / OSR1024:1.20 µVrms（表7-1）、
    //   GC 有効ならいずれも √2 改善する。OSR1024+GC = 0.85 µVrms。
    uint8_t osr        = OSR_1024;
    uint8_t pwr        = PWR_HR;
    uint8_t gain[NUM_CH] = { GAIN_32, GAIN_32, GAIN_32, GAIN_32, GAIN_32, GAIN_32 };
    uint8_t mux[NUM_CH]  = { MUX_AIN, MUX_AIN, MUX_AIN, MUX_AIN, MUX_AIN, MUX_AIN };
    uint8_t chEnMask   = 0x3F;   // CH0〜CH5 すべて有効
    bool    globalChop = true;   // オフセットドリフト除去（要件 §7.2）
    uint8_t gcDelay    = 0x03;   // CFG.GC_DLY[3:0]。既定 0011b
    bool    extRef     = true;   // ★REFIN = VEX/2 を使う。false にすると内蔵1.2Vになる
    bool    disableXtal= true;   // ★CLKIN 外部供給のため内蔵発振器を止める
    // 入力CRC（F-2）。**既定 true。**フレーム構成はデータシートで確認済み
    //   （通常コマンドは語1、単一WREGは語2。transferFrame() のコメント参照）。
    //
    //   ★初期化に失敗した場合、begin() が自動でCRCを切って一度だけ再試行する。
    //     それで通れば crcFallback() が true になる。**原因が入力CRCだと特定できる。**
    //     起動ログに大きく出るので、現場では見逃さない。
    bool    rxCrcEn    = true;
    bool    regCrcEn   = true;   // レジスタマップCRC（F-2）
    bool    drdyFmt    = false;  // false = レベル出力（Low保持）。true = 負パルス
  };

  struct Frame {
    uint16_t status = 0;
    int32_t  ch[NUM_CH] = {0};   // 24bit 2の補数を符号拡張した値（F-7）
    uint16_t crcRx = 0;          // 受信したCRC語
    uint16_t crcCalc = 0;        // 計算したCRC
    bool     crcOk = false;
    uint32_t seq = 0;            // サンプル連番（F-5）
    // ★このサンプルの「直前」に取りこぼした数。呼び出し側はこのぶん
    //   リングの番号を進めること（SampleRing::pushGap）。進めないと番号が時刻を表さなくなる。
    uint32_t gapBefore = 0;
  };

  struct Stats {
    uint32_t frames    = 0;  // 読んだフレーム総数（欠測率の母数・M-7/M-8）
    uint32_t crcErrors = 0;  // 出力CRC不一致（F-2）
    uint32_t dropped   = 0;  // 取りこぼしたサンプル数の推定（F-5）。★読み出し間隔から求める
    uint32_t gaps      = 0;  // 取りこぼしが起きた「回数」（連続欠測は1回と数える）
    uint32_t fifoClears= 0;  // 中断復帰時にFIFOを吐き出させた回数（§8.5.1.9.1）
    uint32_t statusErr = 0;  // STATUS に CRC_ERR / REG_MAP が立った回数
    uint32_t resyncs   = 0;  // F_RESYNC 検出回数
  };

  ADS131M06() = default;

  /**
   * 初期化。**呼ぶ前に CLKIN が発振しており、SYNC/RESET が High であること。**
   * ADS131M06 は有効な CLKIN が無いと POR が完了しない（要件 F-26）。
   *
   * @param spi     SD カードと共有する SPI インスタンス
   * @param csPin   CS_ADC（XIAO D0）
   * @param drdyPin DRDY（XIAO D1）。入力プルアップなしで設定される
   * @param spiHz   SCLK 周波数。本機は 8 MHz
   * @return レジスタ照合まで通れば true
   */
  bool begin(SPIClass& spi, uint8_t csPin, uint8_t drdyPin, const Config& cfg,
             uint32_t spiHz = 8000000UL);

  /** RESET コマンドを送る。応答 0xFF26 を確認する。tREGACQ 待ちも含む */
  bool resetByCommand();

  /** Config の内容をレジスタへ書く（校正レジスタ OCAL/GCAL には触れない＝F-8） */
  bool applyConfig(const Config& cfg);

  /** 書いた設定をリードバックして照合する（F-6）。不一致のレジスタ名を Serial へ出す */
  bool verifyRegisters(const Config& cfg, Stream* log = nullptr);

  /**
   * 1フレーム読む（NULL コマンドを送りつつ STATUS + 6CH + CRC を受け取る）。
   * **DRDY 立下り割込みから直接呼べる。** 8 MHz・24バイトで約 24 µs。
   * CRC 不一致でも Frame は埋め、crcOk=false を返す（捨てるかどうかは呼び出し側の判断）。
   */
  bool readFrame(Frame& out);

  /** 1サンプルの想定周期 [µs]（設定から算出）。0 なら未初期化 */
  uint32_t samplePeriodUs() const { return samplePeriodUs_; }

  /**
   * 中断が起きたことを外から知らせる（SD書込みなどで読み出しを飛ばしたとき）。
   * 次の readFrame() の先頭でFIFOを吐き出させる。**呼ばないとDRDYの挙動が不定になる。**
   */
  void notifyPaused() { pendingFifoClear_ = true; haveLastRead_ = false; }

  /**
   * 入力CRCを切って初期化し直したか。
   * **true なら F-2（入力CRC）を満たしていない状態で動いている。**
   * 起動ログへ必ず出すこと。原因は配線でも CLKIN でもなく入力CRCの形式である。
   */
  bool crcFallback() const { return crcFallback_; }

  /** レジスタ1本読む。**連続変換中に呼ぶとそのフレームのデータを失う** */
  bool readReg(uint8_t addr, uint16_t& value);
  /** レジスタ1本書く */
  bool writeReg(uint8_t addr, uint16_t value);

  /** STANDBY / WAKEUP */
  bool standby();
  bool wakeup();

  const Stats& stats() const { return stats_; }
  void resetStats() { stats_ = Stats(); }

  /** DRDY が Low（データあり）か。ポーリング用だが常用しないこと（要件 §4.2） */
  bool dataReady() const { return digitalRead(drdyPin_) == LOW; }

  /**
   * 1 LSB あたりの入力換算電圧 [V]。
   *
   * **FSR = ±0.96 × VREF / Gain**（要件定義 §3.3 の表。SBAS949A）。
   * したがって LSB = (2 × 0.96 × VREF / Gain) / 2^24。
   * 外部リファレンス VREF = 1.25 V では **2.4/Gain**、ゲイン32 で **4.470 nV**、
   * FSR は **±37.5 mV**（変位換算 ±15.8 mm）。
   *
   * ★係数 0.96 を落として `2 × VREF/Gain` としてはいけない。**4.17% のゲイン誤差**になる。
   *   2026-09-12 に一度この誤りを入れ、外部レビューで指摘されて戻している。
   *   FSR が内蔵基準（1.2 V）のときと同じ ±1.2 V/Gain になるのは偶然ではなく、
   *   0.96 × 1.25 = 1.2 だからである。
   *
   * ★最終確認は実機で行うこと。既知電圧を入力し、読み値との比が 1.000 になることを見る。
   */
  static constexpr double FSR_COEFF = 0.96;   // FSR = ±FSR_COEFF × VREF / Gain
  static double lsbVolts(uint8_t gainCode, double vref = 1.25) {
    return (2.0 * FSR_COEFF * vref / (double)(1u << gainCode)) / 16777216.0;
  }

  /**
   * 設定から実データレート [SPS] を計算する。**この値を手で書き写さないこと。**
   *
   *   グローバルチョップ無効: fDATA = fMOD / OSR          （fMOD = fCLKIN / 2）
   *   グローバルチョップ有効: fDATA = 1 / (tGC_DLY + 3 × OSR × tMOD)   （SBAS949A 式8）
   *
   * ★GC 有効時に `fMOD/OSR` を使うと約3倍ずれる。リング長・1秒ブロック・プリ/ポスト長が
   *   全部この値に乗っているので、ずれると全部が静かに狂う。
   */
  static double dataRateSps(const Config& cfg, double fClkInHz = 8000000.0) {
    static const uint16_t OSR_TABLE[8] = {128, 256, 512, 1024, 2048, 4096, 8192, 16256};
    const double fMod = fClkInHz / 2.0;
    const double osr  = OSR_TABLE[cfg.osr & 0x07];
    if (!cfg.globalChop) return fMod / osr;
    const double gcDlyMod = (double)(2u << (cfg.gcDelay & 0x0F));  // 0000b=2, 0011b=16 …
    return fMod / (gcDlyMod + 3.0 * osr);
  }

  /** CCITT CRC-16（多項式 0x1021 / シード 0xFFFF）。データシート 表8-7 */
  static uint16_t crc16(const uint8_t* data, size_t len, uint16_t seed = 0xFFFF);

private:
  SPIClass* spi_ = nullptr;
  SPISettings settings_;
  uint8_t  csPin_   = 0xFF;
  uint8_t  drdyPin_ = 0xFF;
  bool     rxCrcEn_ = false;
  bool     crcFallback_ = false;   // 入力CRCを切って初期化し直したか（診断用）
  uint32_t seq_     = 0;
  uint16_t lastDrdyBits_ = 0;
  // ★取りこぼし検出の時間基準。begin() で dataRateSps() から求める。
  //   STATUS の DRDY ビットでは検出できないため（§8.5.1.9.1）、間隔で見るしかない。
  uint32_t samplePeriodUs_ = 0;
  uint32_t lastReadUs_     = 0;
  bool     haveLastRead_   = false;
  bool     pendingFifoClear_ = false;  // 次の読み出し前にFIFOを吐き出させるか
  Stats    stats_;

  /** 1フレーム送受信する。txWord0 が先頭語（コマンド）。rx が null なら読み捨て */
  /**
   * 1フレーム送受信する。
   *
   * 【入力フレームの構成】（SBAS949A §8.5.1.7 / §8.5.1.10.8）
   *   通常コマンド : [語0]コマンド [語1]入力CRC [語2〜7]ゼロ
   *   WREG(n本)    : [語0]コマンド [語1..n]書く値 [語n+1]入力CRC [以降]ゼロ
   *
   *   入力CRC は**書いた語の直後**に置き、**それ以前の語だけ**を対象に計算する。
   *   ★フレーム末尾（語7）ではない。2026-09-12 の実装は語7に置き、先頭21バイトを
   *     対象にしていた。RX_CRC_EN を立てると RREG 等が一切実行されなくなる誤りで、
   *     しかも WREG だけは誤CRCでも書き込まれるため（データシート明記）
   *     「書けているのに読めない」という切り分けの難しい壊れ方をする。
   *
   * @param nData 語1以降に載せるデータ語数。通常コマンドは 0、単一WREG は 1。
   */
  void transferFrame(uint16_t txWord0, const uint16_t* txData, uint8_t nData, uint8_t* rx);

  /**
   * MODE レジスタを書く。**入力CRCの有効/無効が切り替わる特別なフレーム。**
   * WREG フレーム自体は「切替前」の設定で送り、応答を読む次のフレームからは
   * 「切替後」の設定で送る必要がある（レジスタは DIN へシフトされた時点で書かれるため）。
   */
  bool writeModeWithCrcSwitch(uint16_t mode, bool rxCrcEnAfter);

  /** 1回分の初期化本体。begin() から入力CRC有り/無しで最大2回呼ばれる */
  bool beginOnce(const Config& cfg);
  /** コマンドを送り、次フレームの先頭語を応答として取り出す */
  uint16_t command(uint16_t cmd);

  static inline void put24(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
  }
  static inline uint32_t get24(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
  }
};
