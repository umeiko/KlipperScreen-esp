/*
 * 菜单（主菜单"菜单"格进入）：挤出 + 传感器/宏/控制台/风扇/Z 校准入口。
 * 图标+文字行列表（KlipperScreen menu 面板的 logo+label 风格）。
 * 全部为 Klipper 专属能力；拓竹模式下主菜单"菜单"格本身禁用，进不来。
 */
#include "../theme.h"
#include "../lang.h"
#include "../assets/icons.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../ui_anim.h"

static void open_panel(lv_event_t *e)
{
    panel_mgr_open((const char *)lv_event_get_user_data(e));
}

/* 图标 + 文字 + 右箭头的菜单行（行高/步进与设置页一致） */
static lv_obj_t *menu_row(lv_obj_t *scr, const lv_image_dsc_t *icon,
                          const char *text, int y, const char *panel)
{
    lv_obj_t *row = theme_action_card(scr);
    lv_obj_set_size(row, ui_content_w(), ui_px(38));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, open_panel, LV_EVENT_CLICKED, (void *)panel);

    lv_obj_t *img = theme_img(row, icon, THEME_COL_ACCENT);
    lv_obj_align(img, LV_ALIGN_LEFT_MID, ui_px(4), 0);

    lv_obj_t *lbl = theme_label(row, text, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_set_width(lbl, ui_content_w() - 2 * THEME_PAD - ui_px(56));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, ui_px(30), 0);

    lv_obj_t *arrow = theme_label(row, LV_SYMBOL_RIGHT, THEME_FONT_ICON, THEME_COL_ACCENT);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -ui_px(4), 0);
    return row;
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);

    int y = THEME_TITLEBAR_H + ui_px(4);
    const int step = ui_px(39);
    menu_row(scr, ui_icon(&img_extrude_24, &img_extrude_24_48), TR("挤出"), y, "extrude");
    y += step;
    menu_row(scr, ui_icon(&img_sensors, &img_sensors_48), TR("传感器"), y, "sensors");
    y += step;
    menu_row(scr, ui_icon(&img_macro, &img_macro_48), TR("宏"), y, "macros");
    y += step;
    menu_row(scr, ui_icon(&img_console, &img_console_48), TR("控制台"), y, "console");
    y += step;
    menu_row(scr, ui_icon(&img_fan, &img_fan_48), TR("风扇"), y, "fan");
    y += step;
    menu_row(scr, ui_icon(&img_zcal, &img_zcal_48), TR("Z 校准"), y, "zcalibrate");

    /* 纯列表页：左 = 返回、右 = 进入/确定（ui_nav 白名单） */
    ui_nav_group_set_list(lv_group_get_default(), true);
    return scr;
}

panel_def_t panel_menu_def = {
    .name = "menu", .title = "菜单", .title_s = "菜单",
    .create = create,
    .on_show = NULL,
    .on_tick = NULL,
    .hide_temps = 1,
};
