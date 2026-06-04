#include "ui.h"

// C側で一度だけ定義（初期値もここで）
bool is_measuring          = false;
bool measurment_mode       = true;   // true=Fast(MSC), false=Slow(MX2ES)
bool nav_to_menu_after_cmd = false;
bool GATT_command_receive  = false;