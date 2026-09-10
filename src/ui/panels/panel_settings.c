/*
 * 设置：网络 / 打印机连接 / 语言 / 显示设置 + 版本
 * 背光、自动息屏、主题、反色、旋转收进"显示设置"二级菜单（panel_display）。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../assets/icons.h"
#include "app_settings.h"
#include "version.h"
#include "bsp.h"
#include <stdio.h>

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

/* 语言：下拉选择；切换后存 klipperscreen.conf，背光 1s 渐暗到黑再重启
 * （热重建 UI 在事件回调里删屏幕会踩 LVGL 对象树，不稳定，故直接重启） */
static void on_lang_select(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    if (sel >= ui_lang_count()) return;
    ui_lang_t want = (ui_lang_t)sel;    /* 下拉顺序 == langs[] 注册表顺序 == 枚举顺序 */
    if (want == ui_lang_get()) return;
    settings_save_language(ui_lang_code(want));
    lv_refr_now(NULL);      /* 先把选中态画出来 */
    bsp_fade_out(1000);     /* 当前亮度 1s 渐暗到纯黑 */
    bsp_restart();
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

    /* 语言：下拉选项按注册表动态生成（各语言母语名），切换后渐暗重启生效 */
    static char lang_opts[128];
    int lo_len = 0;
    for (unsigned i = 0; i < ui_lang_count(); i++)
        lo_len += snprintf(lang_opts + lo_len, sizeof(lang_opts) - lo_len, "%s%s",
                           i ? "\n" : "", ui_lang_name((ui_lang_t)i));
    theme_row_dropdown(scr, "语言", lang_opts, y,
                       (int)ui_lang_get(), on_lang_select, ui_icon(&img_globe_16, &img_globe_32));
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
};
