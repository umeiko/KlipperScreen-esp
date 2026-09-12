/*
 * 设置：网络 / 打印机连接 / 语言 / 显示设置 + 版本
 * 背光、自动息屏、主题、反色、旋转收进"显示设置"二级菜单（panel_display）。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "version.h"

static void open_wifi(lv_event_t *e)
{
    LV_UNUSED(e);
    panel_mgr_open("wifi");
}

static void open_moonraker(lv_event_t *e)
{
    LV_UNUSED(e);
    panel_mgr_open("moonraker");
}

static void open_display(lv_event_t *e)
{
    LV_UNUSED(e);
    panel_mgr_open("display");
}

static void open_language(lv_event_t *e)
{
    LV_UNUSED(e);
    panel_mgr_open("language");
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);

    int y = THEME_TITLEBAR_H + ui_px(4);
    const int step = ui_px(39);

    theme_row_link(scr, "无线网络", "", y, open_wifi);
    y += step;
    theme_row_link(scr, "打印机连接设置", "", y, open_moonraker);
    y += step;

    theme_row_link(scr, "语言", ui_lang_name(ui_lang_get()), y, open_language);
    y += step;

    theme_row_link(scr, "显示设置", "", y, open_display);
    y += step;

    theme_row(scr, "版本", KR_VERSION, y);

    return scr;
}

panel_def_t panel_settings_def = {
    .name = "settings", .title = "设置",
    .create = create,
    .on_show = NULL,
    .on_tick = NULL,
    .hide_temps = 1,   /* 设置及其子菜单与打印控制无关，标题栏不显示温度 */
};
