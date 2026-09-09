#pragma once
/*
 * 常驻标题栏（挂在 lv_layer_top，转场时保持不动）：返回键 + 标题 + 右侧实时温度。
 */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void titlebar_init(void);
lv_obj_t *titlebar_back_button(void);
void titlebar_set(const char *title, int show_back);
void titlebar_show_temps(int show);   /* 隐藏/显示右侧温度（标题长的面板用） */
void titlebar_tick(void);   /* 刷新右侧温度 */

#ifdef __cplusplus
}
#endif
