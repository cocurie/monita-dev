#ifndef BLE_CLIENT_H
#define BLE_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

// "Scan start" / "Re-scan" ボタン: スキャン実行 → ui_Dropdown1 に結果反映
void ble_scan_and_populate(void);

// "Connect" ボタン: ui_Dropdown1 で選択中のデバイスへGATT接続
void ble_connect_selected(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif // BLE_CLIENT_H
