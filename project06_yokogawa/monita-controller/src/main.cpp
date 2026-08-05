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

// ===== BLEクライアント (XIAO ESP32-C3 "MonitaFlex" とのGATT通信) =====
// monita-flex-wifi 側 (Seeed XIAO ESP32-C3) が実装しているNordic UART Service。
#include "src/ui/src/ble_client.h"

static BLEUUID bleServiceUUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
static BLEUUID bleCharUUID_RX("6e400002-b5a3-f393-e0a9-e50e24dcca9e");
static BLEUUID bleCharUUID_TX("6e400003-b5a3-f393-e0a9-e50e24dcca9e");

static NimBLEScan* pBleScan = nullptr;
static NimBLEClient* pBleClient = nullptr;
static NimBLERemoteCharacteristic* pRxCharacteristic = nullptr;
static std::vector<NimBLEAdvertisedDevice*> foundDevices;
static bool bleConnected = false;

class MonitaAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) override {
        if (!advertisedDevice->haveServiceUUID() || !advertisedDevice->isAdvertisingService(bleServiceUUID)) {
            return;
        }
        for (auto* d : foundDevices) {
            if (d->getAddress().equals(advertisedDevice->getAddress())) return; // 重複除外
        }
        foundDevices.push_back(new NimBLEAdvertisedDevice(*advertisedDevice));
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
        Serial.println("[BLE] Connected to MonitaFlex");
        set_label_async(ui_Label8, "BLE: Connected");
    }
    void onDisconnect(NimBLEClient* pClient) override {
        bleConnected = false;
        Serial.println("[BLE] Disconnected from MonitaFlex");
        set_label_async(ui_Label8, "BLE: Disconnected");
    }
};

static void bleNotifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    String msg((char*)pData, length);
    Serial.print("[BLE] recv: ");
    Serial.println(msg);

    set_label_async(ui_Label8, "Last: " + msg);

    if (msg.startsWith("SLEEP=")) {
        set_label_async(ui_Label4, "Sleep: " + msg.substring(6) + " min");
    } else if (msg.startsWith("OK:SLP=")) {
        set_label_async(ui_Label4, "Sleep: " + msg.substring(7) + " min");
    }
}

extern "C" void ble_scan_and_populate(void) {
    Serial.println("[BLE] Scanning...");

    for (auto* d : foundDevices) delete d;
    foundDevices.clear();

    pBleScan->start(3, false); // 3秒間スキャン（ブロッキング）

    if (foundDevices.empty()) {
        lv_dropdown_set_options(ui_Dropdown1, "No devices found");
        Serial.println("[BLE] No devices found");
        return;
    }

    char options[512] = "";
    for (size_t i = 0; i < foundDevices.size(); i++) {
        std::string name = foundDevices[i]->getName();
        if (name.empty()) name = foundDevices[i]->getAddress().toString();
        strncat(options, name.c_str(), sizeof(options) - strlen(options) - 1);
        if (i + 1 < foundDevices.size()) {
            strncat(options, "\n", sizeof(options) - strlen(options) - 1);
        }
        Serial.printf("[BLE] %u: %s (%s)\n", (unsigned)i, name.c_str(),
                      foundDevices[i]->getAddress().toString().c_str());
    }
    lv_dropdown_set_options(ui_Dropdown1, options);
    Serial.printf("[BLE] Found %u device(s)\n", (unsigned)foundDevices.size());

    if (lv_scr_act() != ui_DviceList) {
        _ui_screen_change(&ui_DviceList, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, &ui_DviceList_screen_init);
    }
}

extern "C" void ble_connect_selected(void) {
    // すでに接続済みなら測定画面へ戻るだけ
    if (pBleClient != nullptr && pBleClient->isConnected()) {
        if (lv_scr_act() != ui_Measure) {
            _ui_screen_change(&ui_Measure, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, &ui_Measure_screen_init);
        }
        return;
    }

    uint16_t idx = lv_dropdown_get_selected(ui_Dropdown1);
    if (idx >= foundDevices.size()) {
        Serial.println("[BLE] Invalid selection index");
        return;
    }
    NimBLEAdvertisedDevice* target = foundDevices[idx];

    if (pBleClient == nullptr) {
        pBleClient = NimBLEDevice::createClient();
        pBleClient->setClientCallbacks(new MonitaClientCallbacks(), false);
    }

    std::string devName = target->getName();
    if (devName.empty()) devName = target->getAddress().toString();
    Serial.printf("[BLE] Connecting to %s (%s) ...\n",
                  devName.c_str(), target->getAddress().toString().c_str());
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

    _ui_screen_change(&ui_Measure, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, &ui_Measure_screen_init);

    if (pRxCharacteristic != nullptr) {
        pRxCharacteristic->writeValue("GET"); // 現在のスリープ間隔を取得
    }
}

extern "C" void ble_apply_sleep(void) {
    if (pBleClient == nullptr || !pBleClient->isConnected() || pRxCharacteristic == nullptr) {
        Serial.println("[BLE] Not connected");
        return;
    }
    int32_t n = lv_spinbox_get_value(ui_Spinbox1);
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "SLP:%ld", (long)n);
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

extern "C" void ble_disconnect(void) {
    if (pBleClient != nullptr && pBleClient->isConnected()) {
        pBleClient->disconnect();
    }
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
    digitalWrite(TFT_CS, HIGH);
    digitalWrite(XPT2046_CS, HIGH);

    pinMode(Sensor_VP, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(Sensor_VP, ADC_11db);

    SPI.begin(18, 19, 23);

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
    pBleScan = NimBLEDevice::getScan();
    pBleScan->setAdvertisedDeviceCallbacks(new MonitaAdvertisedDeviceCallbacks());
    pBleScan->setInterval(160);
    pBleScan->setWindow(15);
    pBleScan->setActiveScan(true);

    Serial.println("Setup done");
}

void loop()
{
    lv_timer_handler();
    poweroff_poll();
    delay(5);
}
