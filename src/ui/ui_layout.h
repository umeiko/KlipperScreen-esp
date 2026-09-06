#pragma once
/*
 * UI 布局几何抽象：以 320x240（CYD 2432S028R）为基准分辨率，
 * 大屏（如 JC8048W550 800x480）按高度等比放大（scale = 高/240），
 * 面板代码一律用 ui_px() 换算基准像素、用 ui_scr_w()/ui_content_w() 取宽，
 * 不再写死 320/240/304 等魔数。字体随缩放档位切换（字号翻倍）。
 *
 * ui_layout_init() 须在 lv_display_create 之后、任何 UI 构建之前调用
 * （ui_app_create 开头已调）。boot_anim 等更早运行的代码直接读 display 分辨率。
 */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void  ui_layout_init(void);

int   ui_scr_w(void);        /* 逻辑屏宽（= display 水平分辨率） */
int   ui_scr_h(void);        /* 逻辑屏高 */
float ui_scale(void);        /* 高度缩放比：240 基准，480 高 → 2.0 */
int   ui_px(int v);          /* 基准像素换算（四舍五入） */
int   ui_gap(int v);         /* 间距换算：次线性放大（2x 屏只放 ~1.5x），避免大屏空隙过大 */
int   ui_content_w(void);    /* 内容区宽：屏宽 - 2*ui_px(8) 边距 */

/* 字体档位：scale>=2 时切到双倍字号 */
const lv_font_t *ui_font_s(void);      /* 正文小字  14 → 28 */
const lv_font_t *ui_font_m(void);      /* 正文      16 → 32 */
const lv_font_t *ui_font_l(void);      /* 大号数字  24 → 48（仅数字/ASCII） */
const lv_font_t *ui_font_xl(void);     /* 特大数字  28 → 48（仅数字/ASCII） */
const lv_font_t *ui_font_icon(void);   /* LV_SYMBOL_* 图标 16 → 32 */

/* 图标选择：有 32px 变体时大屏用 32，否则用 16 */
const lv_image_dsc_t *ui_icon(const lv_image_dsc_t *i16, const lv_image_dsc_t *i32);

#ifdef __cplusplus
}
#endif
