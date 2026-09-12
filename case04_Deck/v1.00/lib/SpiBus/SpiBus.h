/**
 * SpiBus — SPI バスの所有権調停（要件 F-28）
 *
 * ═══════════════════════════════════════════════════════════════════
 * ★ここを読まずに SD カードや ADC のコードを書かないこと。
 * ═══════════════════════════════════════════════════════════════════
 *
 * 【何が問題か】
 *   ADS131M06（モード1）と microSD（モード0）が **同じ SPI バス**（D8/D9/D10）に
 *   ぶら下がっている。CS だけが別（D0 / D3）である。
 *   ADC は DRDY 割込みで 0.772 ms ごとに読まれ、SD 書込みは main ループから走る。
 *
 *   **`SPI.beginTransaction()` は排他ロックではない。**
 *   SD の転送中に DRDY 割込みが入って CS_ADC を落とすと、CS が2本とも Low になり
 *   MISO が衝突して**両方の転送が壊れる**。しかも壊れ方が不定で、再現も難しい。
 *
 * 【この仕組み】
 *   割込みは main を追い越せるが、main は割込みを追い越せない。
 *   したがって「main 側が握っているか」を 1 ビット持ち、**ISR はそれを見て
 *   握られていたら即座に戻る**だけでよい。ISR 側は待たない（待てない）。
 *
 *     main 側 : spibus::Lock lk;   // ← これだけ。スコープを抜けると自動で離す
 *     ISR 側  : if (spibus::busyFromIsr()) { 欠測として処理; return; }
 *
 * 【SD を書く人への注意】
 *   ★**1ブロック（512 B）ごとに握り直すこと。**イベント波形は 117 kB あるが、
 *     これを一度に握ると 0.3〜0.5 秒バスを占有し、**その間のサンプルが全部欠測**になる。
 *     ブロックの合間に Lock を抜ければ、待っていた DRDY が処理される。
 *   ★**SD の内部ビジー中は握らないこと。**ブロックを送り終えたら CS_SD を上げて
 *     Lock を離し、ビジー確認のたびに握り直す。ビジーは数 ms〜数百 ms ありうる。
 *   ★握った区間で 1 サンプル（0.772 ms）を超えると必ず欠測が出る。
 *     `spibus::maxHoldUs()` を見て、想定より長く握っていないか確認すること。
 */

#pragma once

#include <Arduino.h>

namespace spibus {

/** ISR から呼ぶ。main 側がバスを握っていれば true（＝ADC を読んではいけない） */
bool busyFromIsr();

/** main 側から握る。**直接呼ばず Lock を使うこと**（離し忘れるとADCが永久に止まる） */
void claim();

/** main 側から離す */
void release();

/**
 * スコープを抜けると自動で離す。**握りっぱなしの事故を防ぐため必ずこれを使う。**
 *
 *   {
 *     spibus::Lock lk;
 *     digitalWrite(PIN_CS_SD, LOW);
 *     ... 512バイト転送 ...
 *     digitalWrite(PIN_CS_SD, HIGH);
 *   }   // ← ここで自動的に離れ、待っていた DRDY が処理される
 */
struct Lock {
  Lock()  { claim(); }
  ~Lock() { release(); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

/** 握った回数 */
uint32_t claims();
/** 1回の保持時間の最大値 [µs]。★1サンプル周期(772µs)を超えていたら欠測が出ている */
uint32_t maxHoldUs();
/** 統計を戻す */
void resetStats();

}  // namespace spibus
