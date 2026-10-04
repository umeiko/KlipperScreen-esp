/*
 * 宏（KlipperScreen gcode_macros 简化版）：列出本机配置的宏，点击即执行。
 * 清单来自 objects.list（剔除 '_' 内部宏与 LOAD/UNLOAD_FILAMENT），
 * 参数化输入本期不做（KlipperScreen 有 params 表单，后续按需补）。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../ui_anim.h"
#include "printer.h"
#include <stdio.h>

static lv_obj_t *lbl_empty;

static void on_macro(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    printer_macro_run(idx);
    char buf[96];
    snprintf(buf, sizeof(buf), TR("已发送 %s"), printer_macro_name(idx));
    ui_toast(buf, THEME_COL_ACCENT);
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);

    int n = printer_macro_count();
    if (n == 0) {
        lbl_empty = theme_label(scr, TR("未发现可用宏（需已连接 Klipper）"),
                                THEME_FONT_M, THEME_COL_TEXT_DIM);
        lv_obj_align(lbl_empty, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(30));
    }

    int y = THEME_TITLEBAR_H + ui_px(4);
    const int step = ui_px(39);
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = theme_action_card(scr);
        lv_obj_set_size(row, ui_content_w(), ui_px(38));
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, on_macro, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = theme_label(row, printer_macro_label(i),
                                    THEME_FONT_M, THEME_COL_TEXT);
        lv_obj_set_width(lbl, ui_content_w() - 2 * THEME_PAD - ui_px(30));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, ui_px(4), 0);

        lv_obj_t *run = theme_label(row, LV_SYMBOL_PLAY, THEME_FONT_ICON, THEME_COL_OK);
        lv_obj_align(run, LV_ALIGN_RIGHT_MID, -ui_px(4), 0);
        y += step;
    }

    ui_nav_group_set_list(lv_group_get_default(), true);
    return scr;
}

panel_def_t panel_macros_def = {
    .name = "macros", .title = "宏", .title_s = "宏",
    .create = create,
    .on_show = NULL,
    .on_tick = NULL,
    .hide_temps = 1,
};
