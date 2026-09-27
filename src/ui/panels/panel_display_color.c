/* 屏幕色序：独立下一级页面，三行单选 + 本地化三色样条。 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../ui_anim.h"
#include "app_settings.h"
#include "bsp.h"
#include <stdint.h>

static lv_obj_t *rows[3], *names[3], *states[3];

static void refresh(void)
{
    if (!bsp_disp_can_color_order()) return;
    bsp_color_order_t current = bsp_disp_get_color_order();
    for (unsigned i = 0; i < 3; i++) {
        bool selected = i == (unsigned)current;
        if (selected) lv_obj_add_state(rows[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(rows[i], LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(rows[i], theme_col(selected ? THEME_COL_OK : THEME_COL_SURFACE), 0);
        theme_focus_bg(rows[i], selected ? THEME_COL_OK : THEME_COL_ACCENT,
                       selected ? LV_OPA_COVER : LV_OPA_30);
        lv_obj_set_style_text_color(names[i], theme_col(selected ? THEME_COL_BG : THEME_COL_TEXT), 0);
        lv_label_set_text(states[i], selected ? TR("当前") : "");
        lv_obj_set_style_text_color(states[i], theme_col(selected ? THEME_COL_BG : THEME_COL_TEXT_DIM), 0);
    }
}

static void on_select(lv_event_t *e)
{
    bsp_color_order_t next = (bsp_color_order_t)(uintptr_t)lv_event_get_user_data(e);
    bsp_color_order_t before = bsp_disp_get_color_order();
    if (next == before || !bsp_disp_set_color_order(next)) return;
    if (!settings_save_display_color_order(next)) {
        bsp_disp_set_color_order(before);
        ui_toast(TR("保存失败"), THEME_COL_ERROR);
    }
    refresh();
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
}

static void create_samples(lv_obj_t *scr, int y)
{
    const uint32_t colors[] = { 0xFF0000, 0x00FF00, 0x0000FF };
    const char *labels[] = { "红", "绿", "蓝" };
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, ui_content_w(), ui_px(32));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, ui_px(4), 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    for (unsigned i = 0; i < 3; i++) {
        lv_obj_t *swatch = lv_obj_create(row);
        lv_obj_remove_style_all(swatch);
        lv_obj_set_height(swatch, LV_PCT(100));
        lv_obj_set_flex_grow(swatch, 1);
        lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(colors[i]), 0);
        lv_obj_t *label = theme_label(swatch, labels[i], THEME_FONT_S, THEME_COL_TEXT);
        lv_obj_set_style_text_color(label, i == 1 ? lv_color_black() : lv_color_white(), 0);
        lv_obj_center(label);
    }
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);
    if (!bsp_disp_can_color_order()) return scr;

    const char *labels[] = { "默认", "RGB", "BGR" };
    int y = THEME_TITLEBAR_H + ui_px(4);
    for (unsigned i = 0; i < 3; i++) {
        rows[i] = theme_action_card(scr);
        lv_obj_set_size(rows[i], ui_content_w(), ui_px(38));
        lv_obj_align(rows[i], LV_ALIGN_TOP_MID, 0, y);
        lv_obj_remove_flag(rows[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(rows[i], on_select, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        names[i] = theme_label(rows[i], labels[i], THEME_FONT_M, THEME_COL_TEXT);
        lv_obj_align(names[i], LV_ALIGN_LEFT_MID, ui_px(2), 0);
        states[i] = theme_label(rows[i], "", THEME_FONT_S, THEME_COL_TEXT_DIM);
        lv_obj_align(states[i], LV_ALIGN_RIGHT_MID, -ui_px(4), 0);
        y += ui_px(39);
    }
    create_samples(scr, y + ui_px(8));
    refresh();
    lv_group_focus_obj(rows[bsp_disp_get_color_order()]);
    ui_nav_group_set_list(lv_group_get_default(), true);
    return scr;
}

panel_def_t panel_display_color_def = {
    .name = "display_color", .title = "屏幕色序", .title_s = "色序",
    .create = create, .on_show = refresh, .hide_temps = 1,
};
