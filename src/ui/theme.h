#pragma once
/*
 * 主题：全局配色 / 字号 / 间距，及通用控件构造助手。
 * 尺寸以 320x240 为基准，经 ui_layout 按屏幕高度等比放大（见 ui_layout.h）。
 */
#include "lvgl.h"
#include "ui_layout.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 调色板 */
#define THEME_COL_BG        0x12151C   /* 应用背景 */
#define THEME_COL_SURFACE   0x1E232E   /* 卡片/面板 */
#define THEME_COL_SURFACE2  0x262C3A   /* 次级表面（行、内嵌块） */
#define THEME_COL_ACCENT    0x2D9CDB   /* 主强调色 */
#define THEME_COL_EXTRUDER  0xE8734A   /* 喷嘴橙 */
#define THEME_COL_BED       0x4A90E2   /* 热床蓝 */
#define THEME_COL_OK        0x27AE60
#define THEME_COL_WARN      0xE2B93B
#define THEME_COL_ERROR     0xE74C3C
#define THEME_COL_TEXT      0xE8EAED
#define THEME_COL_TEXT_DIM  0x8B93A1

/* 尺寸（320x240 基准值，经 ui_px 缩放） */
#define THEME_TITLEBAR_H    ui_px(28)
#define THEME_RADIUS_CARD   ui_px(8)
#define THEME_RADIUS_BTN    ui_px(6)
#define THEME_PAD           ui_px(8)
#define THEME_GAP           ui_px(6)

/* 字体：CJK 子集（simhei，ASCII+界面用字）；大号数字用 Montserrat；符号图标用 Montserrat（内含 LV_SYMBOL 字形）。
   字号随屏幕缩放档位切换（见 ui_layout.c） */
#define THEME_FONT_S    ui_font_s()
#define THEME_FONT_M    ui_font_m()
#define THEME_FONT_L    ui_font_l()     /* 仅数字/ASCII */
#define THEME_FONT_XL   ui_font_xl()    /* 仅数字/ASCII */
#define THEME_FONT_ICON ui_font_icon()  /* LV_SYMBOL_* 图标 */

lv_color_t theme_col(uint32_t hex);

/* 卡片容器（无边框圆角深色块） */
lv_obj_t *theme_card(lv_obj_t *parent);

/* 标准按钮：圆角 + 按压缩放反馈；icon 为 LV_SYMBOL_*（可 NULL），text 为 CJK 文本（可 NULL） */
lv_obj_t *theme_button(lv_obj_t *parent, const char *icon, const char *text, int accent);

/* 图标按钮卡片（主菜单用）：图标在上，文字在下 */
lv_obj_t *theme_menu_button(lv_obj_t *parent, const char *icon, const char *text);

/* 文本标签 */
lv_obj_t *theme_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t col_hex);

/* A8 图片图标（assets/icons.h），recolor 着色 */
lv_obj_t *theme_img(lv_obj_t *parent, const lv_image_dsc_t *src, uint32_t col_hex);

/* 图标按钮卡片（主菜单用）：A8 图片图标在上，文字在下 */
lv_obj_t *theme_menu_button_img(lv_obj_t *parent, const lv_image_dsc_t *icon, const char *text);

/* 浮点格式化（避免 %f：内置 lv_snprintf 不支持，newlib-nano 也会因此膨胀）
 * decimals 仅支持 0 或 1。例: theme_fmt_float(buf, n, 24.56f, 1) -> "24.6" */
void theme_fmt_float(char *buf, size_t n, float v, int decimals);

#ifdef __cplusplus
}
#endif
