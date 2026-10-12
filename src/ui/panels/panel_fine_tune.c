/* Runtime print tuning: reuse subscribed gcode_move data, never save calibration.
 * Every callback checks the active job again; an old visible button cannot send
 * a command after a disconnect, cancellation or backend switch. */
#include "bsp_caps.h"
#if BSP_HAS_FINE_TUNE
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../ui_anim.h"
#include "../assets/icons.h"
#include "printer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TUNE_Z, TUNE_SPEED, TUNE_FLOW, TUNE_COUNT };
static lv_obj_t *values[TUNE_COUNT], *actions[TUNE_COUNT][3];
static lv_obj_t *steps[2][4]; /* Z pair + one shared percentage group. */
static int selected[2] = {0, 1};
static const int percent_steps[] = {1, 5, 10, 25};
static float shown[TUNE_COUNT], expected[TUNE_COUNT];
static uint32_t sent_at[TUNE_COUNT];
static bool pending[TUNE_COUNT], leaving;

static uint32_t tune_color(int kind)
{
    return kind == TUNE_Z ? THEME_COL_EXTRUDER :
           kind == TUNE_SPEED ? THEME_COL_ACCENT : THEME_COL_OK;
}

static void back_after_job(void *unused)
{
    LV_UNUSED(unused);
    if (strcmp(panel_mgr_current(), "fine_tune") == 0) panel_mgr_back();
}

static void update_ui(void)
{
    if (!printer_can_fine_tune()) {
        for (int k = 0; k < TUNE_COUNT; k++)
            for (int a = 0; a < 3; a++) lv_obj_add_state(actions[k][a], LV_STATE_DISABLED);
        if (!leaving) { leaving = true; lv_async_call(back_after_job, NULL); }
        return;
    }
    float actual[TUNE_COUNT];
    bool z_valid = printer_homing_origin_z(&actual[TUNE_Z]) && isfinite(actual[TUNE_Z]);
    actual[TUNE_SPEED] = printer_speed_pct();
    actual[TUNE_FLOW] = printer_flow_pct();
    for (int k = 0; k < TUNE_COUNT; k++) {
        bool valid = k == TUNE_Z ? z_valid : isfinite(actual[k]) && actual[k] > 0;
        bool enabled = valid && (k != TUNE_Z || printer_homed(2));
        for (int a = 0; a < 3; a++) {
            if (enabled) lv_obj_remove_state(actions[k][a], LV_STATE_DISABLED);
            else lv_obj_add_state(actions[k][a], LV_STATE_DISABLED);
        }
        if (!valid) { lv_label_set_text(values[k], "--"); pending[k] = false; continue; }
        /* Keep successive taps cumulative while waiting for Moonraker's echo.
         * Give queued commands a short window; afterwards the printer wins,
         * including commands rejected by Klipper or edits from another client. */
        if (pending[k] && (fabsf(actual[k] - expected[k]) < 0.0005f ||
                           lv_tick_elaps(sent_at[k]) >= 3000)) pending[k] = false;
        shown[k] = pending[k] ? expected[k] : actual[k];
        char text[32];
        if (k == TUNE_Z) {
            int hundredths = (int)lroundf(shown[k] * 100);
            snprintf(text, sizeof(text), "%s%d.%02dmm", hundredths < 0 ? "-" : "",
                     abs(hundredths) / 100, abs(hundredths) % 100);
        } else snprintf(text, sizeof(text), "%d%%", (int)lroundf(shown[k]));
        if (strcmp(lv_label_get_text(values[k]), text)) lv_label_set_text(values[k], text);
    }
}

static void on_adjust(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e), k = id / 3, action = id % 3;
    if (!printer_can_fine_tune()) { update_ui(); return; }
    update_ui();
    if (lv_obj_has_state(actions[k][action], LV_STATE_DISABLED)) return;
    float next;
    bool sent;
    if (k == TUNE_Z) {
        float delta = selected[k] ? 0.05f : 0.01f;
        if (action == 2) delta = -delta;
        next = action == 0 ? 0 : shown[k] + delta;
        sent = action == 0 ? printer_reset_z_offset() : printer_adjust_z_offset(delta);
    } else {
        int delta = percent_steps[selected[1]] * (action == 2 ? -1 : 1);
        int percent = action == 0 ? 100 : (int)lroundf(shown[k]) + delta;
        percent = LV_CLAMP(1, percent, 1000);
        next = percent;
        sent = k == TUNE_SPEED ? printer_set_speed_pct(percent) : printer_set_flow_pct(percent);
    }
    if (sent) { expected[k] = next; sent_at[k] = lv_tick_get(); pending[k] = true; }
    else ui_toast("调整未发送", THEME_COL_ERROR);
    update_ui();
}

static void refresh_steps(int kind)
{
    int count = kind == 0 ? 2 : 4;
    for (int i = 0; i < count; i++) {
        bool active = selected[kind] == i;
        lv_obj_set_style_bg_color(steps[kind][i],
            theme_col(active ? tune_color(kind) : THEME_COL_SURFACE2), 0);
        theme_focus_bg(steps[kind][i], tune_color(kind), active ? LV_OPA_COVER : LV_OPA_30);
    }
}

static void on_step(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    selected[id / 4] = id % 4;
    refresh_steps(id / 4);
}

static lv_obj_t *button(lv_obj_t *scr, int x, int y, int w, int h)
{
    lv_obj_t *b = theme_button(scr, NULL, NULL, 0);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_pad_all(b, ui_px(2), 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

static const lv_image_dsc_t *tune_icon(int kind)
{
#if defined(KR_WIO_TERMINAL)
    if (kind == TUNE_SPEED) return &img_move; /* Reuse flash-resident icons. */
#else
    if (kind == TUNE_SPEED) return ui_icon(&img_tune_speed, &img_tune_speed_48);
#endif
    if (kind == TUNE_FLOW) return ui_icon(&img_extrude_24, &img_extrude_24_48);
    return ui_icon(&img_zcal, &img_zcal_48);
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    memset(pending, 0, sizeof(pending));
    memset(steps, 0, sizeof(steps));
    leaving = false;
#if defined(KR_WIO_TERMINAL)
    const bool portrait = false, small = false; /* Fixed 320x240 display. */
#else
    bool portrait = ui_scr_w() <= ui_scr_h();
    bool small = ui_scale() < 1;
#endif
    int gap = ui_gap(small ? 3 : 5), x0 = ui_px(8);
    int y0 = THEME_TITLEBAR_H + ui_px(5);
    int width = ui_content_w(), height = ui_scr_h() - y0 - ui_px(5);
    int step_h = ui_px(small ? 26 : 28);
    int strip_h = portrait ? 2 * step_h + gap : step_h;
    int body_h = height - strip_h - gap;
    const char *captions[] = {"Z", "速度", "流量"};
    for (int k = 0; k < TUNE_COUNT; k++) {
        int w = portrait ? width : (width - 2 * gap) / 3;
        int h = portrait ? (body_h - 2 * gap) / 3 : body_h;
        int x = portrait ? x0 : x0 + k * (w + gap);
        int y = portrait ? y0 + k * (h + gap) : y0;
        int reset_h = portrait ? h : ui_px(30);
        int control_w = portrait ? (w - 2 * gap) / 4 : w;
        for (int a = 0; a < 3; a++) {
            int bx = x, by = y, bw = w, bh = reset_h;
            if (portrait) {
                bx = a == 2 ? x : a == 1 ? x + w - control_w : x + control_w + gap;
                bw = a == 0 ? w - 2 * control_w - 2 * gap : control_w;
            } else if (a) {
                bh = (h - reset_h - 2 * gap) / 2;
                by = y + reset_h + gap + (a - 1) * (bh + gap);
            }
            lv_obj_t *b = actions[k][a] = button(scr, bx, by, bw, bh);
            lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(b, ui_px(2), 0);
            lv_obj_set_style_border_color(b, theme_col(tune_color(k)), 0);
            lv_obj_add_event_cb(b, on_adjust, LV_EVENT_CLICKED, (void *)(intptr_t)(k * 3 + a));
            if (!a) {
                values[k] = theme_label(b, "--", ui_font_value_compact(), THEME_COL_TEXT);
                lv_obj_set_width(values[k], bw - ui_px(6));
                lv_label_set_long_mode(values[k], LV_LABEL_LONG_CLIP);
                lv_obj_set_style_text_align(values[k], LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_align(values[k], LV_ALIGN_BOTTOM_MID, 0, 0);
                if (portrait) {
                    lv_obj_t *cap = theme_label(b, captions[k], THEME_FONT_S, tune_color(k));
                    lv_obj_set_width(cap, bw - ui_px(6));
                    lv_label_set_long_mode(cap, LV_LABEL_LONG_SCROLL_CIRCULAR);
                    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
                    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 0);
                } else if (!small) {
                    lv_obj_t *ic = theme_label(b, LV_SYMBOL_REFRESH, THEME_FONT_ICON, tune_color(k));
                    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
                    lv_obj_set_width(values[k], bw - ui_px(24));
                    lv_obj_align(values[k], LV_ALIGN_RIGHT_MID, 0, 0);
                } else {
                    lv_obj_center(values[k]);
                }
            } else {
                lv_obj_t *ic = theme_img(b, tune_icon(k), tune_color(k));
                char label[80];
                snprintf(label, sizeof(label), "%s %s", portrait || small ? "" : TR(captions[k]), a == 1 ? "+" : "-");
                lv_obj_t *cap = theme_label(b, label, THEME_FONT_S, THEME_COL_TEXT);
                if (bh < ui_px(48)) {
                    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
                    lv_obj_set_width(cap, bw - ui_px(small ? 24 : 32));
                    lv_obj_align(cap, LV_ALIGN_RIGHT_MID, 0, 0);
                } else {
                    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 0);
                    lv_obj_set_width(cap, bw - ui_px(4));
                    lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, 0);
                }
                lv_label_set_long_mode(cap, LV_LABEL_LONG_SCROLL_CIRCULAR);
                lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
            }
        }
    }
    for (int k = 0; k < 2; k++) {
        int count = k == 0 ? 2 : 4;
        int z_w = (width - 2 * gap) / 3;
        int w = portrait ? width : k == 0 ? z_w : width - z_w - gap;
        int x = portrait || k == 0 ? x0 : x0 + z_w + gap;
        int y = y0 + body_h + gap + (portrait ? k * (step_h + gap) : 0);
        int sw = (w - (count - 1) * ui_px(2)) / count;
        for (int i = 0; i < count; i++) {
            lv_obj_t *b = steps[k][i] = button(scr, x + i * (sw + ui_px(2)), y, sw, step_h);
            char label[12];
            if (k == TUNE_Z) snprintf(label, sizeof(label), "%s", i ? "0.05" : "0.01");
            else snprintf(label, sizeof(label), "%d%%", percent_steps[i]);
            lv_obj_t *lbl = theme_label(b, label, ui_font_value_compact(), THEME_COL_TEXT);
            lv_obj_center(lbl);
            lv_obj_add_event_cb(b, on_step, LV_EVENT_CLICKED, (void *)(intptr_t)(k * 4 + i));
        }
        refresh_steps(k);
    }
    ui_nav_group_set_spatial(lv_group_get_default(), true);
    update_ui();
    return scr;
}

panel_def_t panel_fine_tune_def = {
    .name = "fine_tune", .title = "微调", .title_s = "微调",
    .create = create, .on_show = update_ui, .on_tick = update_ui, .hide_temps = 1,
};
#endif
