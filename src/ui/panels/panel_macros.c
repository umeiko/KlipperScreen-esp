/*
 * 宏（KlipperScreen gcode_macros 简化版）：列出本机配置的宏。
 * 点按=直接执行（无参）；长按=打开预填宏名的输入框，可追加参数后发送
 *（KlipperScreen 的参数表单按 gcode 里 {params.X} 生成，工程量太大，
 *  自由文本追加已覆盖需要参数的场景）。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../ui_anim.h"
#include "printer.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *lbl_empty;

/* ---- 参数输入弹层（复用控制台 textarea+keyboard 模式） ---- */
static lv_obj_t *param_overlay;
static lv_obj_t *ta_line;
static lv_group_t *param_nav_group;

static void param_close(void)
{
    if (!param_overlay) return;
    ui_desktop_input_end();
    ui_nav_detach_scope(param_overlay);
    lv_obj_delete(param_overlay);
    param_overlay = NULL;
    ui_nav_modal_end(param_nav_group);
    param_nav_group = NULL;
}

static void param_send(void)
{
    if (!param_overlay) return;
    char line[192];
    strncpy(line, lv_textarea_get_text(ta_line), sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;
    param_close();
    if (line[0]) {
        printer_console_send(line);   /* 发送并回显到控制台历史 */
        char buf[80];
        snprintf(buf, sizeof(buf), TR("已发送 %s"), line);
        ui_toast(buf, THEME_COL_ACCENT);
    }
}

static void on_kb_ready(lv_event_t *e) { LV_UNUSED(e); param_send(); }
static void on_kb_cancel(lv_event_t *e) { LV_UNUSED(e); param_close(); }

static void open_param(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (param_overlay) return;
    param_nav_group = ui_nav_modal_begin();
    ui_nav_modal_set_cancel(param_nav_group, param_close);

    param_overlay = lv_obj_create(lv_layer_top());
    ui_nav_attach_scope(param_overlay, param_nav_group);
    lv_obj_remove_style_all(param_overlay);
    lv_obj_set_size(param_overlay, ui_scr_w(), ui_scr_h());
    lv_obj_set_style_bg_color(param_overlay, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_bg_opa(param_overlay, LV_OPA_COVER, 0);

    char title[96];
    snprintf(title, sizeof(title), TR("%s（后面可追加参数）"), printer_macro_label(idx));
    lv_obj_t *lbl = theme_label(param_overlay, title, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_set_width(lbl, LV_MIN(ui_px(300), ui_content_w()));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, ui_px(8));

    ta_line = lv_textarea_create(param_overlay);
    lv_obj_set_style_text_font(ta_line, THEME_FONT_S, 0);
    lv_textarea_set_one_line(ta_line, true);
    lv_textarea_set_max_length(ta_line, 160);
    lv_textarea_set_text(ta_line, printer_macro_name(idx));   /* 预填宏名 */
    lv_textarea_set_placeholder_text(ta_line, "NAME PARAM=value ...");
    lv_obj_add_event_cb(ta_line, on_kb_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ta_line, on_kb_cancel, LV_EVENT_CANCEL, NULL);
    lv_obj_set_width(ta_line, LV_MIN(ui_px(300), ui_content_w()));
    lv_obj_align(ta_line, LV_ALIGN_TOP_MID, 0, ui_px(34));

    lv_obj_t *kb = lv_keyboard_create(param_overlay);
    lv_obj_set_size(kb, ui_scr_w(), ui_px(150));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, ui_font_icon(), LV_PART_ITEMS);
    lv_keyboard_set_textarea(kb, ta_line);
    lv_obj_add_event_cb(kb, on_kb_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_kb_cancel, LV_EVENT_CANCEL, NULL);
    lv_group_remove_obj(ta_line);
    theme_focusable(kb);
    lv_group_focus_obj(kb);
    lv_group_set_editing(param_nav_group, true);
    ui_desktop_textarea_begin(ta_line);
}

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
        lv_obj_add_event_cb(row, open_param, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

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
