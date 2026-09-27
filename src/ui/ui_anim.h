#pragma once
/*
 * 动画助手：统一的缓动/时长约定，禁止业务代码散落魔法数字。
 */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 全局动画时长约定 */
#define UI_ANIM_FAST    150   /* 按钮反馈 */
#define UI_ANIM_NORMAL  250   /* 面板转场、弹层 */
#define UI_ANIM_SLOW    700   /* 数据补间 */

/* 数值补间到任意 exec 回调（温度、百分比等） */
void ui_anim_to(void *var, lv_anim_exec_xcb_t cb, int32_t from, int32_t to,
                uint32_t dur, lv_anim_path_cb_t path);

/* N 毫秒后执行回调（占位动画实现） */
void ui_anim_after(uint32_t delay_ms, lv_anim_completed_cb_t cb, void *var);

/* Toast：顶部滑入 -> 停留 -> 滑出销毁 */
void ui_toast(const char *text, uint32_t accent_hex);

/* 面板转场时长/方向的统一入口（panel_mgr 使用）。
   del_prev=true 时旧屏幕在转场结束后由 LVGL 自动删除（无 PSRAM 机型不缓存子面板） */
void ui_screen_push(lv_obj_t *scr, bool del_prev);   /* 进入：左滑 */
void ui_screen_pop(lv_obj_t *scr, bool del_prev);    /* 返回：右滑 */

#ifdef __cplusplus
}
#endif
