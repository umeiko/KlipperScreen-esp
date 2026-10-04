/*
 * 控制台（KlipperScreen console 对应）：GCode 命令输入 + 回显。
 * 回显 = notify_gcode_response 全局推送 + 打开时拉取 server.gcode_store 历史；
 * 行着色：命令=accent、错误(!!)=红、警告(//)=灰、普通=正文色；温度轮询行不显示。
 * 输入弹层复用 WiFi 密码弹层的 textarea + lv_keyboard 模式。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../ui_anim.h"
#include "printer.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *list;          /* 回显滚动区 */
static lv_obj_t *lbl_last;      /* 底行：最近一条 */
static int built_cnt = -1;
static char built_tail[96];

/* ---- 命令输入弹层 ---- */
static lv_obj_t *input_overlay;
static lv_obj_t *ta_cmd;
static lv_group_t *input_nav_group;

static void input_close(void)
{
    if (!input_overlay) return;
    ui_desktop_input_end();
    ui_nav_detach_scope(input_overlay);
    lv_obj_delete(input_overlay);
    input_overlay = NULL;
    ui_nav_modal_end(input_nav_group);
    input_nav_group = NULL;
}

static void input_send(void)
{
    if (!input_overlay) return;
    char cmd[128];
    strncpy(cmd, lv_textarea_get_text(ta_cmd), sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = 0;
    input_close();
    if (cmd[0]) {
        printer_console_send(cmd);
        built_cnt = -1;   /* 触发重建（自身回显已在行内） */
    }
}

static void on_kb_ready(lv_event_t *e) { LV_UNUSED(e); input_send(); }
static void on_kb_cancel(lv_event_t *e) { LV_UNUSED(e); input_close(); }

static void open_input(lv_event_t *e)
{
    LV_UNUSED(e);
    if (input_overlay) return;
    input_nav_group = ui_nav_modal_begin();
    ui_nav_modal_set_cancel(input_nav_group, input_close);   /* 返回键 = 取消输入 */

    input_overlay = lv_obj_create(lv_layer_top());
    ui_nav_attach_scope(input_overlay, input_nav_group);
    lv_obj_remove_style_all(input_overlay);
    lv_obj_set_size(input_overlay, ui_scr_w(), ui_scr_h());
    lv_obj_set_style_bg_color(input_overlay, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_bg_opa(input_overlay, LV_OPA_COVER, 0);

    lv_obj_t *lbl = theme_label(input_overlay, TR("输入 GCode 命令"),
                                THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, ui_px(8));

    ta_cmd = lv_textarea_create(input_overlay);
    lv_obj_set_style_text_font(ta_cmd, THEME_FONT_S, 0);
    lv_textarea_set_one_line(ta_cmd, true);
    lv_textarea_set_max_length(ta_cmd, 96);
    lv_textarea_set_placeholder_text(ta_cmd, "M105 / G28 X / M104 S200 ...");
    lv_obj_add_event_cb(ta_cmd, on_kb_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ta_cmd, on_kb_cancel, LV_EVENT_CANCEL, NULL);
    lv_obj_set_width(ta_cmd, LV_MIN(ui_px(300), ui_content_w()));
    lv_obj_align(ta_cmd, LV_ALIGN_TOP_MID, 0, ui_px(34));

    lv_obj_t *kb = lv_keyboard_create(input_overlay);
    lv_obj_set_size(kb, ui_scr_w(), ui_px(150));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, ui_font_icon(), LV_PART_ITEMS);
    lv_keyboard_set_textarea(kb, ta_cmd);
    lv_obj_add_event_cb(kb, on_kb_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_kb_cancel, LV_EVENT_CANCEL, NULL);
    lv_group_remove_obj(ta_cmd);
    theme_focusable(kb);
    lv_group_focus_obj(kb);
    lv_group_set_editing(input_nav_group, true);
    ui_desktop_textarea_begin(ta_cmd);
}

static void on_clear(lv_event_t *e)
{
    LV_UNUSED(e);
    printer_console_clear();
    built_cnt = -1;
}

/* ---- 回显区 ---- */
static uint32_t kind_color(int kind)
{
    switch (kind) {
    case 1:  return THEME_COL_ACCENT;    /* 本机发出的命令 */
    case 2:  return THEME_COL_ERROR;
    case 3:  return THEME_COL_TEXT_DIM;
    default: return THEME_COL_TEXT;
    }
}

static void rebuild_view(void)
{
    int cnt = printer_console_line_count();
    int kind = 0;
    const char *tail = cnt > 0 ? printer_console_line(cnt - 1, &kind) : "";
    if (cnt == built_cnt && strcmp(tail, built_tail) == 0) return;
    built_cnt = cnt;
    snprintf(built_tail, sizeof(built_tail), "%s", tail);

    lv_obj_clean(list);
    for (int i = 0; i < cnt; i++) {
        const char *text = printer_console_line(i, &kind);
        lv_obj_t *line = lv_label_create(list);
        lv_label_set_text(line, text);
        lv_obj_set_style_text_font(line, THEME_FONT_S, 0);
        lv_obj_set_style_text_color(line, theme_col(kind_color(kind)), 0);
        lv_obj_set_width(line, lv_pct(100));
        lv_label_set_long_mode(line, LV_LABEL_LONG_WRAP);
    }
    lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
}

static void update_last(void)
{
    /* 底行始终显示最新一条（滚走历史时也能看到最新响应） */
    int cnt = printer_console_line_count();
    int kind = 0;
    const char *tail = cnt > 0 ? printer_console_line(cnt - 1, &kind) : TR("暂无回显");
    lv_label_set_text(lbl_last, tail);
    lv_obj_set_style_text_color(lbl_last, theme_col(kind_color(kind)), 0);
}

static void on_tick(void)
{
    rebuild_view();
    update_last();
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    /* 回显滚动区 */
    list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_style_bg_color(list, theme_col(THEME_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(list, THEME_RADIUS_CARD, 0);
    lv_obj_set_style_pad_hor(list, ui_px(6), 0);
    lv_obj_set_style_pad_ver(list, ui_px(4), 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    int list_h = ui_scr_h() - (THEME_TITLEBAR_H + ui_px(4)) - ui_px(58);
    lv_obj_set_size(list, ui_content_w(), list_h);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(4));

    /* 底行：最新一条回显 */
    lbl_last = theme_label(scr, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_set_width(lbl_last, ui_content_w());
    lv_label_set_long_mode(lbl_last, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(lbl_last, LV_ALIGN_BOTTOM_MID, 0, -(ui_px(40)));

    /* 底部按钮行：输入命令 + 清除 */
    int gap = ui_gap(8);
    int bw = (ui_content_w() - gap) / 2;
    lv_obj_t *btn_input = theme_button(scr, LV_SYMBOL_KEYBOARD, "输入命令", 1);
    lv_obj_set_size(btn_input, bw, ui_px(32));
    lv_obj_align(btn_input, LV_ALIGN_BOTTOM_LEFT, ui_px(8), -ui_px(4));
    lv_obj_add_event_cb(btn_input, open_input, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_clear = theme_button(scr, LV_SYMBOL_TRASH, "清除", 0);
    lv_obj_set_size(btn_clear, bw, ui_px(32));
    lv_obj_align(btn_clear, LV_ALIGN_BOTTOM_RIGHT, -ui_px(8), -ui_px(4));
    lv_obj_add_event_cb(btn_clear, on_clear, LV_EVENT_CLICKED, NULL);

    /* 网格：输入/清除左右两键 */
    ui_nav_group_set_spatial(lv_group_get_default(), true);
    return scr;
}

static void on_show(void)
{
    printer_console_load_history();   /* 补服务端历史（gcode_store） */
    built_cnt = -1;
    on_tick();
}

panel_def_t panel_console_def = {
    .name = "console", .title = "控制台", .title_s = "控制",
    .create = create,
    .on_show = on_show,
    .on_tick = on_tick,
    .hide_temps = 1,
};
