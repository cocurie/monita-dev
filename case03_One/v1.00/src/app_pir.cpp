/*
 * Monita One v1.00 PIR版
 * System ON + FreeRTOS Tickless Idle。Arduino loopタスクそのものを単一状態機械として使う。
 */
#include <Arduino.h>
#include <bluefruit.h>
#include <nrf_soc.h>

#include "one_hal.h"
#include "one_payload.h"

#if !defined(COMM_MODE_LORA) && !defined(COMM_MODE_SIGFOX)
#error "COMM_MODE_LORAまたはCOMM_MODE_SIGFOXを指定してください"
#endif

#ifndef FW_VERSION
#define FW_VERSION 0x01
#endif
#ifndef DEVICE_ID
#define DEVICE_ID 0x0F
#endif
// DEVICE_IDは 上位3bit=Gateway群(0〜7) / 下位5bit=群内の機器番号(1〜31)。
// 下位5bitが0のIDはGateway側で無効値として捨てられるため、ビルド時に弾く。
static_assert((DEVICE_ID) >= 0x01 && (DEVICE_ID) <= 0xFF,
              "DEVICE_ID must be 0x01..0xFF");
static_assert(((DEVICE_ID) & 0x1F) != 0,
              "DEVICE_ID low 5 bits must be 1..31 (0 is reserved)");

namespace {

constexpr uint8_t SETTINGS_VERSION = 1;
constexpr uint8_t RETENTION_VERSION = 1;
constexpr char SETTINGS_FILE[] = "/one_pir_settings_v1.bin";
constexpr char RETENTION_FILE[] = "/one_pir_runtime_v1.bin";
constexpr uint8_t MAX_DEVICES = 64;
constexpr uint16_t SCAN_INTERVAL_MS = 150;
constexpr uint16_t SCAN_WINDOW_MS = 100;
constexpr uint32_t ROLLING_WINDOW_MS = 3600000UL;
constexpr uint32_t MAX_ACTIVITY_MS = 180000UL;
constexpr uint32_t DOWNLINK_WINDOW_MS = 2000UL;
constexpr uint32_t PIR_STUCK_MS = 10000UL;

enum class State : uint8_t { WAIT, SCAN, REPORT_SNAPSHOT, TX, RX_WINDOW };

struct __attribute__((packed)) PirSettings {
  uint8_t version;
  uint16_t reportIntervalMin;
  int8_t rssiThreshold;
  uint8_t minHits;
  uint8_t rssiMergeGap;
  uint8_t calibrationTenths;
  uint16_t pirHoldoffSec;
  uint8_t maxScansPerHour;
  uint8_t scanDurationSec;
  uint32_t crc;
};

struct Aggregate {
  uint32_t maxPeople;
  uint32_t sumPeople;
  uint32_t scanCount;
  uint32_t rawWakeCount;
  uint32_t pirEventCount;
  uint32_t holdoffSuppressed;
  uint32_t rateSuppressed;
  uint32_t droppedDeviceCount;
};

struct __attribute__((packed)) RuntimeRetention {
  uint8_t version;
  uint32_t remainingReportMs;
  uint32_t remainingHoldoffMs;
  uint32_t remainingRollingMs;
  uint8_t scansInRollingWindow;
  Aggregate active;
  uint32_t crc;
};

struct Device {
  uint8_t mac[6];
  uint16_t count;
  int32_t rssiSum;
};

PirSettings s_settings{};
Aggregate s_buffers[2]{};
Aggregate *s_active = &s_buffers[0];
Aggregate *s_snapshot = &s_buffers[1];
Device s_devices[MAX_DEVICES]{};
volatile uint32_t s_droppedThisScan = 0;
uint8_t s_deviceCount = 0;
TaskHandle_t s_mainTask = nullptr;
State s_state = State::WAIT;
uint32_t s_nextReportAt = 0;
uint32_t s_holdoffUntil = 0;
uint32_t s_rollingWindowAt = 0;
uint8_t s_scansInRollingWindow = 0;
bool s_pirArmed = false;
bool s_pirQuarantined = false;
uint32_t s_pirHighSince = 0;
bool s_resetAfterAck = false;
uint32_t s_pirNrfPin = 0;
constexpr uint8_t PIR_PPI_CHANNEL = 15;  // SoftDeviceでアプリ利用可能なPPI 8..19から専用確保

bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

uint32_t remainingMs(uint32_t now, uint32_t deadline) {
  return deadlineReached(now, deadline) ? 0U : deadline - now;
}

void defaults() {
  memset(&s_settings, 0, sizeof(s_settings));
  s_settings.version = SETTINGS_VERSION;
  s_settings.reportIntervalMin = 60;
  s_settings.rssiThreshold = -65;
  s_settings.minHits = 10;
  s_settings.rssiMergeGap = 3;
  s_settings.calibrationTenths = 10;
  s_settings.pirHoldoffSec = 120;
  s_settings.maxScansPerHour = 6;
  s_settings.scanDurationSec = 30;
}

uint32_t settingsCrc(const PirSettings &v) {
  return one::crc32(&v, offsetof(PirSettings, crc));
}

bool settingsValid(const PirSettings &v) {
  return v.version == SETTINGS_VERSION && v.reportIntervalMin >= 5 &&
      v.reportIntervalMin <= 1440 && v.rssiThreshold >= -100 && v.rssiThreshold <= -40 &&
      v.minHits >= 1 && v.minHits <= 50 && v.rssiMergeGap <= 20 &&
      v.calibrationTenths >= 1 && v.pirHoldoffSec <= 3600 &&
      v.maxScansPerHour <= 60 && v.scanDurationSec >= 5 && v.scanDurationSec <= 120 &&
      v.crc == settingsCrc(v);
}

#ifdef COMM_MODE_LORA
void saveSettings() {
  s_settings.crc = settingsCrc(s_settings);
  one::writeFile(SETTINGS_FILE, &s_settings, sizeof(s_settings));
}
#endif

void loadSettings() {
  PirSettings stored{};
  if (one::readFile(SETTINGS_FILE, &stored, sizeof(stored)) && settingsValid(stored)) {
    s_settings = stored;
    Serial.println("[SETTINGS] PIR設定をCRC確認後に復元");
  } else {
    defaults();
    Serial.println("[SETTINGS] PIR初期値を使用");
  }
}

uint32_t retentionCrc(const RuntimeRetention &v) {
  return one::crc32(&v, offsetof(RuntimeRetention, crc));
}

void saveRuntimeBeforeReset() {
  const uint32_t now = millis();
  RuntimeRetention record{};
  record.version = RETENTION_VERSION;
  record.remainingReportMs = remainingMs(now, s_nextReportAt);
  record.remainingHoldoffMs = remainingMs(now, s_holdoffUntil);
  record.remainingRollingMs = remainingMs(now, s_rollingWindowAt + ROLLING_WINDOW_MS);
  record.scansInRollingWindow = s_scansInRollingWindow;
  record.active = *s_active;
  record.crc = retentionCrc(record);
  one::writeFile(RETENTION_FILE, &record, sizeof(record));
}

bool restoreRuntime() {
  RuntimeRetention record{};
  if (!one::readFile(RETENTION_FILE, &record, sizeof(record)) ||
      record.version != RETENTION_VERSION || record.crc != retentionCrc(record)) return false;
  const uint32_t now = millis();
  *s_active = record.active;
  s_nextReportAt = now + record.remainingReportMs;
  s_holdoffUntil = now + record.remainingHoldoffMs;
  const uint32_t elapsed = record.remainingRollingMs >= ROLLING_WINDOW_MS
      ? 0U : ROLLING_WINDOW_MS - record.remainingRollingMs;
  s_rollingWindowAt = now - elapsed;
  s_scansInRollingWindow = record.scansInRollingWindow;
  // 復元レコードは一度だけ使う。再度resetした場合は直前の状態で上書きされる。
  record.version = 0;
  record.crc = retentionCrc(record);
  one::writeFile(RETENTION_FILE, &record, sizeof(record));
  return true;
}

int findDevice(const uint8_t *mac) {
  for (uint8_t i = 0; i < s_deviceCount; ++i)
    if (memcmp(s_devices[i].mac, mac, 6) == 0) return i;
  return -1;
}

void updateDevice(const uint8_t *mac, int rssi) {
  if (rssi < s_settings.rssiThreshold) return;
  const int found = findDevice(mac);
  if (found >= 0) {
    if (s_devices[found].count != UINT16_MAX) ++s_devices[found].count;
    s_devices[found].rssiSum += rssi;
  } else if (s_deviceCount < MAX_DEVICES) {
    memcpy(s_devices[s_deviceCount].mac, mac, 6);
    s_devices[s_deviceCount].count = 1;
    s_devices[s_deviceCount].rssiSum = rssi;
    ++s_deviceCount;
  } else {
    ++s_droppedThisScan;
  }
}

int estimatePeople() {
  int rssiList[MAX_DEVICES];
  uint8_t count = 0;
  for (uint8_t i = 0; i < s_deviceCount; ++i)
    if (s_devices[i].count >= s_settings.minHits)
      rssiList[count++] = s_devices[i].rssiSum / s_devices[i].count;
  if (count == 0) return 0;
  for (uint8_t i = 0; i + 1 < count; ++i)
    for (uint8_t j = i + 1; j < count; ++j)
      if (rssiList[i] < rssiList[j]) { const int t = rssiList[i]; rssiList[i] = rssiList[j]; rssiList[j] = t; }
  bool used[MAX_DEVICES] = {};
  int clusters = 0;
  for (uint8_t i = 0; i < count; ++i) {
    if (used[i]) continue;
    ++clusters;
    used[i] = true;
    for (uint8_t j = i + 1; j < count; ++j)
      if (!used[j] && abs(rssiList[i] - rssiList[j]) <= s_settings.rssiMergeGap)
        used[j] = true;
  }
  // Toyono同様に補正後は切り捨て。設定値は0.1倍単位。
  return clusters * s_settings.calibrationTenths / 10;
}

void scanCallback(ble_gap_evt_adv_report_t *report) {
  updateDevice(report->peer_addr.addr, report->rssi);
  Bluefruit.Scanner.resume();
}

void clearDeviceData() {
  // プライバシー要件: raw MACはスキャン終了直後に揮発RAMから消去する。
  memset(s_devices, 0, sizeof(s_devices));
  s_deviceCount = 0;
}

void pirPortIsrBody() {
  // PORT/SENSE→PPI→EGU3経路。GPIOTE IN eventを一切割り当てない。
  NRF_EGU3->EVENTS_TRIGGERED[0] = 0;
  NRF_GPIOTE->EVENTS_PORT = 0;
  uint32_t pin = s_pirNrfPin;
  NRF_GPIO_Type *port = nrf_gpio_pin_port_decode(&pin);
  const uint32_t mask = 1UL << pin;
  const uint32_t latched = port->LATCH & mask;
  if (latched) port->LATCH = latched;
  BaseType_t higherPriorityTaskWoken = pdFALSE;
  if (s_mainTask != nullptr) vTaskNotifyGiveFromISR(s_mainTask, &higherPriorityTaskWoken);
  portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

void armPirSense() {
  if (digitalRead(ONE_DOUT_PIN) != LOW) return;
  uint32_t pin = s_pirNrfPin;
  NRF_GPIO_Type *port = nrf_gpio_pin_port_decode(&pin);
  port->LATCH = 1UL << pin;
  port->PIN_CNF[pin] = (port->PIN_CNF[pin] & ~GPIO_PIN_CNF_SENSE_Msk) |
      (GPIO_PIN_CNF_SENSE_High << GPIO_PIN_CNF_SENSE_Pos);
  s_pirArmed = true;
  s_pirQuarantined = false;
  s_pirHighSince = 0;
}

void disarmPirSense() {
  uint32_t pin = s_pirNrfPin;
  NRF_GPIO_Type *port = nrf_gpio_pin_port_decode(&pin);
  port->PIN_CNF[pin] = (port->PIN_CNF[pin] & ~GPIO_PIN_CNF_SENSE_Msk) |
      (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos);
  s_pirArmed = false;
}

bool initPirSense() {
  pinMode(ONE_DOUT_PIN, INPUT_PULLDOWN);
  s_pirNrfPin = g_ADigitalPinMap[ONE_DOUT_PIN];
  // AdafruitのGPIOTE INハンドラと競合させず、EVENTS_PORTをEGU3 IRQへPPI転送する。
  NRF_GPIOTE->EVENTS_PORT = 0;
  NRF_EGU3->EVENTS_TRIGGERED[0] = 0;
  NRF_EGU3->INTENSET = EGU_INTENSET_TRIGGERED0_Msk;
  NVIC_ClearPendingIRQ(SWI3_EGU3_IRQn);
  NVIC_SetPriority(SWI3_EGU3_IRQn, 7);
  NVIC_EnableIRQ(SWI3_EGU3_IRQn);
  if (sd_ppi_channel_assign(PIR_PPI_CHANNEL, &NRF_GPIOTE->EVENTS_PORT,
                            &NRF_EGU3->TASKS_TRIGGER[0]) != NRF_SUCCESS) return false;
  if (sd_ppi_channel_enable_set(1UL << PIR_PPI_CHANNEL) != NRF_SUCCESS) return false;
  if (digitalRead(ONE_DOUT_PIN) == LOW) armPirSense();
  else {
    // 起動時HIGHも有効な1イベントとしてタスクに渡し、LOW復帰までdisarm扱いにする。
    xTaskNotifyGive(s_mainTask);
    s_pirHighSince = millis();
  }
  return true;
}

int performScan() {
  s_state = State::SCAN;
  memset(s_devices, 0, sizeof(s_devices));
  s_deviceCount = 0;
  s_droppedThisScan = 0;
  Bluefruit.Scanner.start(0);
  const uint32_t durationMs = static_cast<uint32_t>(s_settings.scanDurationSec) * 1000UL;
  vTaskDelay(pdMS_TO_TICKS(durationMs));
  Bluefruit.Scanner.stop();
  const int people = estimatePeople();
  s_active->droppedDeviceCount += s_droppedThisScan;
  clearDeviceData();
  ++s_active->scanCount;
  s_active->sumPeople += people < 0 ? 0U : static_cast<uint32_t>(people);
  if (static_cast<uint32_t>(people) > s_active->maxPeople) s_active->maxPeople = people;
  ++s_scansInRollingWindow;
  s_holdoffUntil = millis() + static_cast<uint32_t>(s_settings.pirHoldoffSec) * 1000UL;
  s_state = State::WAIT;
  one::watchdogFeed();  // SCAN状態遷移が正常完了したチェックポイント
  return people;
}

void handlePirNotification(uint32_t notifications) {
  if (notifications == 0) return;
  s_active->rawWakeCount += notifications;
  disarmPirSense();
  const uint32_t now = millis();
  if (digitalRead(ONE_DOUT_PIN) == HIGH && s_pirHighSince == 0) s_pirHighSince = now;
  if (!deadlineReached(now, s_holdoffUntil)) {
    s_active->holdoffSuppressed += notifications;
    return;
  }
  s_active->pirEventCount += notifications;
  if (deadlineReached(now, s_rollingWindowAt + ROLLING_WINDOW_MS)) {
    s_rollingWindowAt = now;
    s_scansInRollingWindow = 0;
  }
  if (s_settings.maxScansPerHour != 0 &&
      s_scansInRollingWindow >= s_settings.maxScansPerHour) {
    s_active->rateSuppressed += notifications;
    return;
  }
  performScan();
}

void maintainPirRearm() {
  if (s_pirArmed) return;
  if (digitalRead(ONE_DOUT_PIN) == LOW) {
    armPirSense();
    return;
  }
  if (s_pirHighSince == 0) s_pirHighSince = millis();
  if (millis() - s_pirHighSince >= PIR_STUCK_MS) s_pirQuarantined = true;
  // 張り付き中はSENSEを再armしない。LOW復帰だけがquarantine解除条件。
}

void takeReportSnapshot() {
  s_state = State::REPORT_SNAPSHOT;
  taskENTER_CRITICAL();
  Aggregate *completed = s_active;
  s_active = s_snapshot;
  s_snapshot = completed;
  memset(s_active, 0, sizeof(*s_active));
  taskEXIT_CRITICAL();
  s_nextReportAt += static_cast<uint32_t>(s_settings.reportIntervalMin) * 60000UL;
  if (deadlineReached(millis(), s_nextReportAt))
    s_nextReportAt = millis() + static_cast<uint32_t>(s_settings.reportIntervalMin) * 60000UL;
}

#ifdef COMM_MODE_LORA
void sendDownlinkAck(uint8_t status) {
  const uint8_t ack[] = {0x05, DEVICE_ID, status,
      static_cast<uint8_t>(s_settings.reportIntervalMin >> 8),
      static_cast<uint8_t>(s_settings.reportIntervalMin),
      static_cast<uint8_t>(s_settings.rssiThreshold), s_settings.minHits,
      s_settings.rssiMergeGap, s_settings.calibrationTenths,
      static_cast<uint8_t>(s_settings.pirHoldoffSec >> 8),
      static_cast<uint8_t>(s_settings.pirHoldoffSec),
      s_settings.maxScansPerHour, s_settings.scanDurationSec, FW_VERSION};
  one::sendLoRaFrame(ack, sizeof(ack));
  delay(300);
}

bool applyPirDownlink(const uint8_t *p, uint8_t length) {
  // 15B: C0DE,81,ID,flags, report(2),rssi,minHits,gap,cal,holdoff(2),max,duration
  if (length != 15 || p[0] != 0xC0 || p[1] != 0xDE || p[2] != 0x81 || p[3] != DEVICE_ID)
    return false;
  const uint8_t flags = p[4];
  PirSettings candidate = s_settings;
  if (flags & (1U << 0)) candidate.reportIntervalMin = static_cast<uint16_t>(p[5]) << 8 | p[6];
  if (flags & (1U << 1)) candidate.rssiThreshold = static_cast<int8_t>(p[7]);
  if (flags & (1U << 2)) candidate.minHits = p[8];
  if (flags & (1U << 3)) candidate.rssiMergeGap = p[9];
  if (flags & (1U << 4)) candidate.calibrationTenths = p[10];
  if (flags & (1U << 5)) candidate.pirHoldoffSec = static_cast<uint16_t>(p[11]) << 8 | p[12];
  if (flags & (1U << 6)) candidate.maxScansPerHour = p[13];
  if (flags & (1U << 7)) candidate.scanDurationSec = p[14];
  candidate.crc = settingsCrc(candidate);
  if (!settingsValid(candidate)) {
    sendDownlinkAck(1);  // 値域エラー
    return true;
  }
  const bool changed = memcmp(&candidate, &s_settings, sizeof(candidate)) != 0;
  const bool intervalChanged = candidate.reportIntervalMin != s_settings.reportIntervalMin;
  s_settings = candidate;
  if (changed) saveSettings();
  sendDownlinkAck(0);
  if (changed) {
    // WDTを新設定で張り直すためreset。集計・期限・holdoff/rolling窓を先に保存する。
    s_resetAfterAck = intervalChanged || changed;
  }
  return true;
}
#endif

void transmitSnapshot() {
  s_state = State::TX;
  const one::PirPayloadInput input{s_snapshot->maxPeople, s_snapshot->sumPeople,
      s_snapshot->pirEventCount, s_snapshot->scanCount, one::readBatteryMv()};
#ifdef COMM_MODE_LORA
  one::beginRadioUart();
  if (one::checkAndConfigureLoRa()) {
    uint8_t payload[one::LORA_PAYLOAD_SIZE];
    one::buildPirLoRaPayload(input, DEVICE_ID, FW_VERSION, payload);
    one::sendLoRaFrame(payload, sizeof(payload));
    delay(300);
    one::sendLoRaFrame(payload, sizeof(payload));
    delay(300);
    s_state = State::RX_WINDOW;
    uint8_t downlink[32], length = 0;
    if (one::receiveLoRaFrame(downlink, sizeof(downlink), length, DOWNLINK_WINDOW_MS))
      applyPirDownlink(downlink, length);
  } else {
    Serial.println("[LORA] 設定確認失敗、送信中止");
  }
  one::endRadioUart();
  one::setLoRaModeSleep();
#else
  one::beginRadioUart();
  uint8_t payload[one::SIGFOX_PAYLOAD_SIZE];
  one::buildPirSigfoxPayload(input, one::readCpuTemperatureDeciC(), payload);
  if (!one::sendSigfoxPayload(payload, sizeof(payload))) Serial.println("[SIGFOX] 送信失敗");
  one::endRadioUart();
  // LSM100Aの低消費モード契約は仕様未確定。3V3_SWはPIRのため落とさない。
#endif
  memset(s_snapshot, 0, sizeof(*s_snapshot));
  s_state = State::WAIT;
  one::watchdogFeed();  // TX/RX状態遷移完了のチェックポイント
  if (s_resetAfterAck) {
    saveRuntimeBeforeReset();
    Serial.flush();
    delay(100);
    NVIC_SystemReset();
  }
}

void reportIfDue() {
  if (!deadlineReached(millis(), s_nextReportAt)) return;
  // 期限をPIR通知より先に評価。送信中に届いた通知は新しいactive期間へ残る。
  takeReportSnapshot();
  transmitSnapshot();
}

}  // namespace

extern "C" void SWI3_EGU3_IRQHandler(void) {
  // ISRはLATCH取得/clear・イベントclear・タスク通知だけ。判定はloopタスク側で行う。
  pirPortIsrBody();
}

void setup() {
  Serial.begin(115200);
  delay(50);
  one::halBegin();
  // PIRがJP10 pad2(3V3_SW)給電のため、PIR版ではこのレールを常時ONにする。
  one::setPeripheralPower(true);
  s_mainTask = xTaskGetCurrentTaskHandle();
  loadSettings();
  memset(s_buffers, 0, sizeof(s_buffers));
  if (!restoreRuntime()) {
    s_nextReportAt = millis() + static_cast<uint32_t>(s_settings.reportIntervalMin) * 60000UL;
    s_rollingWindowAt = millis();
  }

  Bluefruit.begin(1, 0);
  Bluefruit.setName("MonitaOne-PIR");
  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.useActiveScan(false);
  Bluefruit.Scanner.setInterval(static_cast<uint16_t>(SCAN_INTERVAL_MS * 1000UL / 625UL),
                                static_cast<uint16_t>(SCAN_WINDOW_MS * 1000UL / 625UL));
  if (!initPirSense()) Serial.println("[PIR] PORT/SENSE初期化失敗");
#ifdef COMM_MODE_LORA
  one::setLoRaModeSleep();
#endif
  // 必ずloadSettings()後。最大活動時間を加算し、状態機械停止だけをreset対象にする。
  one::watchdogBegin(static_cast<uint32_t>(s_settings.reportIntervalMin) * 60000UL + MAX_ACTIVITY_MS);
  // 機体の焼き間違いを現場で検知できるよう、完全ID・群番号・群内番号を出す。
  Serial.print("[BOOT] Monita One PIR FW="); Serial.print(FW_VERSION);
  Serial.print(" DEVICE_ID=0x"); Serial.print(static_cast<uint8_t>(DEVICE_ID), HEX);
  Serial.print(" group="); Serial.print(static_cast<uint8_t>((DEVICE_ID) >> 5));
  Serial.print(" localNo="); Serial.println(static_cast<uint8_t>((DEVICE_ID) & 0x1F));
}

void loop() {
  reportIfDue();
  maintainPirRearm();

  const uint32_t now = millis();
  uint32_t waitMs = remainingMs(now, s_nextReportAt);
  // SENSEをdisarmしたHIGH保持中だけ短く起き、LOW復帰を確認してからrearmする。
  // 通常待機中にはこのpollingは走らないため、GPIOTE IN event相当の常時消費を生まない。
  if (!s_pirArmed) {
    const uint32_t rearmPollMs = s_pirQuarantined ? 1000U : 50U;
    if (waitMs > rearmPollMs) waitMs = rearmPollMs;
  }
  s_state = State::WAIT;
  // PIR通知または次回レポート期限までブロック。ここでTickless Idleへ入る。
  const uint32_t notifications = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs));

  // タイムアウトとPIRが同時なら、必ずレポートを先に確定する。
  reportIfDue();
  if (notifications) handlePirNotification(notifications);
  maintainPirRearm();
  // 抑止イベント処理もWAITへの状態遷移が完了した時点だけ給餌する。
  if (notifications && s_state == State::WAIT) one::watchdogFeed();
}
