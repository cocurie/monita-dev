#include "custom_keyboard.h"

// キー配列
const char *custom_key_map[] = {
    "-", "0", "1", "2", "\n",
    "3", "4", "5", "6", "\n",
    "7", "8", "9", LV_SYMBOL_BACKSPACE, ""
};

// 制御マップ
const lv_btnmatrix_ctrl_t custom_key_ctrl_map[] = {
    LV_BTNMATRIX_CTRL_NO_REPEAT, LV_BTNMATRIX_CTRL_NO_REPEAT,
    LV_BTNMATRIX_CTRL_NO_REPEAT, LV_BTNMATRIX_CTRL_NO_REPEAT, // 行1
    LV_BTNMATRIX_CTRL_NO_REPEAT, LV_BTNMATRIX_CTRL_NO_REPEAT,
    LV_BTNMATRIX_CTRL_NO_REPEAT, LV_BTNMATRIX_CTRL_NO_REPEAT, // 行2
    LV_BTNMATRIX_CTRL_NO_REPEAT, LV_BTNMATRIX_CTRL_NO_REPEAT,
    LV_BTNMATRIX_CTRL_NO_REPEAT, LV_BTNMATRIX_CTRL_NO_REPEAT  // 行3
};

// カスタムキーボード作成関数
lv_obj_t *create_custom_keyboard(lv_obj_t *parent) {
    lv_obj_t *keyboard = lv_keyboard_create(parent);

    // カスタムマップと制御マップを設定
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, custom_key_map, custom_key_ctrl_map);

    // キーボードの外観を設定
    lv_obj_set_width(keyboard, 109);
    lv_obj_set_height(keyboard, 120);
    lv_obj_align(keyboard, LV_ALIGN_CENTER, -90, -10);

    // デフォルトで非表示
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

    return keyboard;
}