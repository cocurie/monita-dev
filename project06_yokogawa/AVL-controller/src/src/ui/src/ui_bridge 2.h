#ifdef __cplusplus
extern "C" {
#endif
bool get_gatt_busy(void);                 // GATT_command_receive を読む
void set_nav_to_menu_after_cmd(bool v);   // nav_to_menu_after_cmd を書く
void clear_nav_to_menu_after_cmd(void);   // 明示クリア（任意）
#ifdef __cplusplus
}
#endif