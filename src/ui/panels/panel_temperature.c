/*
 * 温度控制：触摸点击仍可用 keypad；旋钮按下后在设备卡内就地调温。
 * 连续快速旋转会把步长从 1°C 逐步放大到 10°C，停顿后恢复精调。
 */
#include "../theme.h"
#include "../lang.h"
#include "../ui_anim.h"
#include "../panel_mgr.h"
#include "printer.h"
#include "../widgets/keypad.h"
#include "../assets/icons.h"
#include <stdio.h>

#define EXTRUDER_MAX_TEMP 320
#define BED_MAX_TEMP      150

static lv_obj_t *lbl_ext_cur, *lbl_ext_tgt;
static lv_obj_t *lbl_bed_cur, *lbl_bed_tgt;
static int ext_shown10 = -1, bed_shown10 = -1;   /* 0.1 度单位的显示值 */
static lv_obj_t *row_ext_obj, *row_bed_obj;
static lv_obj_t *preset_row, *readonly_hint;
static lv_obj_t *editing_row, *editing_tgt;
static lv_timer_t *commit_timer;
static int editing_value, sent_value, speed_score, step_size = 1;
static uint32_t last_step_at;

static void update_temps(void);

static void temp_anim_cb(void *obj, int32_t v10)
{
    lv_label_set_text_fmt((lv_obj_t *)obj, "%ld.%ld", (long)(v10 / 10),
                          (long)(v10 < 0 ? -(v10 % 10) : v10 % 10));
}

static void set_ext_cb(float v, int ok, void *ud)
{
    LV_UNUSED(ud);
    if (ok) {
        v = LV_CLAMP(0, v, EXTRUDER_MAX_TEMP);
        printer_set_target_ext(v);
        ui_toast(v > 0 ? "喷嘴加热中" : "喷嘴已关闭", THEME_COL_EXTRUDER);
    }
}

static void set_bed_cb(float v, int ok, void *ud)
{
    LV_UNUSED(ud);
    if (ok) {
        v = LV_CLAMP(0, v, BED_MAX_TEMP);
        printer_set_target_bed(v);
        ui_toast(v > 0 ? "热床加热中" : "热床已关闭", THEME_COL_BED);
    }
}

static int editing_is_ext(void) { return editing_row == row_ext_obj; }
static int editing_max_temp(void) { return editing_is_ext() ? EXTRUDER_MAX_TEMP : BED_MAX_TEMP; }

static void render_edit_value(void)
{
    if (!editing_tgt) return;
    lv_label_set_text_fmt(editing_tgt, "/%d" "\xC2\xB0", editing_value);
    lv_obj_set_style_text_color(editing_tgt, theme_col(THEME_COL_ERROR), 0);
}

static void send_edit_value(void)
{
    if (!editing_row || editing_value == sent_value) return;
    if (editing_is_ext()) printer_set_target_ext((float)editing_value);
    else                  printer_set_target_bed((float)editing_value);
    sent_value = editing_value;
}

static void commit_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    commit_timer = NULL;
    send_edit_value();
}

static void schedule_commit(void)
{
    if (commit_timer) lv_timer_delete(commit_timer);
    commit_timer = lv_timer_create(commit_timer_cb, 300, NULL);
    lv_timer_set_repeat_count(commit_timer, 1);
}

static void begin_edit(lv_obj_t *row)
{
    if (editing_row == row) return;
    editing_row = row;
    editing_tgt = row == row_ext_obj ? lbl_ext_tgt : lbl_bed_tgt;
    float target = row == row_ext_obj ? printer_target_ext() : printer_target_bed();
    editing_value = (int)(target + 0.5f);
    if (editing_value < 0) editing_value = 0;
    if (editing_value > editing_max_temp()) editing_value = editing_max_temp();
    sent_value = editing_value;
    speed_score = 0;
    step_size = 1;
    last_step_at = 0;
    render_edit_value();
}

static void finish_edit(void)
{
    if (!editing_row) return;
    if (commit_timer) {
        lv_timer_delete(commit_timer);
        commit_timer = NULL;
    }
    send_edit_value();
    lv_obj_set_style_text_color(editing_tgt, theme_col(THEME_COL_TEXT_DIM), 0);
    editing_row = NULL;
    editing_tgt = NULL;
    update_temps();
}

static int accelerated_step(uint32_t now)
{
    uint32_t dt = last_step_at ? lv_tick_elaps(last_step_at) : 1000;
    last_step_at = now;
    if (dt <= 90) speed_score += 3;
    else if (dt <= 170) speed_score += 2;
    else if (dt <= 280) speed_score += 1;
    else speed_score = 0;
    if (speed_score > 10) speed_score = 10;
    return speed_score >= 8 ? 10 : speed_score >= 5 ? 5 : speed_score >= 3 ? 2 : 1;
}

static void on_temp_row(lv_event_t *e)
{
    if (!printer_has_capability(PRINTER_CAP_TEMP_CONTROL)) return;
    lv_obj_t *row = lv_event_get_target_obj(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_FOCUSED) {
        lv_group_t *group = lv_obj_get_group(row);
        if (group && lv_group_get_editing(group)) begin_edit(row);
    } else if (code == LV_EVENT_KEY && editing_row == row) {
        uint32_t key = lv_event_get_key(e);
        if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
            step_size = accelerated_step(lv_tick_get());
            editing_value += key == LV_KEY_RIGHT ? step_size : -step_size;
            if (editing_value < 0) editing_value = 0;
            if (editing_value > editing_max_temp()) editing_value = editing_max_temp();
            render_edit_value();
            schedule_commit();
        } else if (key == LV_KEY_ENTER) {
            finish_edit();
            lv_group_set_editing(lv_obj_get_group(row), false);
        }
    } else if (code == LV_EVENT_DEFOCUSED && editing_row == row) {
        finish_edit();
    } else if (code == LV_EVENT_CLICKED) {
        lv_indev_t *indev = lv_event_get_indev(e);
        if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
            lv_group_t *group = lv_obj_get_group(row);
            if (editing_row == row) {
                finish_edit();
                lv_group_set_editing(group, false);
            } else {
                begin_edit(row);
                lv_group_set_editing(group, true);
            }
        } else if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            if (row == row_ext_obj)
                keypad_open("喷嘴目标温度", printer_target_ext(), set_ext_cb, NULL);
            else
                keypad_open("热床目标温度", printer_target_bed(), set_bed_cb, NULL);
        }
    }
}

static void on_preset(lv_event_t *e)
{
    if (!printer_has_capability(PRINTER_CAP_TEMP_CONTROL)) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    static const struct { float e, b; const char *name; } presets[] = {
        {210, 60, "PLA"}, {240, 80, "PETG"}, {250, 100, "ABS"}, {0, 0, "全部冷却"},
    };
    printer_set_target_ext(presets[idx].e);
    printer_set_target_bed(presets[idx].b);
    ui_toast(presets[idx].name, THEME_COL_ACCENT);
}

static lv_obj_t *make_row(lv_obj_t *parent, const char *name, uint32_t col,
                          const lv_image_dsc_t *icon, int h,
                          lv_obj_t **cur_out, lv_obj_t **tgt_out)
{
    lv_obj_t *row = theme_action_card(parent);
    lv_obj_set_size(row, ui_content_w(), h);
    lv_obj_set_style_bg_color(row, theme_col(THEME_COL_ERROR),
                              LV_STATE_FOCUS_KEY | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(row, LV_OPA_30, LV_STATE_FOCUS_KEY | LV_STATE_EDITED);
    lv_obj_set_style_outline_color(row, theme_col(THEME_COL_ERROR),
                                   LV_STATE_FOCUS_KEY | LV_STATE_EDITED);
    lv_obj_set_style_outline_width(row, ui_px(3), LV_STATE_FOCUS_KEY | LV_STATE_EDITED);
    lv_obj_add_event_cb(row, on_temp_row, LV_EVENT_ALL, NULL);

    lv_obj_t *ic = theme_img(row, icon, col);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, ui_px(4), 0);

    lv_obj_t *name_lbl = theme_label(row, name, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, ui_px(44), 0);

    lv_obj_t *cur = theme_label(row, "--", THEME_FONT_L, col);
    lv_obj_align(cur, LV_ALIGN_RIGHT_MID, -ui_px(58), 0);

    lv_obj_t *tgt = theme_label(row, "/0°", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(tgt, LV_ALIGN_RIGHT_MID, -ui_px(4), ui_px(6));

    *cur_out = cur;
    *tgt_out = tgt;
    return row;
}

static void update_temps(void)
{
    bool writable = printer_has_capability(PRINTER_CAP_TEMP_CONTROL);
    lv_obj_set_flag(preset_row, LV_OBJ_FLAG_HIDDEN, !writable);
    lv_obj_set_flag(readonly_hint, LV_OBJ_FLAG_HIDDEN, writable);
    if (writable) {
        lv_obj_remove_state(row_ext_obj, LV_STATE_DISABLED);
        lv_obj_remove_state(row_bed_obj, LV_STATE_DISABLED);
    } else {
        if (editing_row) {
            if (commit_timer) { lv_timer_delete(commit_timer); commit_timer = NULL; }
            lv_obj_set_style_text_color(editing_tgt, theme_col(THEME_COL_TEXT_DIM), 0);
            editing_row = NULL;
            editing_tgt = NULL;
        }
        lv_obj_add_state(row_ext_obj, LV_STATE_DISABLED);
        lv_obj_add_state(row_bed_obj, LV_STATE_DISABLED);
    }

    int e10 = (int)(printer_temp_ext() * 10);
    int b10 = (int)(printer_temp_bed() * 10);
    if (ext_shown10 < 0) { ext_shown10 = e10; temp_anim_cb(lbl_ext_cur, e10); }
    if (bed_shown10 < 0) { bed_shown10 = b10; temp_anim_cb(lbl_bed_cur, b10); }
    if (e10 != ext_shown10) {
        ui_anim_to(lbl_ext_cur, temp_anim_cb, ext_shown10, e10, UI_ANIM_SLOW, lv_anim_path_ease_out);
    }
    if (b10 != bed_shown10) {
        ui_anim_to(lbl_bed_cur, temp_anim_cb, bed_shown10, b10, UI_ANIM_SLOW, lv_anim_path_ease_out);
    }
    ext_shown10 = e10;
    bed_shown10 = b10;
    if (editing_tgt != lbl_ext_tgt)
        lv_label_set_text_fmt(lbl_ext_tgt, "/%d" "\xC2\xB0", (int)(printer_target_ext() + 0.5f));
    if (editing_tgt != lbl_bed_tgt)
        lv_label_set_text_fmt(lbl_bed_tgt, "/%d" "\xC2\xB0", (int)(printer_target_bed() + 0.5f));
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    /* 两张设备卡撑满标题栏与预设行之间的空间（大屏不留空带） */
    int gap = ui_gap(6);
    int y0 = THEME_TITLEBAR_H + gap;
    int reserve_bottom = ui_px(36) + ui_px(12) + gap;   /* 预设行高 + 底边距 + 间隔 */
    int card_h = (ui_scr_h() - y0 - reserve_bottom - gap) / 2;

    row_ext_obj = make_row(scr, "Extruder", THEME_COL_EXTRUDER, ui_icon(&img_nozzle_32, NULL), card_h,
                           &lbl_ext_cur, &lbl_ext_tgt);
    lv_obj_align(row_ext_obj, LV_ALIGN_TOP_MID, 0, y0);

    row_bed_obj = make_row(scr, "Heatbed", THEME_COL_BED, ui_icon(&img_bed_32, NULL), card_h,
                           &lbl_bed_cur, &lbl_bed_tgt);
    lv_obj_align(row_bed_obj, LV_ALIGN_TOP_MID, 0, y0 + card_h + gap);

    /* 预设行 */
    static const char *names[] = {"PLA", "PETG", "ABS", "冷却"};
    preset_row = lv_obj_create(scr);
    lv_obj_remove_style_all(preset_row);
    lv_obj_set_size(preset_row, ui_content_w(), ui_px(36));
    lv_obj_align(preset_row, LV_ALIGN_BOTTOM_MID, 0, -ui_px(12));
    lv_obj_set_flex_flow(preset_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(preset_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(preset_row, gap, 0);

    int pw = (ui_content_w() - 3 * gap) / 4;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = theme_button(preset_row, NULL, names[i], 0);
        lv_obj_set_size(b, pw, ui_px(34));
        lv_obj_add_event_cb(b, on_preset, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    readonly_hint = theme_label(scr, "云端监视 · 温度只读", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(readonly_hint, LV_ALIGN_BOTTOM_MID, 0, -ui_px(20));

    return scr;
}

panel_def_t panel_temperature_def = {
    .name = "temperature", .title = "温度控制", .title_s = "温度",
    .create = create,
    .on_show = update_temps,
    .on_tick = update_temps,
};
