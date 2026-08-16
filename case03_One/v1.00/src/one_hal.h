#pragma once

#include <Arduino.h>
#include <Wire.h>

#include <stddef.h>
#include <stdint.h>

// ── Monita One v1.00 ピン割当（差し替えはこのブロックだけで完結させる）──
constexpr uint8_t ONE_UART_TX_PIN = D8;
constexpr uint8_t ONE_UART_RX_PIN = D9;
// ★以下6本は回路図未確認の推定値。実機配線確認後にここだけを更新すること。
constexpr uint8_t ONE_MOSFET_GATE_PIN = D10;  // 要回路図確認
constexpr uint8_t ONE_LORA_MODE_PIN = D7;     // 要回路図確認: M0/M1共通
constexpr uint8_t ONE_PD_SCK_PIN = D3;        // 要回路図確認
constexpr uint8_t ONE_DOUT_PIN = D4;          // 要回路図確認 / PIR OUT兼用
constexpr uint8_t ONE_I2C_SDA_PIN = D5;       // 要回路図確認
constexpr uint8_t ONE_I2C_SCL_PIN = D6;       // 要回路図確認

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

