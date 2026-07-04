#ifndef CUSTOM_KEYBOARD_H
#define CUSTOM_KEYBOARD_H

#include "lvgl.h"

// 外部参照としてキー配列と制御マップを宣言
extern const char *custom_key_map[];
extern const lv_btnmatrix_ctrl_t custom_key_ctrl_map[];

// カスタムキーボード作成関数のプロトタイプ
lv_obj_t *create_custom_keyboard(lv_obj_t *parent);

#endif // CUSTOM_KEYBOARD_H