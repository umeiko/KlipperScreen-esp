/*
 * 设置：网络 / Moonraker / 语言 / 显示设置 + 版本
 * 背光、自动息屏、主题、反色、旋转收进"显示设置"二级菜单（panel_display）。
 * 列表容器效仿 WiFi 页面：flex 纵向排布 + 容器滚动，焦点移动自动滚到可见。
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

/* 展示行（主题/版本）：可聚焦（进编码器焦点组），随列表滚动可见 */
static lv_obj_t *make_show_row(lv_obj_t *parent, const char *key, const char *val)
{
    lv_obj_t *row = theme_card(parent);
    lv_obj_set_size(row, ui_content_w(), ui_px(38));
    theme_focusable(row);
    lv_obj_t *k = theme_label(row, key, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, ui_px(2), 0);
    lv_obj_t *v = theme_label(row, val, THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -ui_px(4), 0);
    return row;
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    /* 列表容器（效仿 WiFi 页面）：flex 纵向排布，内容超高时随焦点滚动 */
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, ui_content_w(), ui_scr_h() - THEME_TITLEBAR_H - ui_px(10));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(4));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, ui_px(6), 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    theme_row_link(list, "无线网络", "", 0, open_wifi);
    theme_row_link(list, "Moonraker 连接", "", 0, open_moonraker);

    /* 语言：下拉选项按注册表动态生成（各语言母语名），切换后渐暗重启生效 */
    static char lang_opts[128];
    int lo_len = 0;
    for (unsigned i = 0; i < ui_lang_count(); i++)
        lo_len += snprintf(lang_opts + lo_len, sizeof(lang_opts) - lo_len, "%s%s",
                           i ? "\n" : "", ui_lang_name((ui_lang_t)i));
    theme_row_dropdown(list, "语言", lang_opts, 0,
                       (int)ui_lang_get(), on_lang_select, ui_icon(&img_globe_16, &img_globe_32));

    theme_row_link(list, "显示设置", "", 0, open_display);

    /* 自动息屏：下拉选择超时（立即生效） */
    static char so_opts[96];   /* 按当前语言拼接选项 */
    int so_len = 0, so_sel = (int)SO_COUNT - 1;
    int cur = settings_load_screen_off();
    for (unsigned i = 0; i < SO_COUNT; i++) {
        so_len += snprintf(so_opts + so_len, sizeof(so_opts) - so_len, "%s%s",
                           i ? "\n" : "", TR(so_labels[i]));
        if ((uint32_t)cur == so_values[i]) so_sel = (int)i;
    }
    theme_row_dropdown(list, "自动息屏", so_opts, 0, so_sel, on_screen_off_select, NULL);

    /* 屏外展示行（补回）：主题 + 版本，随编码器滚动可见 */
    make_show_row(list, "主题", "Dark");
    make_show_row(list, "版本", "v" KR_VERSION);

    return scr;
}

panel_def_t panel_settings_def = {
    .name = "settings", .title = "设置",
    .create = create,
    .on_show = NULL,
    .on_tick = NULL,
};
