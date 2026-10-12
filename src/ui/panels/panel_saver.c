/*
 * 供奉屏保延时：滑杆 1..60 秒，松手落盘 klipperscreen.conf（saver_delay=N）。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "app_settings.h"
#include "bsp.h"
#include <stdio.h>

static lv_obj_t *lbl;

static void update(int s)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d 秒", s);
    lv_label_set_text(lbl, buf);
}

static void on_slider(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    update(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED)
        settings_save_saver_delay(v);
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    int cur = settings_load_saver_delay();

    lv_obj_t *card = theme_card(scr);
    lv_obj_set_size(card, ui_content_w(), ui_scr_h() - THEME_TITLEBAR_H - ui_px(16));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(8));

    lbl = theme_label(card, "", THEME_FONT_XL, THEME_COL_TEXT);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, ui_px(10));
    update(cur);

    lv_obj_t *sl = lv_slider_create(card);
    lv_obj_set_size(sl, ui_px(264), ui_px(16));
    lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, ui_px(-30));
    lv_slider_set_range(sl, 1, 60);
    lv_slider_set_value(sl, cur, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, theme_col(THEME_COL_SURFACE2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, theme_col(THEME_COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, theme_col(THEME_COL_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, ui_px(4), LV_PART_KNOB);
    lv_obj_add_event_cb(sl, on_slider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, on_slider, LV_EVENT_RELEASED, NULL);
    theme_focusable(sl);

    return scr;
}

panel_def_t panel_saver_def = {
    .name = "saver", .title = "屏保延时",
    .create = create,
    .on_show = NULL,
    .on_tick = NULL,
};
