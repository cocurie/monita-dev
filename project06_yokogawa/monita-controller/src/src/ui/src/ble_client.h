#ifndef BLE_CLIENT_H
#define BLE_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

// "Scan start" / "Re-scan" ボタン: スキャン実行 → ui_Dropdown1 に結果反映
void ble_scan_and_populate(void);

// "Connect" ボタン: ui_Dropdown1 で選択中のデバイスへGATT接続 → Mesure画面へ
void ble_connect_selected(void);

// "Apply" ボタン(Setting画面): Spinbox1/2/3(Interval/N/M)の値をまとめて送信
void ble_apply_settings(void);

// "Tare" ボタン: "TARE" を送信
void ble_tare(void);

// "Dump" ボタン: "DUMP" を送信し、SDログをコントローラー側SDへ受信保存
void ble_dump(void);

// "Start" / "Stop" ボタン(Mesure画面): 計測ループの再開・一時停止
void ble_start(void);
void ble_stop(void);

// "Settings" ボタン(Mesure画面→Setting画面)
void ble_open_settings(void);

// "Back" ボタン(Setting画面→Mesure画面)
void ble_back_to_measure(void);

// "Disconnect" ボタン: BLE切断してInitial画面へ戻る
void ble_disconnect(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif // BLE_CLIENT_H
