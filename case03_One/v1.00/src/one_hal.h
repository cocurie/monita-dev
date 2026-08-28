#pragma once

#include <Arduino.h>
#include <Wire.h>

#include <stddef.h>
#include <stdint.h>

// ── Monita One v1.00 ピン割当（差し替えはこのブロックだけで完結させる）──
//
// 全ピン確定済み。根拠は2段階:
//  1) ネットリスト解析: Flex v3.20 と同一の EAGLE 部品（XIAOフットプリント）を使用しており、
//     稼働中のFlexファームのピン定義と5〜7点で矛盾なく一致した。
//  2) 実機検証(2026-08-26): MOSFET_GATE / PD_SCK / DOUT の動作を基板上で確認済み。
//     詳細: 【7】Monita/開発/開発メモ/20260826_MonitaOne_v1.00_実機立ち上げ記録.md
constexpr uint8_t ONE_UART_TX_PIN = D8;
constexpr uint8_t ONE_UART_RX_PIN = D9;
constexpr uint8_t ONE_MOSFET_GATE_PIN = D10;  // 3V3_SW電源ゲート。★LOW=ON（Flexと逆極性）
constexpr uint8_t ONE_LORA_MODE_PIN = D7;     // E220 M0/M1共通（基板上で短絡）
constexpr uint8_t ONE_PD_SCK_PIN = D3;        // HX711 PD_SCK / DS18B20 1-Wire兼用
constexpr uint8_t ONE_DOUT_PIN = D4;          // HX711 DOUT / PIRモードでは PIR OUT兼用
constexpr uint8_t ONE_I2C_SDA_PIN = D5;       // ★XIAO既定(D4)と異なる。専用SensorWireを使う
constexpr uint8_t ONE_I2C_SCL_PIN = D6;       // ★XIAO既定(D5)と異なる

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
bool checkAndConfigureLoRa();
void sendLoRaFrame(const uint8_t *payload, uint8_t length);
bool receiveLoRaFrame(uint8_t *payload, uint8_t capacity, uint8_t &length,
                      uint32_t timeoutMs);
bool sendSigfoxPayload(const uint8_t *payload, size_t length);

uint16_t readBatteryMv();
int16_t readCpuTemperatureDeciC();

uint32_t crc32(const void *data, size_t length);
bool readFile(const char *path, void *data, size_t length);
bool writeFile(const char *path, const void *data, size_t length);

void watchdogBegin(uint32_t timeoutMs);
void watchdogFeed();

}  // namespace one

