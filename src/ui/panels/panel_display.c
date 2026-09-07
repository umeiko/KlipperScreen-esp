/*
 * 显示设置：反色 / 180° 旋转（按 BSP 能力显示）+ 背光 / 自动息屏 / 主题。
 * 反色、旋转运行时立即生效并落盘 klipperscreen.conf；主题切换与语言同理——
 * 各面板在 create 时取色一次，热切换要全量重建 UI，故落盘后渐暗重启。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "app_settings.h"
#include "bsp.h"
#include <stdio.h>
#include <string.h>

static void open_brightness(lv_event_t *e)
{
    LV_UNUSED(e);
    panel_mgr_open("brightness");
}

static void on_invert_toggle(lv_event_t *e)
{
    int en = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    bsp_disp_set_invert(en);                 /* 立即生效 */
    settings_save_display_invert(en);
}

static void on_rotate_toggle(lv_event_t *e)
{
    int en = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    bsp_disp_set_rotate180(en);              /* 立即生效（含触摸坐标翻转） */
    settings_save_display_rotate(en);
    /* 镜像翻转后 GRAM 旧内容按新寻址读出来是错乱的，必须立刻全屏重绘
       （当前屏 + layer_top 的标题栏）；否则要等到切页才恢复正常 */
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(NULL);
}

/* 息屏选项（秒）；0 = 永不 */
static const uint32_t so_values[] = { 15, 30, 60, 300, 900, 1800, 3600, 0 };
static const char    *so_labels[] = { "15秒", "30秒", "1分钟", "5分钟", "15分钟", "30分钟", "1小时", "永不" };
#define SO_COUNT (sizeof(so_values) / sizeof(so_values[0]))

static void on_screen_off_select(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    if (sel >= SO_COUNT) return;
    settings_save_screen_off((int)so_values[sel]);
    bsp_set_screen_timeout(so_values[sel]);   /* 立即生效，无需重启 */
}

/* 主题：深色/浅色。切换后存 klipperscreen.conf，渐暗到黑再重启（同语言切换） */
static const char *theme_codes[] = { "dark", "light" };

static void on_theme_select(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    if (sel > 1) return;
    char cur[8];
    settings_load_theme(cur, sizeof(cur));
    if (strcmp(cur, theme_codes[sel]) == 0) return;
    settings_save_theme(theme_codes[sel]);
    lv_refr_now(NULL);      /* 先把选中态画出来 */
    bsp_fade_out(1000);
    bsp_restart();
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);   /* 行数可能超出屏高，允许上下滚动 */

    int y = THEME_TITLEBAR_H + ui_px(4);
    const int step = ui_px(39);

    /* 反色 / 180° 旋转：仅硬件支持的板型显示（SPI 屏；RGB 屏与桌面端隐藏） */
    if (bsp_disp_can_invert()) {
        theme_row_switch(scr, "反色", y, settings_load_display_invert(), on_invert_toggle);
        y += step;
    }
    if (bsp_disp_can_rotate180()) {
        theme_row_switch(scr, "旋转 180°", y, settings_load_display_rotate(), on_rotate_toggle);
        y += step;
    }

    /* 背光：行内显示当前亮度，点击进滑杆调节 */
    char br[8];
    snprintf(br, sizeof(br), "%d%%", settings_load_brightness());
    theme_row_link(scr, "背光", br, y, open_brightness);
    y += step;

    /* 自动息屏：下拉选择超时（立即生效） */
    static char so_opts[96];   /* 按当前语言拼接选项 */
    int so_len = 0, so_sel = (int)SO_COUNT - 1;
    int cur_off = settings_load_screen_off();
    for (unsigned i = 0; i < SO_COUNT; i++) {
        so_len += snprintf(so_opts + so_len, sizeof(so_opts) - so_len, "%s%s",
                           i ? "\n" : "", TR(so_labels[i]));
        if ((uint32_t)cur_off == so_values[i]) so_sel = (int)i;
    }
    theme_row_dropdown(scr, "自动息屏", so_opts, y, so_sel, on_screen_off_select, NULL);
    y += step;

    /* 主题：下拉选择深/浅色，切换后渐暗重启生效 */
    static char th_opts[32];
    int th_len = 0, th_sel = 0;
    char cur_theme[8];
    settings_load_theme(cur_theme, sizeof(cur_theme));
    for (unsigned i = 0; i < 2; i++) {
        th_len += snprintf(th_opts + th_len, sizeof(th_opts) - th_len, "%s%s",
                           i ? "\n" : "", TR(i ? "浅色" : "深色"));
        if (strcmp(cur_theme, theme_codes[i]) == 0) th_sel = (int)i;
    }
    theme_row_dropdown(scr, "主题", th_opts, y, th_sel, on_theme_select, NULL);

    return scr;
}

panel_def_t panel_display_def = {
    .name = "display", .title = "显示设置",
    .create = create,
    .on_show = NULL,
    .on_tick = NULL,
};
