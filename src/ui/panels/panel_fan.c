/*
 * 风扇（KlipperScreen fan 对应）：fan / fan_generic 可写（滑条 0~100%，
 * 松手才发 M106/SET_FAN_SPEED），heater_fan / controller_fan 只读展示。
 * 滑条按住期间不回写（避免打断拖拽），转速以订阅推送为准。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../ui_anim.h"
#include "printer.h"
#include <stdio.h>

#define FAN_ROW_MAX 8
static lv_obj_t *slider_or_bar[FAN_ROW_MAX];
static lv_obj_t *lbl_pct[FAN_ROW_MAX];
static bool row_writable[FAN_ROW_MAX];
static int row_cnt;
static bool dragging;

static void on_fan_release(lv_event_t *e)
{
    dragging = false;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!row_writable[idx]) return;
    int v = lv_slider_get_value(lv_event_get_target(e));
    printer_fan_set(idx, v / 100.0f);
    char buf[48];
    snprintf(buf, sizeof(buf), "%s %d%%", printer_fan_name(idx), v);
    ui_toast(buf, THEME_COL_ACCENT);
}

static void on_fan_press(lv_event_t *e)
{
    LV_UNUSED(e);
    dragging = true;
}

static void update_view(void)
{
    for (int i = 0; i < row_cnt; i++) {
        float sp = printer_fan_speed(i);
        if (sp < 0) {
            lv_label_set_text(lbl_pct[i], "--");
            continue;
        }
        int pct = (int)(sp * 100 + 0.5f);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(lbl_pct[i], buf);
        if (!dragging) {
            if (row_writable[i]) lv_slider_set_value(slider_or_bar[i], pct, LV_ANIM_OFF);
            else                 lv_bar_set_value(slider_or_bar[i], pct, LV_ANIM_OFF);
        }
    }
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);

    row_cnt = printer_fan_count();
    if (row_cnt > FAN_ROW_MAX) row_cnt = FAN_ROW_MAX;
    if (row_cnt == 0) {
        lv_obj_t *lbl = theme_label(scr, TR("未发现风扇（需已连接 Klipper）"),
                                    THEME_FONT_M, THEME_COL_TEXT_DIM);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(30));
    }

    int y = THEME_TITLEBAR_H + ui_px(6);
    const int step = ui_px(44);
    for (int i = 0; i < row_cnt; i++) {
        row_writable[i] = printer_fan_writable(i);
        lv_obj_t *row = theme_card(scr);
        lv_obj_set_size(row, ui_content_w(), ui_px(40));
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = theme_label(row, printer_fan_name(i), THEME_FONT_M, THEME_COL_TEXT);
        lv_obj_set_width(name, ui_px(64));
        lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, ui_px(4), 0);

        lbl_pct[i] = theme_label(row, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
        lv_obj_set_width(lbl_pct[i], ui_px(36));
        lv_obj_set_style_text_align(lbl_pct[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(lbl_pct[i], LV_ALIGN_RIGHT_MID, -ui_px(4), 0);

        int ctrl_w = ui_content_w() - 2 * THEME_PAD - ui_px(64) - ui_px(44);
        if (row_writable[i]) {
            lv_obj_t *sl = lv_slider_create(row);
            lv_obj_set_size(sl, ctrl_w, ui_px(14));
            lv_obj_align(sl, LV_ALIGN_LEFT_MID, ui_px(68), 0);
            lv_obj_set_style_bg_color(sl, theme_col(THEME_COL_ACCENT), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(sl, theme_col(THEME_COL_ACCENT), LV_PART_KNOB);
            lv_slider_set_range(sl, 0, 100);
            lv_obj_add_event_cb(sl, on_fan_release, LV_EVENT_RELEASED, (void *)(intptr_t)i);
            lv_obj_add_event_cb(sl, on_fan_press, LV_EVENT_PRESSED, (void *)(intptr_t)i);
            theme_focusable(sl);
            slider_or_bar[i] = sl;
        } else {
            lv_obj_t *bar = lv_bar_create(row);
            lv_obj_set_size(bar, ctrl_w, ui_px(10));
            lv_obj_align(bar, LV_ALIGN_LEFT_MID, ui_px(68), 0);
            lv_obj_set_style_bg_color(bar, theme_col(THEME_COL_TEXT_DIM), LV_PART_INDICATOR);
            lv_bar_set_range(bar, 0, 100);
            slider_or_bar[i] = bar;
        }
        y += step;
    }

    /* 列表 + 滑条混排：方向键几何走位（ui_nav 白名单） */
    ui_nav_group_set_spatial(lv_group_get_default(), true);
    return scr;
}

panel_def_t panel_fan_def = {
    .name = "fan", .title = "风扇", .title_s = "风扇",
    .create = create,
    .on_show = update_view,
    .on_tick = update_view,
    .hide_temps = 1,
};
