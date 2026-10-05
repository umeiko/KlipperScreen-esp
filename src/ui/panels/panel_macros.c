/*
 * 宏（KlipperScreen gcode_macros 对应）：
 * - ESP32：纯列表，点选哪个执行哪个（无图标、无参数输入，存储所限）。
 * - 桌面端（Win/Linux/Android）：行尾 ▶ 直接执行；✏️ 打开参数表单——
 *   每个 {params.X|default()|type_hint} 一行，数值型参数弹浮点数字键盘
 *  （widgets/keypad），其余弹文本键盘（参数来自 configfile 解析，见
 *   printer_model 的 M_mparams；表单语义与 KlipperScreen 对齐：
 *   默认值预填可改、空值省略、M/G 编号宏空格风格其余等号风格）。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../ui_anim.h"
#include "printer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static lv_obj_t *lbl_empty;

static void do_run(int idx)
{
    printer_macro_run(idx);
    char buf[96];
    snprintf(buf, sizeof(buf), TR("已发送 %s"), printer_macro_name(idx));
    ui_toast(buf, THEME_COL_ACCENT);
}

static void on_macro(lv_event_t *e)
{
    do_run((int)(intptr_t)lv_event_get_user_data(e));
}

#if !defined(ESP_PLATFORM)

#include "../widgets/keypad.h"

#define FORM_PARAM_MAX 8
static char form_values[FORM_PARAM_MAX][24];
static char form_default[FORM_PARAM_MAX][24];
static bool form_numeric[FORM_PARAM_MAX];
static char form_pname[FORM_PARAM_MAX][32];
static int form_macro_idx = -1, form_cnt;
static lv_obj_t *form_overlay;
static lv_obj_t *form_value_lbl[FORM_PARAM_MAX];
static lv_group_t *form_nav_group;

/* ---- 通用文本输入弹层（文本型参数用；写回 target 缓冲） ---- */
static lv_obj_t *input_overlay;
static lv_obj_t *ta_input;
static lv_group_t *input_nav_group;
static char *input_target;
static size_t input_cap;
static void (*input_refresh)(void);

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

static void input_confirm(void)
{
    if (!input_overlay || !input_target) { input_close(); return; }
    strncpy(input_target, lv_textarea_get_text(ta_input), input_cap - 1);
    input_target[input_cap - 1] = 0;
    void (*refresh)(void) = input_refresh;
    input_close();
    if (refresh) refresh();
}

static void on_input_ready(lv_event_t *e) { LV_UNUSED(e); input_confirm(); }
static void on_input_cancel(lv_event_t *e) { LV_UNUSED(e); input_close(); }

static void input_open(const char *title, const char *initial,
                       char *target, size_t cap, void (*refresh)(void))
{
    if (input_overlay) return;
    input_target = target;
    input_cap = cap;
    input_refresh = refresh;
    input_nav_group = ui_nav_modal_begin();
    ui_nav_modal_set_cancel(input_nav_group, input_close);

    input_overlay = lv_obj_create(lv_layer_top());
    ui_nav_attach_scope(input_overlay, input_nav_group);
    lv_obj_remove_style_all(input_overlay);
    lv_obj_set_size(input_overlay, ui_scr_w(), ui_scr_h());
    lv_obj_set_style_bg_color(input_overlay, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_bg_opa(input_overlay, LV_OPA_COVER, 0);

    lv_obj_t *lbl = theme_label(input_overlay, title, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_set_width(lbl, LV_MIN(ui_px(300), ui_content_w()));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, ui_px(8));

    ta_input = lv_textarea_create(input_overlay);
    lv_obj_set_style_text_font(ta_input, THEME_FONT_S, 0);
    lv_textarea_set_one_line(ta_input, true);
    lv_textarea_set_max_length(ta_input, 40);
    lv_textarea_set_text(ta_input, initial);
    lv_obj_add_event_cb(ta_input, on_input_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ta_input, on_input_cancel, LV_EVENT_CANCEL, NULL);
    lv_obj_set_width(ta_input, LV_MIN(ui_px(300), ui_content_w()));
    lv_obj_align(ta_input, LV_ALIGN_TOP_MID, 0, ui_px(34));

    lv_obj_t *kb = lv_keyboard_create(input_overlay);
    lv_obj_set_size(kb, ui_scr_w(), ui_px(150));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, ui_font_icon(), LV_PART_ITEMS);
    lv_keyboard_set_textarea(kb, ta_input);
    lv_obj_add_event_cb(kb, on_input_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_input_cancel, LV_EVENT_CANCEL, NULL);
    lv_group_remove_obj(ta_input);
    theme_focusable(kb);
    lv_group_focus_obj(kb);
    lv_group_set_editing(input_nav_group, true);
    ui_desktop_textarea_begin(ta_input);
}

/* ---- 参数表单 ---- */
static void form_close(void)
{
    if (!form_overlay) return;
    ui_nav_detach_scope(form_overlay);
    lv_obj_delete(form_overlay);
    form_overlay = NULL;
    ui_nav_modal_end(form_nav_group);
    form_nav_group = NULL;
    form_macro_idx = -1;
}

static void form_refresh_value(int p)
{
    if (!form_overlay || !form_value_lbl[p]) return;
    const char *v = form_values[p][0] ? form_values[p] : form_default[p];
    lv_label_set_text(form_value_lbl[p], v[0] ? v : TR("（空）"));
    lv_obj_set_style_text_color(form_value_lbl[p],
        theme_col(form_values[p][0] ? THEME_COL_TEXT : THEME_COL_TEXT_DIM), 0);
}

static int input_target_ud;

static void on_keypad_done(float value, int ok, void *ud)
{
    int p = (int)(intptr_t)ud;
    if (!ok) return;
    if (value == (float)(int)value)
        snprintf(form_values[p], sizeof(form_values[p]), "%d", (int)value);
    else
        snprintf(form_values[p], sizeof(form_values[p]), "%.3f", (double)value);
    form_refresh_value(p);
}

static void on_text_done(void)
{
    form_refresh_value(input_target_ud);
}

static void on_value_click(lv_event_t *e)
{
    int p = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_stop_bubbling(e);
    if (form_numeric[p]) {
        float cur = (float)atof(form_values[p][0] ? form_values[p] : form_default[p]);
        keypad_open(form_pname[p], cur, on_keypad_done, (void *)(intptr_t)p);
    } else {
        input_target_ud = p;
        input_open(form_pname[p], form_values[p],
                   form_values[p], sizeof(form_values[p]), on_text_done);
    }
}

static void form_send(void)
{
    int idx = form_macro_idx;
    if (idx < 0) { form_close(); return; }
    /* 空值参数 KlipperScreen 语义：整参省略（宏体 default 兜底）；
       表单预填了默认值，这里只在"用户清空过"时省略 */
    const char *vals[FORM_PARAM_MAX];
    for (int p = 0; p < FORM_PARAM_MAX; p++) vals[p] = form_values[p];
    form_close();
    printer_macro_run_with(idx, vals);
    char buf[96];
    snprintf(buf, sizeof(buf), TR("已发送 %s"), printer_macro_name(idx));
    ui_toast(buf, THEME_COL_ACCENT);
}

static void on_form_ok(lv_event_t *e) { LV_UNUSED(e); form_send(); }
static void on_form_cancel(lv_event_t *e) { LV_UNUSED(e); form_close(); }

static void form_open(int idx)
{
    if (form_overlay || idx < 0) return;
    if (printer_macro_params_loading()) {
        ui_toast(TR("正在读取参数…"), THEME_COL_TEXT_DIM);
        return;
    }
    int cnt = printer_macro_param_count(idx);
    if (cnt == 0) {
        ui_toast(TR("该宏没有参数"), THEME_COL_TEXT_DIM);
        return;
    }
    if (cnt > FORM_PARAM_MAX) cnt = FORM_PARAM_MAX;
    form_macro_idx = idx;
    form_cnt = cnt;

    form_nav_group = ui_nav_modal_begin();
    ui_nav_modal_set_cancel(form_nav_group, form_close);

    form_overlay = lv_obj_create(lv_layer_top());
    ui_nav_attach_scope(form_overlay, form_nav_group);
    lv_obj_remove_style_all(form_overlay);
    lv_obj_set_size(form_overlay, ui_scr_w(), ui_scr_h());
    lv_obj_set_style_bg_color(form_overlay, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_bg_opa(form_overlay, LV_OPA_COVER, 0);

    int ow = LV_MIN(ui_px(280), ui_content_w());
    int oh = ui_px(44) + cnt * ui_px(34) + ui_px(48);
    lv_obj_t *card = theme_card(form_overlay);
    lv_obj_set_size(card, ow, oh);
    lv_obj_center(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = theme_label(card, printer_macro_label(idx), THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_set_width(title, ow - 2 * THEME_PAD - ui_px(8));
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, ui_px(6));

    for (int p = 0; p < cnt; p++) {
        printer_macro_param_info(idx, p, form_pname[p], sizeof(form_pname[p]),
                                 form_default[p], sizeof(form_default[p]), &form_numeric[p]);
        snprintf(form_values[p], sizeof(form_values[p]), "%s", form_default[p]);

        int ry = ui_px(44) + p * ui_px(34);
        lv_obj_t *row = theme_card(card);
        lv_obj_set_size(row, ow - ui_px(16), ui_px(30));
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, ry - ui_px(6));
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = theme_label(row, form_pname[p], THEME_FONT_S, THEME_COL_TEXT);
        lv_obj_set_width(name, (ow - ui_px(16)) / 2 - ui_px(4));
        lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, ui_px(4), 0);

        lv_obj_t *val = theme_label(row, "", THEME_FONT_S, THEME_COL_ACCENT);
        form_value_lbl[p] = val;
        lv_obj_set_width(val, (ow - ui_px(16)) / 2 - ui_px(8));
        lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -ui_px(4), 0);
        lv_obj_add_flag(val, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(val, on_value_click, LV_EVENT_CLICKED, (void *)(intptr_t)p);
        theme_focusable(val);
        form_refresh_value(p);
    }

    int bw = (ow - ui_px(24)) / 2;
    int by = oh - ui_px(40);
    lv_obj_t *ok = theme_button(card, LV_SYMBOL_OK, "确定", 1);
    lv_obj_set_size(ok, bw, ui_px(32));
    lv_obj_align(ok, LV_ALIGN_TOP_LEFT, ui_px(8), by);
    lv_obj_add_event_cb(ok, on_form_ok, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel = theme_button(card, LV_SYMBOL_CLOSE, "取消", 0);
    lv_obj_set_size(cancel, bw, ui_px(32));
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -ui_px(8), by);
    lv_obj_add_event_cb(cancel, on_form_cancel, LV_EVENT_CLICKED, NULL);

    ui_nav_group_set_spatial(form_nav_group, true);
    lv_group_set_editing(form_nav_group, false);
}

static void on_edit(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    form_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void on_run_icon(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    do_run((int)(intptr_t)lv_event_get_user_data(e));
}

/* 无参数宏不显示编辑图标。参数靠 configfile 异步查询，面板打开时可能
   尚未就绪——create 先全部显示，加载完成后由 on_tick 原地隐藏无参数行 */
#define ROW_EDIT_MAX 24
static lv_obj_t *row_edit_icons[ROW_EDIT_MAX];
static bool row_icons_dirty;

static void sync_row_edit_icons(void)
{
    if (printer_macro_params_loading()) return;
    row_icons_dirty = false;
    int n = printer_macro_count();
    for (int i = 0; i < n && i < ROW_EDIT_MAX; i++) {
        if (!row_edit_icons[i]) continue;
        if (printer_macro_param_count(i) > 0)
            lv_obj_remove_flag(row_edit_icons[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(row_edit_icons[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_tick(void)
{
    if (row_icons_dirty) sync_row_edit_icons();
}
#endif /* !ESP_PLATFORM */

#if !defined(ESP_PLATFORM) && defined(KLIPPER_DESKTOP_SIMULATOR)
/* 模拟器截图用：直接打开第 idx 个宏的参数表单（main.c 的 macro-form 演示参数） */
void panel_macros_demo_open_form(void *idx)
{
    form_open((int)(intptr_t)idx);
}
#endif

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);
#if !defined(ESP_PLATFORM)
    memset(row_edit_icons, 0, sizeof(row_edit_icons));
    row_icons_dirty = true;
#endif

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

#if defined(ESP_PLATFORM)
        /* ESP32：纯列表点选执行，无图标无输入 */
        lv_obj_t *lbl = theme_label(row, printer_macro_label(i),
                                    THEME_FONT_M, THEME_COL_TEXT);
        lv_obj_set_width(lbl, ui_content_w() - 2 * THEME_PAD - ui_px(8));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, ui_px(4), 0);
#else
        lv_obj_t *lbl = theme_label(row, printer_macro_label(i),
                                    THEME_FONT_M, THEME_COL_TEXT);
        lv_obj_set_width(lbl, ui_content_w() - 2 * THEME_PAD - ui_px(62));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, ui_px(4), 0);

        lv_obj_t *edit = theme_label(row, LV_SYMBOL_EDIT, THEME_FONT_ICON, THEME_COL_ACCENT);
        lv_obj_align(edit, LV_ALIGN_RIGHT_MID, -ui_px(30), 0);
        lv_obj_add_flag(edit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(edit, on_edit, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        theme_focusable(edit);
        if (i < ROW_EDIT_MAX) row_edit_icons[i] = edit;

        lv_obj_t *run = theme_label(row, LV_SYMBOL_PLAY, THEME_FONT_ICON, THEME_COL_OK);
        lv_obj_align(run, LV_ALIGN_RIGHT_MID, -ui_px(4), 0);
        lv_obj_add_flag(run, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(run, on_run_icon, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        theme_focusable(run);
#endif
        y += step;
    }

    ui_nav_group_set_list(lv_group_get_default(), true);
#if !defined(ESP_PLATFORM)
    sync_row_edit_icons();   /* 参数已就绪则立即隐藏无参数行的图标；加载中留给 on_tick */
#endif
    return scr;
}

panel_def_t panel_macros_def = {
    .name = "macros", .title = "宏", .title_s = "宏",
    .create = create,
    .on_show = NULL,
#if defined(ESP_PLATFORM)
    .on_tick = NULL,
#else
    .on_tick = on_tick,
#endif
    .hide_temps = 1,
};
