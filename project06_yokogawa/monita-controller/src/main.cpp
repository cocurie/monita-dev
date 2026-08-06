#include <Arduino.h>
#include <lvgl.h>          //8.4.0
#include <TFT_eSPI.h>      //2.5.43
#include "src/ui/ui.h"

#include <XPT2046_Touchscreen.h>
#include <nvs_flash.h>
#include <algorithm>
#include <cmath>

#include <NimBLEDevice.h>
#include <vector>
#include <atomic>
using std::atomic;

#include <SPI.h>
#include <SD.h>

// ===== SDカード (Dumpコマンドで受信したログの保存先。AVL基板と共通ピン配置) =====
#define SD_CS_PIN 5
static bool g_sd_ok = false;

// ===== 電源スイッチ (AVLファームと同一ハード: LTC2954プッシュボタン電源コントローラー) =====
// PWボタン長押し(ハード側タイマー)で*INTがアサートされ、S_VP(IO36)がLOWになる。
// 確認ダイアログ「はい」でSW_OFF(IO25)をLOW出力し*KILLをアサート、実電源を落とす。
#define Sensor_VP 36
#define sw_off    25

// ===== 画面解像度 (AVLファーム踏襲: 320x240) =====
static const uint16_t screenWidth  = 320;
static const uint16_t screenHeight = 240;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[screenWidth * 10];
static lv_color_t buf2[screenWidth * 10];

TFT_eSPI tft = TFT_eSPI(screenWidth, screenHeight);

// ===== タッチパネル (XPT2046, AVLファームと同一配線) =====
#define XPT2046_IRQ 13
#define XPT2046_MOSI 23
#define XPT2046_MISO 19
#define XPT2046_CLK 18
#define XPT2046_CS 33
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

/* Display flushing */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(disp);
}

// ===== タッチキャリブレーション (AVLファームから移植) =====
struct TouchCal {
  float a, b, c;
  float d, e, f;
  bool  valid;
};
static TouchCal g_cal = {0,0,0,0,0,0,false};

static lv_obj_t* draw_cross(lv_coord_t x, lv_coord_t y) {
  const int len = 10;
  lv_obj_t* line1 = lv_line_create(lv_scr_act());
  static lv_point_t pts1[2];
  pts1[0] = { (lv_coord_t)(x - len), y };
  pts1[1] = { (lv_coord_t)(x + len), y };
  lv_line_set_points(line1, pts1, 2);

  lv_obj_t* line2 = lv_line_create(lv_scr_act());
  static lv_point_t pts2[2];
  pts2[0] = { x, (lv_coord_t)(y - len) };
  pts2[1] = { x, (lv_coord_t)(y + len) };
  lv_line_set_points(line2, pts2, 2);

  lv_obj_t* cont = lv_obj_create(lv_scr_act());
  lv_obj_set_size(cont, 1, 1);
  lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
  lv_obj_align(cont, LV_ALIGN_TOP_LEFT, x, y);
  lv_obj_set_style_border_opa(cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_style_radius(cont, 0, 0);
  lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_parent(line1, cont);
  lv_obj_set_parent(line2, cont);
  return cont;
}

static bool read_raw_touch(int &rx, int &ry, int samples = 10) {
  if (!touchscreen.touched()) return false;
  int xs[20], ys[20];
  samples = min(samples, 20);
  for (int i=0;i<samples;i++) {
    if (!touchscreen.touched()) return false;
    TS_Point p = touchscreen.getPoint();
    xs[i] = p.x;
    ys[i] = p.y;
    delay(5);
  }
  auto nth = samples/2;
  std::nth_element(xs, xs+nth, xs+samples);
  std::nth_element(ys, ys+nth, ys+samples);
  rx = xs[nth];
  ry = ys[nth];
  return true;
}

static bool solve_affine_least_squares(
  const int raw_x[], const int raw_y[],
  const int scr_x[], const int scr_y[],
  int n, TouchCal &out)
{
  if (n < 3) return false;

  double A[6][6] = {0};
  double B[6]    = {0};

  auto add_row = [&](double xr, double yr, double xs, double ys){
    double rowx[6] = { xr, yr, 1.0, 0,  0,  0 };
    double rowy[6] = { 0,  0,  0,   xr, yr, 1.0 };
    for (int r=0;r<6;r++){
      for(int c=0;c<6;c++){
        A[r][c] += rowx[r]*rowx[c] + rowy[r]*rowy[c];
      }
    }
    B[0] += xr*xs;  B[1] += yr*xs;  B[2] += 1.0*xs;
    B[3] += xr*ys;  B[4] += yr*ys;  B[5] += 1.0*ys;
  };

  for (int i=0;i<n;i++){
    add_row((double)raw_x[i], (double)raw_y[i],
            (double)scr_x[i], (double)scr_y[i]);
  }

  const int N = 6;
  double M[N][N+1];
  for (int r=0;r<N;r++){
    for (int c=0;c<N;c++) M[r][c] = A[r][c];
    M[r][N] = B[r];
  }

  for (int col=0; col<N; col++) {
    int piv = col;
    for (int r=col+1; r<N; r++) {
      if (fabs(M[r][col]) > fabs(M[piv][col])) piv = r;
    }
    if (fabs(M[piv][col]) < 1e-9) return false;
    if (piv != col) {
      for (int c=col; c<=N; c++) std::swap(M[piv][c], M[col][c]);
    }
    double div = M[col][col];
    for (int c=col; c<=N; c++) M[col][c] /= div;
    for (int r=0; r<N; r++){
      if (r==col) continue;
      double f = M[r][col];
      for (int c=col; c<=N; c++) M[r][c] -= f*M[col][c];
    }
  }

  out.a = M[0][N];
  out.b = M[1][N];
  out.c = M[2][N];
  out.d = M[3][N];
  out.e = M[4][N];
  out.f = M[5][N];
  out.valid = true;
  return true;
}

static bool save_touch_calibration(const TouchCal &cal){
  nvs_handle h;
  if (nvs_open("storage", NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_blob(h, "touch_cal", &cal, sizeof(cal));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

static bool load_touch_calibration(TouchCal &cal){
  nvs_handle h;
  if (nvs_open("storage", NVS_READONLY, &h) != ESP_OK) return false;
  size_t sz = sizeof(cal);
  esp_err_t err = nvs_get_blob(h, "touch_cal", &cal, &sz);
  nvs_close(h);
  if (err == ESP_OK && sz == sizeof(cal) && cal.valid) return true;
  cal.valid = false;
  return false;
}

static bool run_touch_calibration_ui() {
  const int TX = 20, TY = 20;
  const int scr_x[5] = { TX, (int)screenWidth-1-TX, (int)screenWidth-1-TX, TX, (int)screenWidth/2 };
  const int scr_y[5] = { TY, TY, (int)screenHeight-1-TY, (int)screenHeight-1-TY, (int)screenHeight/2 };

  int raw_x[5], raw_y[5];

  lv_obj_t* label = lv_label_create(lv_scr_act());
  lv_label_set_text(label, "Touch calibration: Tap the dots in order");
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 8);

  for (int i=0;i<5;i++){
    lv_obj_t* cross = draw_cross(scr_x[i], scr_y[i]);
    lv_obj_t* dot = lv_obj_create(lv_scr_act());
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x000000), 0);
    lv_obj_align(dot, LV_ALIGN_TOP_LEFT, scr_x[i]-4, scr_y[i]-4);

    int rx=0, ry=0, stable=0;
    uint32_t t0 = millis();
    while (millis() - t0 < 60000) {
      lv_task_handler();
      if (touchscreen.touched()) {
        int tr_x, tr_y;
        if (read_raw_touch(tr_x, tr_y, 5)) {
          rx = tr_x; ry = tr_y; stable++;
          if (stable > 4) break;
        }
      }
      delay(10);
    }
    lv_obj_del(cross);
    lv_obj_del(dot);
    if (stable <= 4){
      lv_label_set_text(label, "Calibration aborted: timeout");
      return false;
    }
    raw_x[i] = rx; raw_y[i] = ry;

    // タップ後の離し待ち＋デバウンス
    uint32_t rt0 = millis();
    while (touchscreen.touched() && millis() - rt0 < 2000) { lv_task_handler(); delay(5); }
    delay(200);
  }

  TouchCal cal;
  if (!solve_affine_least_squares(raw_x, raw_y, scr_x, scr_y, 5, cal)){
    lv_label_set_text(label, "Failed to calculate coefficients");
    return false;
  }
  save_touch_calibration(cal);
  g_cal = cal;

  lv_label_set_text(label, "Calibration complete! Saved to NVS.");
  return true;
}

static void ensure_touch_calibrated() {
  if (!load_touch_calibration(g_cal)) {
    run_touch_calibration_ui();
    uint32_t t0 = millis();
    while (millis() - t0 < 3000) {
      lv_timer_handler();
      delay(10);
    }
  }
}

static void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
  if (touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();
    int rx = p.x, ry = p.y;

    int x = 0, y = 0;
    if (g_cal.valid) {
      float xf = g_cal.a * rx + g_cal.b * ry + g_cal.c;
      float yf = g_cal.d * rx + g_cal.e * ry + g_cal.f;
      x = (int)lrintf(xf);
      y = (int)lrintf(yf);
    }
    x = constrain(x, 0, screenWidth - 1);
    y = constrain(y, 0, screenHeight - 1);

    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// ===== BLEクライアント (XIAO ESP32-C3 "ver1.1" とのGATT通信) =====
// project06_yokogawa/ver1.1 側 src/main.cpp が実装しているNordic UART Service。
#include "src/ui/src/ble_client.h"

static BLEUUID bleServiceUUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
static BLEUUID bleCharUUID_RX("6e400002-b5a3-f393-e0a9-e50e24dcca9e");
static BLEUUID bleCharUUID_TX("6e400003-b5a3-f393-e0a9-e50e24dcca9e");

static NimBLEScan* pBleScan = nullptr;
static NimBLEClient* pBleClient = nullptr;
static NimBLERemoteCharacteristic* pRxCharacteristic = nullptr;
static bool bleConnected = false;

// ===== パッシブスキャン: ver1.1ファームのBLEアドバタイズ(Manufacturer Specific Data)を常時受信 =====
// フォーマットは ver1.1 側 src/main.cpp の bleAdvertiseMeasurement() コメント参照。
// CompanyID=0xFFFF, PktType=0x11 の20バイトMSDのみを対象とする（接続は不要）。
static const uint8_t MONITA_COMPANY_LO = 0xFF;
static const uint8_t MONITA_COMPANY_HI = 0xFF;
static const uint8_t MONITA_PKT_TYPE   = 0x11;
static const uint32_t DEVICE_STALE_MS  = 15000;  // これより古い最終受信のデバイスは一覧から除外

struct MonitaDevice {
    NimBLEAdvertisedDevice* adv;  // GATT接続用（アドレス保持のため複製を保持）
    uint8_t devId;
    int16_t chRaw[8];  // CH1-5:µε/mm相当(そのままの値), CH6:0.1℃単位, CH7-8:mV。0x7FFFはNaN
    bool    chOk[8];
    uint32_t lastSeenMs;
};
static std::vector<MonitaDevice> g_devices;

// Mesure画面で表示中のデバイス（GATT接続の有無に関わらず、パッシブ受信値を表示し続ける）
static bool g_hasSelected = false;
static NimBLEAddress g_selectedAddr;

static MonitaDevice* findDeviceByAddress(const NimBLEAddress& addr) {
    for (auto& d : g_devices) {
        if (d.adv->getAddress().equals(addr)) return &d;
    }
    return nullptr;
}

class MonitaAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) override {
        if (!advertisedDevice->haveManufacturerData()) return;
        std::string md = advertisedDevice->getManufacturerData();
        if (md.size() != 20) return;
        const uint8_t* b = (const uint8_t*)md.data();
        if (b[0] != MONITA_COMPANY_LO || b[1] != MONITA_COMPANY_HI || b[2] != MONITA_PKT_TYPE) return;

        MonitaDevice* slot = findDeviceByAddress(advertisedDevice->getAddress());
        if (slot == nullptr) {
            MonitaDevice nd{};
            nd.adv = new NimBLEAdvertisedDevice(*advertisedDevice);
            g_devices.push_back(nd);
            slot = &g_devices.back();
        } else {
            *slot->adv = *advertisedDevice;  // アドレス以外の広告情報も最新化
        }

        slot->devId = b[3];
        for (int i = 0; i < 8; i++) {
            int16_t v = (int16_t)((uint16_t)b[4 + i * 2] | ((uint16_t)b[5 + i * 2] << 8));
            slot->chOk[i]  = (v != 0x7FFF);
            slot->chRaw[i] = v;
        }
        slot->lastSeenMs = millis();
    }
};

// LVGLはスレッドセーフではないため、NimBLEのコールバック(別タスク)から
// ラベル更新する際は lv_async_call() でメインループ側に処理を委譲する。
struct LabelUpdate { lv_obj_t* label; char* text; };

static void apply_label_update(void* p) {
    LabelUpdate* u = (LabelUpdate*)p;
    if (u->label != nullptr && lv_obj_is_valid(u->label)) {
        lv_label_set_text(u->label, u->text);
    }
    free(u->text);
    delete u;
}

static void set_label_async(lv_obj_t* label, const String& text) {
    LabelUpdate* u = new LabelUpdate();
    u->label = label;
    u->text = strdup(text.c_str());
    lv_async_call(apply_label_update, u);
}

class MonitaClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) override {
        bleConnected = true;
        Serial.println("[BLE] Connected");
    }
    void onDisconnect(NimBLEClient* pClient) override {
        bleConnected = false;
        Serial.println("[BLE] Disconnected");
    }
};

// ===== Dump受信 (SDログのチャンク転送) =====
// プロトコル: "DUMPHDR:<総バイト数>" → 生データチャンク(複数notify) → "DUMPEND"
// SDへの書き込みはSPIバス競合を避けるため、ここではメモリに溜めるだけにして
// 実際の書き込みは loop() (メインスレッド)側の dumpWriteIfReady() で行う。
static bool g_dumpReceiving = false;
static uint32_t g_dumpExpected = 0;
static std::vector<uint8_t> g_dumpBuffer;
static volatile bool g_dumpReady = false;
static uint8_t g_dumpDeviceId = 0;

// RUN=1(計測中)/RUN=0(停止中)に応じて、Statusラベルの文言・色とStart/Stopボタンの表示を切り替える。
// NimBLEコールバック(別タスク)から呼ばれるため lv_async_call() でメインループ側に委譲する。
static void applyRunStateAsync(bool running) {
    bool* p = new bool(running);
    lv_async_call([](void* arg) {
        bool running = *(bool*)arg;
        delete (bool*)arg;
        if (running) {
            lv_label_set_text(ui_Label11, "Status: Running");
            lv_obj_set_style_text_color(ui_Label11, lv_color_hex(0x00AA00), LV_PART_MAIN);
            lv_obj_add_flag(ui_start, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ui_stop, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(ui_Label11, "Status: Stopped");
            lv_obj_set_style_text_color(ui_Label11, lv_color_hex(0x888888), LV_PART_MAIN);
            lv_obj_clear_flag(ui_start, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_stop, LV_OBJ_FLAG_HIDDEN);
        }
    }, p);
}

// "LIVE:CH1=1.94,CH2=1.41,...,CH8=1.884,TIME=2026-08-10 05:51:00" を解析して
// Mesure画面のCH1〜8・timestampラベルを更新する（GATT接続中の実時間表示用）。
static void applyLiveUpdate(const String& msg) {
    String body = msg.substring(5); // "LIVE:" の後ろ

    int timeIdx = body.indexOf("TIME=");
    String chPart  = (timeIdx >= 0) ? body.substring(0, timeIdx) : body;
    String timeVal = (timeIdx >= 0) ? body.substring(timeIdx + 5) : String("--");

    static const char* keys[8]  = {"CH1=", "CH2=", "CH3=", "CH4=", "CH5=", "CH6=", "CH7=", "CH8="};
    static const char* units[8] = {"uS", "uS", "uS", "mm", "mm", "C", "V", "V"};
    lv_obj_t* chLabels[8] = {ui_CH1, ui_CH2, ui_CH3, ui_CH4, ui_CH5, ui_CH6, ui_CH7, ui_CH8};

    for (int i = 0; i < 8; i++) {
        int p = chPart.indexOf(keys[i]);
        if (p < 0) continue;
        int vs = p + strlen(keys[i]);
        int ve = chPart.indexOf(',', vs);
        String val = (ve >= 0) ? chPart.substring(vs, ve) : chPart.substring(vs);
        char text[24];
        snprintf(text, sizeof(text), "CH%d:%s%s", i + 1, val.c_str(), units[i]);
        set_label_async(chLabels[i], text);
    }
    set_label_async(ui_timestamp, "timestamp: " + timeVal);
}

static void bleNotifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    if (g_dumpReceiving) {
        if (length == 7 && memcmp(pData, "DUMPEND", 7) == 0) {
            g_dumpReceiving = false;
            g_dumpReady = true;
            Serial.printf("[BLE] Dump complete: %u bytes received\n", (unsigned)g_dumpBuffer.size());
        } else {
            g_dumpBuffer.insert(g_dumpBuffer.end(), pData, pData + length);
        }
        return;
    }

    String msg((char*)pData, length);
    Serial.print("[BLE] recv: ");
    Serial.println(msg);

    if (msg.startsWith("LIVE:")) {
        applyLiveUpdate(msg);
        return;
    }

    if (msg.startsWith("DUMPHDR:")) {
        g_dumpExpected = (uint32_t)msg.substring(8).toInt();
        g_dumpBuffer.clear();
        g_dumpBuffer.reserve(g_dumpExpected);
        g_dumpReceiving = true;
        Serial.printf("[BLE] Dump starting: %u bytes expected\n", (unsigned)g_dumpExpected);
        return;
    }

    // GET/OK:INTERVAL 応答例: "INTERVAL=1;N=5;M=5;RUN=1;DEVID=1;TIME=..."（INTERVALは分単位）
    int idx = msg.indexOf("INTERVAL=");
    if (idx >= 0) {
        int start = idx + 9;
        int end = msg.indexOf(';', start);
        String interval = (end >= 0) ? msg.substring(start, end) : msg.substring(start);
        set_label_async(ui_sleepDisplay, "Interval: " + interval + " min.");
    } else if (msg.startsWith("OK:INTERVAL=")) {
        set_label_async(ui_sleepDisplay, "Interval: " + msg.substring(12) + " min.");
    }

    // GET応答("RUN=1;...")・START/STOP応答("OK:RUN=1"/"OK:RUN=0")のいずれにも対応
    int runIdx = msg.indexOf("RUN=");
    if (runIdx >= 0 && (size_t)(runIdx + 4) < msg.length()) {
        applyRunStateAsync(msg.charAt(runIdx + 4) == '1');
    }
}

// g_devices は常時バックグラウンドでパッシブスキャン受信されているので、
// ここでは「最近見えているデバイス」のスナップショットをDropdownへ反映するだけでよい。
// 文字列が前回と同じなら lv_dropdown_set_options() を呼ばない（不要な再描画・選択リセットを避ける）。
static char g_lastDropdownOptions[512] = "";

static void refreshDeviceListOptions() {
    uint32_t now = millis();

    // 古いエントリ（しばらく電波を受信できていないデバイス）を除去
    for (size_t i = 0; i < g_devices.size();) {
        if (now - g_devices[i].lastSeenMs > DEVICE_STALE_MS) {
            delete g_devices[i].adv;
            g_devices.erase(g_devices.begin() + i);
        } else {
            i++;
        }
    }

    char options[512];
    if (g_devices.empty()) {
        snprintf(options, sizeof(options), "No devices found");
    } else {
        options[0] = '\0';
        for (size_t i = 0; i < g_devices.size(); i++) {
            char line[48];
            snprintf(line, sizeof(line), "Monita-%02X (%s)", g_devices[i].devId,
                     g_devices[i].adv->getAddress().toString().c_str());
            strncat(options, line, sizeof(options) - strlen(options) - 1);
            if (i + 1 < g_devices.size()) {
                strncat(options, "\n", sizeof(options) - strlen(options) - 1);
            }
        }
    }

    if (strcmp(options, g_lastDropdownOptions) != 0) {
        lv_dropdown_set_options(ui_Dropdown1, options);
        strncpy(g_lastDropdownOptions, options, sizeof(g_lastDropdownOptions) - 1);
        g_lastDropdownOptions[sizeof(g_lastDropdownOptions) - 1] = '\0';
        Serial.printf("[BLE] %u device(s) visible\n", (unsigned)g_devices.size());
    }
}

extern "C" void ble_scan_and_populate(void) {
    refreshDeviceListOptions();
    if (lv_scr_act() != ui_DviceList) {
        _ui_screen_change(&ui_DviceList, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, &ui_DviceList_screen_init);
    }
}

// DviceList画面を表示している間、loop()から定期的に呼んで一覧を自動更新する。
static void deviceListAutoRefresh() {
    static uint32_t lastRefresh = 0;
    uint32_t now = millis();
    if (now - lastRefresh < 1000) return;
    lastRefresh = now;
    if (lv_scr_act() == ui_DviceList) {
        refreshDeviceListOptions();
    }
}

// Mesure画面のCH1〜8・timestampラベルをg_selectedAddrの最新パッシブ受信値で更新する。
// メインループから直接呼ばれるので lv_async_call は不要。
static void measureScreenAutoRefresh() {
    static uint32_t lastRefresh = 0;
    uint32_t now = millis();
    if (now - lastRefresh < 500) return;
    lastRefresh = now;

    if (lv_scr_act() != ui_Mesure || !g_hasSelected) return;

    // GATT接続中は ble_client の "LIVE:" Notify が表示を更新するので、
    // ここでパッシブ受信のキャッシュ値を書き戻して上書きしないようにする。
    if (pBleClient != nullptr && pBleClient->isConnected()) return;

    MonitaDevice* d = findDeviceByAddress(g_selectedAddr);
    if (d == nullptr) {
        lv_label_set_text(ui_timestamp, "timestamp: (no signal)");
        return;
    }

    auto fmtCh = [&](int i, const char* unit, float scale) {
        char b[24];
        if (d->chOk[i]) {
            snprintf(b, sizeof(b), "%.1f%s", d->chRaw[i] * scale, unit);
        } else {
            snprintf(b, sizeof(b), "--%s", unit);
        }
        return String(b);
    };

    lv_label_set_text(ui_CH1, ("CH1:" + fmtCh(0, "uS", 1.0f)).c_str());
    lv_label_set_text(ui_CH2, ("CH2:" + fmtCh(1, "uS", 1.0f)).c_str());
    lv_label_set_text(ui_CH3, ("CH3:" + fmtCh(2, "uS", 1.0f)).c_str());
    lv_label_set_text(ui_CH4, ("CH4:" + fmtCh(3, "mm", 1.0f)).c_str());
    lv_label_set_text(ui_CH5, ("CH5:" + fmtCh(4, "mm", 1.0f)).c_str());
    lv_label_set_text(ui_CH6, ("CH6:" + fmtCh(5, "C", 0.1f)).c_str());
    lv_label_set_text(ui_CH7, ("CH7:" + fmtCh(6, "V", 0.001f)).c_str());
    lv_label_set_text(ui_CH8, ("CH8:" + fmtCh(7, "V", 0.001f)).c_str());

    char ts[32];
    snprintf(ts, sizeof(ts), "timestamp: %lus ago", (unsigned long)((now - d->lastSeenMs) / 1000));
    lv_label_set_text(ui_timestamp, ts);
}

// 受信済みのDumpデータをSDへ書き込む（メインループから呼ぶ。SPIバス競合を避けるため）。
static void dumpWriteIfReady() {
    if (!g_dumpReady) return;
    g_dumpReady = false;

    if (!g_sd_ok || g_dumpBuffer.empty()) {
        Serial.println("[DUMP] SD unavailable or empty buffer, discarding");
        g_dumpBuffer.clear();
        return;
    }

    char path[40];
    snprintf(path, sizeof(path), "/dump_%02X_%lu.csv", g_dumpDeviceId, (unsigned long)millis());
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[DUMP] Failed to open %s for write\n", path);
        g_dumpBuffer.clear();
        return;
    }
    f.write(g_dumpBuffer.data(), g_dumpBuffer.size());
    f.close();
    Serial.printf("[DUMP] Saved %u bytes to %s\n", (unsigned)g_dumpBuffer.size(), path);
    g_dumpBuffer.clear();
}

extern "C" void ble_connect_selected(void) {
    // すでに接続済みなら測定画面へ戻るだけ
    if (pBleClient != nullptr && pBleClient->isConnected()) {
        if (lv_scr_act() != ui_Mesure) {
            _ui_screen_change(&ui_Mesure, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, &ui_Mesure_screen_init);
        }
        return;
    }

    uint16_t idx = lv_dropdown_get_selected(ui_Dropdown1);
    if (idx >= g_devices.size()) {
        Serial.println("[BLE] Invalid selection index");
        return;
    }
    NimBLEAdvertisedDevice* target = g_devices[idx].adv;
    g_selectedAddr = target->getAddress();
    g_hasSelected = true;
    g_dumpDeviceId = g_devices[idx].devId;

    if (pBleClient == nullptr) {
        pBleClient = NimBLEDevice::createClient();
        pBleClient->setClientCallbacks(new MonitaClientCallbacks(), false);
    }

    Serial.printf("[BLE] Connecting to Monita-%02X (%s) ...\n",
                  g_devices[idx].devId, target->getAddress().toString().c_str());
    if (!pBleClient->connect(target)) {
        Serial.println("[BLE] Connection failed");
        return;
    }

    NimBLERemoteService* pService = pBleClient->getService(bleServiceUUID);
    if (pService == nullptr) {
        Serial.println("[BLE] NUS service not found");
        return;
    }

    pRxCharacteristic = pService->getCharacteristic(bleCharUUID_RX);
    NimBLERemoteCharacteristic* pTxCharacteristic = pService->getCharacteristic(bleCharUUID_TX);

    if (pTxCharacteristic != nullptr && pTxCharacteristic->canNotify()) {
        pTxCharacteristic->subscribe(true, bleNotifyCallback);
    }

    Serial.println("[BLE] Connected and subscribed");

    _ui_screen_change(&ui_Mesure, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, &ui_Mesure_screen_init);

    if (pRxCharacteristic != nullptr) {
        pRxCharacteristic->writeValue("GET"); // 現在の設定値を取得
    }
}

extern "C" void ble_apply_settings(void) {
    if (pBleClient == nullptr || !pBleClient->isConnected() || pRxCharacteristic == nullptr) {
        Serial.println("[BLE] Not connected");
        return;
    }
    int32_t interval = lv_spinbox_get_value(ui_Spinbox1);
    int32_t avgN      = lv_spinbox_get_value(ui_Spinbox2);
    int32_t avgM      = lv_spinbox_get_value(ui_Spinbox3);

    char cmd[24];
    snprintf(cmd, sizeof(cmd), "INTERVAL:%ld", (long)interval);
    Serial.printf("[BLE] send: %s\n", cmd);
    pRxCharacteristic->writeValue(cmd);
    delay(50);

    snprintf(cmd, sizeof(cmd), "AVGN:%ld", (long)avgN);
    Serial.printf("[BLE] send: %s\n", cmd);
    pRxCharacteristic->writeValue(cmd);
    delay(50);

    snprintf(cmd, sizeof(cmd), "AVGM:%ld", (long)avgM);
    Serial.printf("[BLE] send: %s\n", cmd);
    pRxCharacteristic->writeValue(cmd);
}

extern "C" void ble_tare(void) {
    if (pBleClient == nullptr || !pBleClient->isConnected() || pRxCharacteristic == nullptr) {
        Serial.println("[BLE] Not connected");
        return;
    }
    pRxCharacteristic->writeValue("TARE");
}

extern "C" void ble_dump(void) {
    if (pBleClient == nullptr || !pBleClient->isConnected() || pRxCharacteristic == nullptr) {
        Serial.println("[BLE] Not connected");
        return;
    }
    Serial.println("[BLE] send: DUMP");
    pRxCharacteristic->writeValue("DUMP");
}

extern "C" void ble_start(void) {
    if (pBleClient == nullptr || !pBleClient->isConnected() || pRxCharacteristic == nullptr) {
        Serial.println("[BLE] Not connected");
        return;
    }
    pRxCharacteristic->writeValue("START");
}

extern "C" void ble_stop(void) {
    if (pBleClient == nullptr || !pBleClient->isConnected() || pRxCharacteristic == nullptr) {
        Serial.println("[BLE] Not connected");
        return;
    }
    pRxCharacteristic->writeValue("STOP");
}

extern "C" void ble_open_settings(void) {
    if (lv_scr_act() != ui_Setting) {
        _ui_screen_change(&ui_Setting, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, &ui_Setting_screen_init);
    }
    if (pBleClient != nullptr && pBleClient->isConnected() && pRxCharacteristic != nullptr) {
        pRxCharacteristic->writeValue("GET");
    }
}

extern "C" void ble_back_to_measure(void) {
    if (lv_scr_act() != ui_Mesure) {
        _ui_screen_change(&ui_Mesure, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, &ui_Mesure_screen_init);
    }
}

extern "C" void ble_disconnect(void) {
    if (pBleClient != nullptr && pBleClient->isConnected()) {
        pBleClient->disconnect();
    }
    g_hasSelected = false;
    if (lv_scr_act() != ui_Initial) {
        _ui_screen_change(&ui_Initial, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, &ui_Initial_screen_init);
    }
}

// ===== 電源OFF処理 (AVLファーム perform_safe_poweroff() を移植) =====
static atomic<bool> g_poweroff_request{false};
static atomic<bool> g_poweroff_in_progress{false};
static bool g_sensor_vp_high = false;
static lv_obj_t* g_poweroff_mbox = nullptr;

// 電源OFFをブロックすべき状況があれば true を返す（今後、計測中フラグ等を追加）
static inline bool monita_busy() {
    return false;
}

static void show_measuring_block_dialog() {
    static const char * btns[] = { "OK", "" };
    lv_obj_t* mbox = lv_msgbox_create(NULL, "電源OFF不可", "動作中は電源を切れません。", btns, true);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, [](lv_event_t * e){
        lv_obj_t * m = lv_event_get_current_target(e);
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_DELETE) {
            lv_msgbox_close(m);
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);
}

static void poweroff_mbox_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * mbox = lv_event_get_current_target(e);
        const char * txt = lv_msgbox_get_active_btn_text(mbox);
        if (txt != nullptr && strcmp(txt, "はい") == 0) {
            if (monita_busy()) {
                show_measuring_block_dialog();
            } else {
                g_poweroff_request = true;
            }
        }
        lv_msgbox_close(mbox);
    } else if (code == LV_EVENT_DELETE) {
        g_poweroff_mbox = nullptr;
        g_sensor_vp_high = false; // 次回また検出できるようにリセット
    }
}

static void show_poweroff_dialog() {
    if (g_poweroff_mbox != nullptr) return;

    static const char * btns[] = { "はい", "いいえ", "" };
    g_poweroff_mbox = lv_msgbox_create(NULL, "電源OFF", "電源を切りますか？", btns, true);
    lv_obj_center(g_poweroff_mbox);
    lv_obj_add_event_cb(g_poweroff_mbox, poweroff_mbox_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(g_poweroff_mbox, poweroff_mbox_event_cb, LV_EVENT_DELETE, NULL);
}

static void perform_safe_poweroff() {
    if (g_poweroff_in_progress.exchange(true)) return;

    Serial.println("[POW] start safe shutdown");

    if (pBleClient && pBleClient->isConnected()) {
        pBleClient->disconnect();
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    Serial.println("[POW] power OFF");
    pinMode(sw_off, OUTPUT);
    digitalWrite(sw_off, LOW);
}

// ui_task相当（loop内）から毎回呼ぶ: 電源ボタン検出＋OFF要求の実行
static void poweroff_poll() {
    int raw = analogRead(Sensor_VP);
    float voltage = (float)raw / 4095.0f * 3.1f;

    if (voltage <= 1.4f) {
        if (!g_sensor_vp_high) {
            g_sensor_vp_high = true;
            if (monita_busy()) {
                show_measuring_block_dialog();
            } else {
                show_poweroff_dialog();
            }
        }
    } else if (voltage >= 1.5f) {
        g_sensor_vp_high = false;
    }

    if (g_poweroff_request.load() && !g_poweroff_in_progress.load()) {
        g_poweroff_request = false;
        perform_safe_poweroff();
    }
}

void setup()
{
    Serial.begin(115200);

    pinMode(TFT_CS, OUTPUT);
    pinMode(XPT2046_CS, OUTPUT);
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(TFT_CS, HIGH);
    digitalWrite(XPT2046_CS, HIGH);
    digitalWrite(SD_CS_PIN, HIGH);

    pinMode(Sensor_VP, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(Sensor_VP, ADC_11db);

    SPI.begin(18, 19, 23);
    g_sd_ok = SD.begin(SD_CS_PIN);
    Serial.println(g_sd_ok ? "[OK] SD card" : "[WARN] SD card not found");

    lv_init();

    tft.begin();
    tft.setRotation(1);

    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, screenWidth * 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    touchscreen.begin();
    touchscreen.setRotation(3);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    ensure_touch_calibrated();
    lv_obj_clean(lv_scr_act());

    ui_init();

    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setMTU(247);  // ver1.1側のDUMP転送に合わせて大きめのMTUを希望
    pBleScan = NimBLEDevice::getScan();
    pBleScan->setAdvertisedDeviceCallbacks(new MonitaAdvertisedDeviceCallbacks());
    pBleScan->setInterval(160);
    pBleScan->setWindow(15);
    pBleScan->setActiveScan(false);   // MSDは主アドバタイズパケットに載っているのでパッシブで十分
    pBleScan->start(0, nullptr, false);  // 継続的にバックグラウンドでスキャンし続ける

    Serial.println("Setup done");
}

void loop()
{
    lv_timer_handler();
    poweroff_poll();
    deviceListAutoRefresh();
    measureScreenAutoRefresh();
    dumpWriteIfReady();
    delay(5);
}
