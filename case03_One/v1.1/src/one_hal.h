#pragma once

#include <Arduino.h>
#include <Wire.h>

#include <stddef.h>
#include <stdint.h>

#include "one_status.h"

// ── Monita One v1.1 ピン割当（差し替えはこのブロックだけで完結させる）──
//
// D3〜D10 は v1.00 と同一（ネットリスト解析＋実機検証 2026-08-26 で確定済み）。
// D0〜D2 は v1.1 で追加（ver1.1.sch 2026-09-15 22:48 エクスポートのネットリストで確認。実機未検証）。
//   詳細: 【7】Monita/01_開発/MonitaOne基板/Monita_One_v1.00_to_v1.1_diff.md
constexpr uint8_t ONE_UART_TX_PIN = D8;
constexpr uint8_t ONE_UART_RX_PIN = D9;
constexpr uint8_t ONE_MOSFET_GATE_PIN = D10;  // 3V3_SW電源ゲート。★LOW=ON（Flexと逆極性）
constexpr uint8_t ONE_LORA_MODE_PIN = D7;     // E220 M0/M1共通（基板上で短絡）
constexpr uint8_t ONE_PD_SCK_PIN = D3;        // HX711 PD_SCK / DS18B20 1-Wire兼用
constexpr uint8_t ONE_DOUT_PIN = D4;          // HX711 DOUT / PIRモードでは PIR OUT兼用
constexpr uint8_t ONE_I2C_SDA_PIN = D5;       // ★XIAO既定(D4)と異なる。専用SensorWireを使う
constexpr uint8_t ONE_I2C_SCL_PIN = D6;       // ★XIAO既定(D5)と異なる
constexpr uint8_t ONE_ST_CHRG_PIN = D0;       // v1.1: 充電モジュール CHRG（ダイオード分離、LOW=充電中）
constexpr uint8_t ONE_ST_DONE_PIN = D1;       // v1.1: 充電モジュール DONE（ダイオード分離、LOW=満充電）
constexpr uint8_t ONE_LORA_AUX_PIN = D2;      // v1.1: E220 AUX（LOW=処理中、HIGH=待機）。Sigfox実装機では未接続

namespace one {

// XIAO既定Wire(D4/D5)とは別の、Oneセンサ端子専用TwoWire。
extern TwoWire SensorWire;

void halBegin();
void setPeripheralPower(bool on);
bool peripheralPowerIsOn();
void beginSensorI2c();
void endSensorI2c();

void beginRadioUart();
void endRadioUart();
void setLoRaModeNormal();  // M0=M1=LOW
void setLoRaModeSleep();   // M0=M1=HIGH (Mode 3 / config)
bool waitLoRaIdle(uint32_t timeoutMs);  // AUX=HIGHを待つ。タイムアウトでfalse
bool checkAndConfigureLoRa();
void sendLoRaFrame(const uint8_t *payload, uint8_t length);
bool receiveLoRaFrame(uint8_t *payload, uint8_t capacity, uint8_t &length,
                      uint32_t timeoutMs);
bool sendSigfoxPayload(const uint8_t *payload, size_t length);

uint16_t readBatteryMv();
int16_t readCpuTemperatureDeciC();
ChargeState readChargeState();

uint32_t crc32(const void *data, size_t length);
bool readFile(const char *path, void *data, size_t length);
bool writeFile(const char *path, const void *data, size_t length);

void watchdogBegin(uint32_t timeoutMs);
void watchdogFeed();

}  // namespace one
