/*
 * Monita One v1.00 標準センサ版
 * Flex v3.20の起床→1CH計測→送信→Tickless Idleを、One直結配線へ移植したもの。
 */
#include <Arduino.h>
#include <DallasTemperature.h>
#include <HX711.h>
#include <OneWire.h>
#include <vl53l4cd_class.h>

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

// 1=HX711、2=LSM6DS、3=DS18B20、4=VL53L4CD。Oneは単一CHのみ。
constexpr uint8_t CH_ASSIGN = 1;
constexpr uint8_t SETTINGS_VERSION = 1;
constexpr char SETTINGS_FILE[] = "/one_sensor_v1.bin";
constexpr uint16_t DEFAULT_SLEEP_MIN = 60;
constexpr uint8_t DEFAULT_SAMPLES_PER_AVG = 10;
constexpr uint8_t DEFAULT_MEASURE_COUNT = 10;
constexpr uint8_t MAX_MEASURE_COUNT = 50;
constexpr uint32_t HX_READY_TIMEOUT_MS = 1000;
constexpr uint32_t DOWNLINK_WINDOW_MS = 2000;
constexpr uint16_t WDT_MARGIN_MIN = 15;
constexpr int32_t STRAIN_SCALE = 100;
constexpr uint8_t LSM6DS_ADDR = 0x6B;

enum ErrorFlag : uint32_t {
  ERR_NONE = 0,
  ERR_HX711 = 1U << 0,
  ERR_I2C = 1U << 1,
  ERR_DS18B20 = 1U << 2,
  ERR_LORA_CONFIG = 1U << 3,
  ERR_RADIO = 1U << 4,
};

struct __attribute__((packed)) SensorSettings {
  uint8_t version;
  uint16_t sleepMinutes;
  uint8_t samplesPerAvg;
  uint8_t measureCount;
  int32_t tareOffset;
  uint32_t crc;
};

SensorSettings s_settings{};
uint32_t s_errors = ERR_NONE;
HX711 s_hx;
OneWire s_oneWire(ONE_PD_SCK_PIN);
DallasTemperature s_ds18(&s_oneWire);

void applyDefaultSettings() {
  memset(&s_settings, 0, sizeof(s_settings));
  s_settings.version = SETTINGS_VERSION;
  s_settings.sleepMinutes = DEFAULT_SLEEP_MIN;
  s_settings.samplesPerAvg = DEFAULT_SAMPLES_PER_AVG;
  s_settings.measureCount = DEFAULT_MEASURE_COUNT;
  s_settings.tareOffset = 0;
}

uint32_t settingsCrc(const SensorSettings &settings) {
  return one::crc32(&settings, offsetof(SensorSettings, crc));
}

bool settingsValid(const SensorSettings &settings) {
  return settings.version == SETTINGS_VERSION &&
         settings.sleepMinutes >= 1 && settings.sleepMinutes <= 1440 &&
         settings.samplesPerAvg >= 1 &&
         settings.measureCount >= 1 && settings.measureCount <= MAX_MEASURE_COUNT &&
         settings.crc == settingsCrc(settings);
}

#ifdef COMM_MODE_LORA
void saveSettings() {
  s_settings.crc = settingsCrc(s_settings);
  if (!one::writeFile(SETTINGS_FILE, &s_settings, sizeof(s_settings)))
    Serial.println("[SETTINGS] 保存失敗");
}
#endif

void loadSettings() {
  SensorSettings stored{};
  if (one::readFile(SETTINGS_FILE, &stored, sizeof(stored)) && settingsValid(stored)) {
    s_settings = stored;
    Serial.println("[SETTINGS] sensor設定をCRC確認後に復元");
  } else {
    applyDefaultSettings();
    Serial.println("[SETTINGS] 初期値を使用");
  }
}

int medianInt(int *values, uint8_t count) {
  for (uint8_t i = 0; i + 1 < count; ++i)
    for (uint8_t j = i + 1; j < count; ++j)
      if (values[i] > values[j]) { const int t = values[i]; values[i] = values[j]; values[j] = t; }
  return values[count / 2U];
}

bool measureHx711(int32_t &value, uint8_t &range) {
  s_hx.begin(ONE_DOUT_PIN, ONE_PD_SCK_PIN);
  // bogde/HX711 の begin() は PD_SCK を LOW に戻さない。直前の失敗時に
  // power_down() を呼んでいると PD_SCK が HIGH のまま残り、HX711 が
  // パワーダウン状態から復帰できずに以後ずっと失敗し続ける。
  // 明示的に power_up() して復帰させる。
  s_hx.power_up();
  delay(1);  // HX711 のパワーアップ安定待ち
  s_hx.set_offset(s_settings.tareOffset);
  int32_t medians[MAX_MEASURE_COUNT] = {};
  int32_t minimum = INT32_MAX, maximum = INT32_MIN;
  for (uint8_t m = 0; m < s_settings.measureCount; ++m) {
    int64_t sum = 0;
    for (uint8_t n = 0; n < s_settings.samplesPerAvg; ++n) {
      if (!s_hx.wait_ready_timeout(HX_READY_TIMEOUT_MS)) {
        s_errors |= ERR_HX711;
        s_hx.power_down();
        return false;
      }
      sum += s_hx.read() - s_settings.tareOffset;
      one::watchdogFeed();
    }
    medians[m] = static_cast<int32_t>(sum / s_settings.samplesPerAvg / STRAIN_SCALE);
    if (medians[m] < minimum) minimum = medians[m];
    if (medians[m] > maximum) maximum = medians[m];
  }
  // int32配列用の局所ソート（VL53のint版と型幅を混ぜない）。
  for (uint8_t i = 0; i + 1 < s_settings.measureCount; ++i)
    for (uint8_t j = i + 1; j < s_settings.measureCount; ++j)
      if (medians[i] > medians[j]) { const int32_t t = medians[i]; medians[i] = medians[j]; medians[j] = t; }
  value = medians[s_settings.measureCount / 2U];
  const uint32_t width = maximum >= minimum ? static_cast<uint32_t>(maximum - minimum) : 0U;
  range = width > 255U ? 255U : static_cast<uint8_t>(width);
  s_hx.power_down();
  return true;
}

#ifdef COMM_MODE_LORA
bool tareHx711() {
  s_hx.begin(ONE_DOUT_PIN, ONE_PD_SCK_PIN);
  int64_t sum = 0;
  constexpr uint8_t TARE_SAMPLES = 10;
  for (uint8_t i = 0; i < TARE_SAMPLES; ++i) {
    if (!s_hx.wait_ready_timeout(HX_READY_TIMEOUT_MS)) return false;
    sum += s_hx.read();
  }
  s_settings.tareOffset = static_cast<int32_t>(sum / TARE_SAMPLES);
  saveSettings();
  s_hx.power_down();
  return true;
}
#endif

bool measureDs18b20(int32_t &value) {
  // HX711操作後に同じ線を再利用しても確実に1-Wireの立上りを作る。
  pinMode(ONE_PD_SCK_PIN, OUTPUT);
  digitalWrite(ONE_PD_SCK_PIN, LOW);
  delay(10);
  digitalWrite(ONE_PD_SCK_PIN, HIGH);
  pinMode(ONE_PD_SCK_PIN, INPUT);
  s_ds18.begin();
  if (s_ds18.getDeviceCount() == 0) { s_errors |= ERR_DS18B20; return false; }
  s_ds18.requestTemperatures();
  const float c = s_ds18.getTempCByIndex(0);
  if (c == DEVICE_DISCONNECTED_C) { s_errors |= ERR_DS18B20; return false; }
  value = static_cast<int32_t>(c >= 0 ? c * 10.0f + 0.5f : c * 10.0f - 0.5f);
  return true;
}

bool measureLsm6ds(int32_t &value) {
  one::SensorWire.beginTransmission(LSM6DS_ADDR);
  one::SensorWire.write(0x10);
  one::SensorWire.write(0x40);  // 104Hz / ±2g
  if (one::SensorWire.endTransmission() != 0) { s_errors |= ERR_I2C; return false; }
  delay(20);
  one::SensorWire.beginTransmission(LSM6DS_ADDR);
  one::SensorWire.write(0x28);
  if (one::SensorWire.endTransmission(false) != 0 ||
      one::SensorWire.requestFrom(LSM6DS_ADDR, static_cast<uint8_t>(6)) < 6) {
    s_errors |= ERR_I2C;
    return false;
  }
  const int16_t ax = static_cast<int16_t>(one::SensorWire.read() | one::SensorWire.read() << 8);
  const int16_t ay = static_cast<int16_t>(one::SensorWire.read() | one::SensorWire.read() << 8);
  const int16_t az = static_cast<int16_t>(one::SensorWire.read() | one::SensorWire.read() << 8);
  const float pitch = atan2f(static_cast<float>(ax),
      sqrtf(static_cast<float>(ay) * ay + static_cast<float>(az) * az)) * 1800.0f / PI;
  value = static_cast<int32_t>(pitch);
  return true;
}

bool measureVl53l4cd(int32_t &value) {
  VL53L4CD sensor(&one::SensorWire, -1);
  if (sensor.begin() != 0 || sensor.InitSensor() != 0) { s_errors |= ERR_I2C; return false; }
  sensor.VL53L4CD_SetRangeTiming(100, 0);
  sensor.VL53L4CD_StartRanging();
  int samples[10];
  for (uint8_t i = 0; i < 10; ++i) {
    uint8_t ready = 0;
    const uint32_t started = millis();
    while (!ready && millis() - started < 1000U) {
      sensor.VL53L4CD_CheckForDataReady(&ready);
      delay(5);
    }
    if (!ready) { sensor.VL53L4CD_StopRanging(); s_errors |= ERR_I2C; return false; }
    VL53L4CD_Result_t result{};
    sensor.VL53L4CD_GetResult(&result);
    sensor.VL53L4CD_ClearInterrupt();
    samples[i] = result.distance_mm;
  }
  sensor.VL53L4CD_StopRanging();
  value = medianInt(samples, 10);
  return true;
}

bool measureChannel(int32_t &value, uint8_t &range) {
  range = 0;
  switch (CH_ASSIGN) {
    case 1: return measureHx711(value, range);
    case 2: return measureLsm6ds(value);
    case 3: return measureDs18b20(value);
    case 4: return measureVl53l4cd(value);
    default: return false;
  }
}

#ifdef COMM_MODE_LORA
void sendDownlinkAck(uint8_t status) {
  uint8_t ack[9] = {0x05, DEVICE_ID, status,
      static_cast<uint8_t>(s_settings.sleepMinutes >> 8),
      static_cast<uint8_t>(s_settings.sleepMinutes),
      s_settings.samplesPerAvg, s_settings.measureCount,
      static_cast<uint8_t>((s_settings.sleepMinutes + WDT_MARGIN_MIN) >> 8),
      static_cast<uint8_t>(s_settings.sleepMinutes + WDT_MARGIN_MIN)};
  one::sendLoRaFrame(ack, sizeof(ack));
  delay(300);
}

bool applyDownlinkPayload(const uint8_t *payload, uint8_t length) {
  constexpr uint16_t COMPANY_ID = 0xC0DE;
  constexpr uint8_t PKT_TYPE = 0x81;
  constexpr uint8_t DL_FLAG_TIME = 1U << 0;
  constexpr uint8_t DL_FLAG_SLEEP = 1U << 1;
  constexpr uint8_t DL_FLAG_AVG_MEDIAN = 1U << 2;
  constexpr uint8_t DL_FLAG_TARE = 1U << 3;
  constexpr uint8_t STATUS_OK = 0;
  constexpr uint8_t STATUS_RANGE_ERROR = 1;
  constexpr uint8_t STATUS_TIME_UNSUPPORTED = 2;
  if (length != 15 || (static_cast<uint16_t>(payload[0]) << 8 | payload[1]) != COMPANY_ID ||
      payload[2] != PKT_TYPE || payload[3] != DEVICE_ID) return false;

  const uint8_t flags = payload[4];
  uint8_t status = STATUS_OK;
  bool changed = false;
  bool resetNeeded = false;
  if (flags & DL_FLAG_TIME) {
    // DS3231を持たないことをGatewayへ明示する専用status。黙って値域エラーにしない。
    status = STATUS_TIME_UNSUPPORTED;
  }
  if (flags & DL_FLAG_SLEEP) {
    const uint16_t requested = static_cast<uint16_t>(payload[11]) << 8 | payload[12];
    if (requested < 1 || requested > 1440) status = STATUS_RANGE_ERROR;
    else if (requested != s_settings.sleepMinutes) {
      s_settings.sleepMinutes = requested;
      changed = resetNeeded = true;
    }
  }
  if (flags & DL_FLAG_AVG_MEDIAN) {
    if (payload[13] == 0 || payload[14] == 0 || payload[14] > MAX_MEASURE_COUNT)
      status = STATUS_RANGE_ERROR;
    else {
      s_settings.samplesPerAvg = payload[13];
      s_settings.measureCount = payload[14];
      changed = true;
    }
  }
  if (flags & DL_FLAG_TARE) {
    if (!tareHx711()) status = STATUS_RANGE_ERROR;
    else changed = true;
  }
  if (changed) saveSettings();
  sendDownlinkAck(status);
  if (resetNeeded) {
    Serial.flush();
    delay(100);
    NVIC_SystemReset();
  }
  return true;
}
#endif

void sendReport(int32_t channel, uint8_t range, uint16_t batteryMv, int16_t temperature) {
  one::SensorPayloadInput input{channel, temperature, batteryMv, range};
#ifdef COMM_MODE_LORA
  if (!one::checkAndConfigureLoRa()) { s_errors |= ERR_LORA_CONFIG; return; }
  uint8_t payload[one::LORA_PAYLOAD_SIZE];
  one::buildSensorLoRaPayload(input, DEVICE_ID, FW_VERSION, payload);
  one::sendLoRaFrame(payload, sizeof(payload));
  delay(300);
  one::sendLoRaFrame(payload, sizeof(payload));
  delay(300);
  uint8_t downlink[32], length = 0;
  if (one::receiveLoRaFrame(downlink, sizeof(downlink), length, DOWNLINK_WINDOW_MS))
    applyDownlinkPayload(downlink, length);
#else
  uint8_t payload[one::SIGFOX_PAYLOAD_SIZE];
  one::buildSensorSigfoxPayload(input, payload);
  if (!one::sendSigfoxPayload(payload, sizeof(payload))) s_errors |= ERR_RADIO;
#endif
}

void sleepUntilNextCycle() {
  one::watchdogFeed();
  one::setLoRaModeNormal();  // 逆給電対策: 3V3_SWを落とす前にLOW
  one::endSensorI2c();
  one::setPeripheralPower(false);
  Serial.print("[SLEEP] "); Serial.print(s_settings.sleepMinutes); Serial.println(" min");
  Serial.flush();
  vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(s_settings.sleepMinutes) * 60000UL));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  one::halBegin();
  loadSettings();
  one::watchdogBegin(static_cast<uint32_t>(s_settings.sleepMinutes + WDT_MARGIN_MIN) * 60000UL);

  // USB CDC の列挙完了前に出力すると PC に届かず消える。
  // Flex v3.20 と同じ方式で、1秒間隔で複数回リピートしてモニタ接続を待つ。
  // ONE_DEBUG_BOOT_REPEAT=0 を渡せば本番ビルドで待ち時間をゼロにできる。
#ifndef ONE_DEBUG_BOOT_REPEAT
#define ONE_DEBUG_BOOT_REPEAT 5
#endif
  for (uint8_t i = 0; i < ONE_DEBUG_BOOT_REPEAT; ++i) {
    Serial.print("[BOOT] Monita One sensor FW=");
    Serial.print(FW_VERSION);
    // 機体の焼き間違いを現場で検知できるよう、完全ID・群番号・群内番号を出す。
    Serial.print(" DEVICE_ID=0x");
    Serial.print(static_cast<uint8_t>(DEVICE_ID), HEX);
    Serial.print(" group=");
    Serial.print(static_cast<uint8_t>((DEVICE_ID) >> 5));
    Serial.print(" localNo=");
    Serial.print(static_cast<uint8_t>((DEVICE_ID) & 0x1F));
    Serial.print(" CH_ASSIGN=");
    Serial.print(CH_ASSIGN);
    Serial.print(" sleepMin=");
    Serial.println(s_settings.sleepMinutes);
    Serial.flush();
    one::watchdogFeed();
    delay(1000);
  }
}

void loop() {
  one::watchdogFeed();
  s_errors = ERR_NONE;
  one::setPeripheralPower(true);
  one::beginSensorI2c();
  one::beginRadioUart();
  delay(500);

  int32_t channel = 0;
  uint8_t range = 0;
  const bool measured = measureChannel(channel, range);
  const uint16_t battery = one::readBatteryMv();
  const int16_t temperature = one::readCpuTemperatureDeciC();

  // 実機の切り分け用。計測が成功／失敗したかと、その値を必ず出す。
  Serial.print("[MEAS] ok=");   Serial.print(measured ? 1 : 0);
  Serial.print(" ch=");         Serial.print(channel);
  Serial.print(" range=");      Serial.print(range);
  Serial.print(" batt_mV=");    Serial.print(battery);
  Serial.print(" cpuTemp_dC="); Serial.print(temperature);
  Serial.print(" errors=0x");   Serial.println(s_errors, HEX);
  Serial.flush();

  // 本番既定では計測エラー時に送信しない。疎通試験用緩和は明示flagだけに隔離する。
#if defined(ONE_ALLOW_TX_WITH_ERRORS)
  (void)measured;
  sendReport(channel, range, battery, temperature);
  Serial.print("[TX] 送信試行 errors=0x"); Serial.println(s_errors, HEX);
#else
  if (measured && s_errors == ERR_NONE) {
    sendReport(channel, range, battery, temperature);
    // sendReport() は失敗時に ERR_LORA_CONFIG / ERR_RADIO を立てる。
    // [MEAS] の errors は計測フェーズの値なので、送信後に改めて出す。
    if (s_errors == ERR_NONE) {
      Serial.println("[TX] 送信完了");
    } else {
      Serial.print("[TX] 送信失敗 errors=0x"); Serial.println(s_errors, HEX);
    }
  } else {
    Serial.println("[TX] 計測エラーのため送信を中止");
  }
#endif
  Serial.flush();
  sleepUntilNextCycle();
}
