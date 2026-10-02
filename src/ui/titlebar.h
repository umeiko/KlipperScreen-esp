#pragma once
/*
 * 常驻标题栏（挂在 lv_layer_top，转场时保持不动）：返回键 + 标题 + 右侧实时温度。
 */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void titlebar_init(void);
/* 分辨率变化后重建常驻标题栏：栏对象挂在 layer_top，面板重建碰不到它，
   需要显式删除重建（几何/字号才按新缩放档刷新）。重建后由 panel_mgr
   的 show/init 流程恢复标题、返回键与温度显隐。 */
void titlebar_refresh(void);
lv_obj_t *titlebar_back_button(void);
void titlebar_set(const char *title, int show_back);
void titlebar_show_temps(int show);   /* 隐藏/显示右侧温度（标题长的面板用） */
void titlebar_show_motoroff(int show);   /* 隐藏/显示右上角关闭电机按钮（移动面板用） */
lv_obj_t *titlebar_motoroff_button(void);
void titlebar_tick(void);   /* 刷新右侧温度 */

#ifdef __cplusplus
}
#endif
