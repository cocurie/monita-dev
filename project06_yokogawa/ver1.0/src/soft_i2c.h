/**
 * ソフトウェア（ビットバング）I2C
 *
 * ESP32-C3のハードウェアI2Cペリフェラルはクロックストレッチング用
 * タイムアウトカウンタが5bitしかなく、クロックストレッチングを行う
 * デバイス（MCP9600など）と相性が悪い（ハードウェア制約、Wire.setTimeOut()
 * やWire.setClock()では回避不可）。
 *
 * このモジュールはGPIOを直接叩いてI2Cプロトコルを実装するため、
 * クロックストレッチングの待ち時間をソフトウェア側で自由に設定できる。
 *
 * 同じSDA/SCLピンをハードウェアWire（MCP23008等）と共用する場合は、
 * ビットバング通信の前後で Wire.end() / Wire.begin() を呼んで
 * ペリフェラルとの競合を避けること。
 */
#pragma once
#include <Arduino.h>

void softI2CInit(uint8_t sdaPin, uint8_t sclPin);

// アドレスに応答があるか（I2Cスキャン用）。true=ACKあり
bool softI2CProbe(uint8_t addr7);

// register 1バイト書き込み
bool softI2CWriteReg(uint8_t addr7, uint8_t reg, uint8_t val);

// レジスタポインタを書いてからリピーテッドスタートでlenバイト読む（MSBFIRST格納）
bool softI2CReadReg(uint8_t addr7, uint8_t reg, uint8_t *buf, uint8_t len);
