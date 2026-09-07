#include "theme.h"
#include "lang.h"
#include "app_settings.h"
#include <stdio.h>
#include <string.h>

/* 深/浅两套调色板；状态色（OK/WARN/ERROR/EXTRUDER/BED/ACCENT）两主题共用 */
uint32_t THEME_COL_BG        = 0x12151C;
uint32_t THEME_COL_SURFACE   = 0x1E232E;
uint32_t THEME_COL_SURFACE2  = 0x262C3A;
uint32_t THEME_COL_ACCENT    = 0x2D9CDB;
uint32_t THEME_COL_EXTRUDER  = 0xE8734A;
uint32_t THEME_COL_BED       = 0x4A90E2;
uint32_t THEME_COL_OK        = 0x27AE60;
uint32_t THEME_COL_WARN      = 0xE2B93B;
uint32_t THEME_COL_ERROR     = 0xE74C3C;
uint32_t THEME_COL_TEXT      = 0xE8EAED;
uint32_t THEME_COL_TEXT_DIM  = 0x8B93A1;

void theme_set_dark(int dark)
{
    if (dark) {
        THEME_COL_BG       = 0x12151C;
        THEME_COL_SURFACE  = 0x1E232E;
        THEME_COL_SURFACE2 = 0x262C3A;
        THEME_COL_TEXT     = 0xE8EAED;
        THEME_COL_TEXT_DIM = 0x8B93A1;
    } else {
        THEME_COL_BG       = 0xEEF1F5;
        THEME_COL_SURFACE  = 0xFFFFFF;
        THEME_COL_SURFACE2 = 0xE2E8F0;
        THEME_COL_TEXT     = 0x1A1D24;
        THEME_COL_TEXT_DIM = 0x5B6472;
    }
}

void theme_load(void)
{
    char t[8];
    settings_load_theme(t, sizeof(t));
    theme_set_dark(strcmp(t, "light") != 0);
}

void theme_fmt_float(char *buf, size_t n, float v, int decimals)
{
    int v10 = (int)(v * 10.0f + (v >= 0 ? 0.5f : -0.5f));   /* 四舍五入到 0.1 */
    if (decimals == 0)
        snprintf(buf, n, "%d", (v10 + (v10 >= 0 ? 5 : -5)) / 10);
    else
        snprintf(buf, n, "%d.%d", v10 / 10, v10 < 0 ? -(v10 % 10) : v10 % 10);
}

lv_color_t theme_col(uint32_t hex)
{
    return lv_color_hex(hex);
}

lv_obj_t *theme_card(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, theme_col(THEME_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, THEME_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(obj, THEME_PAD, 0);
    return obj;
}

lv_obj_t *theme_button(lv_obj_t *parent, const char *icon, const char *text, int accent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, theme_col(accent ? THEME_COL_ACCENT : THEME_COL_SURFACE2), 0);
    lv_obj_set_style_radius(btn, THEME_RADIUS_BTN, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    /* 按压反馈：背景半透明即可。
       不能用 transform_scale/opa —— 它们会强制 LVGL 把控件渲染进中间层缓冲，
       ESP32 堆紧张时该分配失败会让 lvgl 任务死循环（看门狗卡死 UI） */
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, LV_STATE_PRESSED);

    if ((icon && icon[0]) || (text && text[0])) {
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(btn, 4, 0);
    }
    if (icon && icon[0]) {
        lv_obj_t *ic = lv_label_create(btn);
        lv_label_set_text(ic, icon);
        lv_obj_set_style_text_font(ic, THEME_FONT_ICON, 0);
        lv_obj_set_style_text_color(ic, theme_col(THEME_COL_TEXT), 0);
    }
    if (text && text[0]) {
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, ui_tr(text));
        lv_obj_set_style_text_font(lbl, THEME_FONT_S, 0);
        lv_obj_set_style_text_color(lbl, theme_col(THEME_COL_TEXT), 0);
    }
    return btn;
}

lv_obj_t *theme_menu_button(lv_obj_t *parent, const char *icon, const char *text)
{
    lv_obj_t *btn = theme_button(parent, NULL, NULL, 0);
    lv_obj_set_style_bg_color(btn, theme_col(THEME_COL_SURFACE), 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(btn, 4, 0);

    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, THEME_FONT_L, 0);
    lv_obj_set_style_text_color(ic, theme_col(THEME_COL_ACCENT), 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, ui_tr(text));
    lv_obj_set_style_text_font(lbl, THEME_FONT_S, 0);
    lv_obj_set_style_text_color(lbl, theme_col(THEME_COL_TEXT), 0);
    return btn;
}

lv_obj_t *theme_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t col_hex)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, ui_tr(text));
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, theme_col(col_hex), 0);
    return lbl;
}

lv_obj_t *theme_img(lv_obj_t *parent, const lv_image_dsc_t *src, uint32_t col_hex)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, src);
    /* A8 只有 alpha 通道，靠 recolor 上色 */
    lv_obj_set_style_image_recolor(img, theme_col(col_hex), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    return img;
}

lv_obj_t *theme_menu_button_img(lv_obj_t *parent, const lv_image_dsc_t *icon, const char *text)
{
    lv_obj_t *btn = theme_button(parent, NULL, NULL, 0);
    lv_obj_set_style_bg_color(btn, theme_col(THEME_COL_SURFACE), 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(btn, 4, 0);

    theme_img(btn, icon, THEME_COL_ACCENT);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, ui_tr(text));
    lv_obj_set_style_text_font(lbl, THEME_FONT_S, 0);
    lv_obj_set_style_text_color(lbl, theme_col(THEME_COL_TEXT), 0);
    return btn;
}

/* ---------- 设置行（设置 / 显示设置面板共用） ---------- */

lv_obj_t *theme_row(lv_obj_t *parent, const char *key, const char *val, int y)
{
    lv_obj_t *row = theme_card(parent);
    lv_obj_set_size(row, ui_content_w(), ui_px(38));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);

    lv_obj_t *k = theme_label(row, key, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, ui_px(2), 0);

    lv_obj_t *v = theme_label(row, val, THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -ui_px(4), 0);
    return row;
}

lv_obj_t *theme_row_link(lv_obj_t *scr, const char *key, const char *val, int y, lv_event_cb_t cb)
{
    lv_obj_t *row = theme_card(scr);
    lv_obj_set_size(row, ui_content_w(), ui_px(38));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *k = theme_label(row, key, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, ui_px(2), 0);

    lv_obj_t *arrow = theme_label(row, LV_SYMBOL_RIGHT, THEME_FONT_ICON, THEME_COL_ACCENT);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -ui_px(4), 0);

    /* 值标签在箭头左侧 */
    lv_obj_t *v = theme_label(row, val, THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align_to(v, arrow, LV_ALIGN_OUT_LEFT_MID, -ui_px(4), 0);
    return row;
}

/* 带下拉的设置行（语言/自动息屏/主题共用样式）；icon 非 NULL 时在文字前加小图标 */
lv_obj_t *theme_row_dropdown(lv_obj_t *scr, const char *key, const char *options,
                             int y, int sel, lv_event_cb_t cb, const void *icon)
{
    lv_obj_t *row = theme_card(scr);
    lv_obj_set_size(row, ui_content_w(), ui_px(38));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);   /* 行内下拉拖动不卷动整页 */

    int text_x = ui_px(2);
    if (icon) {
        lv_obj_t *ic = theme_img(row, icon, THEME_COL_TEXT_DIM);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, ui_px(2), 0);
        text_x = ui_px(22);
    }
    lv_obj_t *k = theme_label(row, key, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, text_x, 0);

    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_symbol(dd, NULL);   /* CJK 字库无 LV_SYMBOL_DOWN 字形，省得显示方框 */
    lv_obj_set_size(dd, ui_px(100), ui_px(30));
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, -ui_px(2), 0);
    lv_obj_set_style_text_font(dd, THEME_FONT_S, 0);
    lv_obj_set_style_text_color(dd, theme_col(THEME_COL_TEXT), 0);
    lv_obj_set_style_bg_color(dd, theme_col(THEME_COL_SURFACE2), 0);
    lv_obj_set_style_border_width(dd, 0, 0);
    /* 下拉列表：深色底，选中项更深一档；限高 120（列表从下拉框向上展开，
       再高顶端会顶出屏幕上沿，顶部选项够不着） */
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_height(list, ui_px(120));
    lv_obj_set_style_max_height(list, ui_px(120), 0);
    lv_obj_set_style_text_font(list, THEME_FONT_S, 0);
    lv_obj_set_style_text_color(list, theme_col(THEME_COL_TEXT), 0);
    lv_obj_set_style_bg_color(list, theme_col(THEME_COL_SURFACE), 0);
    lv_obj_set_style_bg_color(list, theme_col(THEME_COL_SURFACE2), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED);
    lv_dropdown_set_selected(dd, sel);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return row;
}

/* 带开关的设置行（反色/旋转用） */
lv_obj_t *theme_row_switch(lv_obj_t *scr, const char *key, int y, int on, lv_event_cb_t cb)
{
    lv_obj_t *row = theme_card(scr);
    lv_obj_set_size(row, ui_content_w(), ui_px(38));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *k = theme_label(row, key, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, ui_px(2), 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, ui_px(44), ui_px(24));
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -ui_px(2), 0);
    lv_obj_set_style_bg_color(sw, theme_col(THEME_COL_SURFACE2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, theme_col(THEME_COL_ACCENT),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return row;
}
