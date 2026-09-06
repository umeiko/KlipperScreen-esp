/*
 * 背光亮度：滑杆实时调光，松手落盘 klipperscreen.conf（brightness=0-100）。
 * esp32 走 LEDC PWM（bsp_set_brightness），desktop 仅打印。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "app_settings.h"
#include "bsp.h"
#include <stdio.h>

static lv_obj_t *lbl_pct;

static void update_label(int pct)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(lbl_pct, buf);
}

static void on_slider_change(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int pct = lv_slider_get_value(slider);
    bsp_set_brightness(pct);            /* 实时生效 */
    update_label(pct);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED)
        settings_save_brightness(pct);  /* 松手才写文件，避免拖动着刷 flash */
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    int pct = settings_load_brightness();

    lv_obj_t *card = theme_card(scr);
    /* 卡片撑满标题栏到底边的空间（大屏不再上半截卡片、下半截空白） */
    lv_obj_set_size(card, ui_content_w(), ui_scr_h() - THEME_TITLEBAR_H - ui_px(16));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(8));

    /* 大号百分比（仅数字/ASCII，可用 XL 字体） */
    lbl_pct = theme_label(card, "", THEME_FONT_XL, THEME_COL_TEXT);
    lv_obj_align(lbl_pct, LV_ALIGN_TOP_MID, 0, ui_px(10));
    update_label(pct);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_size(slider, ui_px(264), ui_px(16));
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, ui_px(-30));
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, theme_col(THEME_COL_SURFACE2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, theme_col(THEME_COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, theme_col(THEME_COL_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, ui_px(4), LV_PART_KNOB);   /* 加粗把手方便点按 */
    lv_obj_add_event_cb(slider, on_slider_change, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, on_slider_change, LV_EVENT_RELEASED, NULL);

    return scr;
}

panel_def_t panel_brightness_def = {
    .name = "brightness", .title = "背光",
    .create = create,
    .on_show = NULL,
    .on_tick = NULL,
};
