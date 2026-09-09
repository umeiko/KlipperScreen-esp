/*
 * 设置：网络 / Moonraker / 语言 / 显示设置 + 版本
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

/* 带下拉的设置行（语言/自动息屏共用样式）；icon 非 NULL 时在文字前加小图标 */
static lv_obj_t *make_dropdown_row(lv_obj_t *scr, const char *key, const char *options,
                                   int y, int sel, lv_event_cb_t cb, const void *icon)
{
    lv_obj_t *row = theme_card(scr);
    lv_obj_set_size(row, ui_content_w(), ui_px(38));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);   /* 行内下拉拖动不卷动整页 */

    int text_x = ui_px(2);
    if (icon) {
        lv_obj_t *ic = theme_img(row, icon, THEME_COL_TEXT_DIM);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, ui_px(2), 0);
        text_x = ui_px(22);
    }
    lv_obj_t *k = theme_label(row, key, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, text_x, 0);

    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_symbol(dd, NULL);   /* CJK 字库无 LV_SYMBOL_DOWN 字形，省得显示方框 */
    lv_obj_set_size(dd, ui_px(100), ui_px(30));
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, -ui_px(2), 0);
    lv_obj_set_style_text_font(dd, THEME_FONT_S, 0);
    lv_obj_set_style_text_color(dd, theme_col(THEME_COL_TEXT), 0);
    lv_obj_set_style_bg_color(dd, theme_col(THEME_COL_SURFACE2), 0);
    lv_obj_set_style_border_width(dd, 0, 0);
    /* 下拉列表：深色底，选中项更深一档；限高 120（列表从下拉框向上展开，
       再高顶端会顶出屏幕上沿，顶部选项够不着） */
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_height(list, ui_px(120));
    lv_obj_set_style_max_height(list, ui_px(120), 0);
    lv_obj_set_style_text_font(list, THEME_FONT_S, 0);
    lv_obj_set_style_text_color(list, theme_col(THEME_COL_TEXT), 0);
    lv_obj_set_style_bg_color(list, theme_col(THEME_COL_SURFACE), 0);
    lv_obj_set_style_bg_color(list, theme_col(THEME_COL_SURFACE2), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED);
    lv_dropdown_set_selected(dd, sel);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return row;
}

static lv_obj_t *make_link_row(lv_obj_t *scr, const char *key, const char *val, int y, lv_event_cb_t cb)
{
    lv_obj_t *row = theme_card(scr);
    lv_obj_set_size(row, ui_content_w(), ui_px(38));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *k = theme_label(row, key, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, ui_px(2), 0);

    lv_obj_t *arrow = theme_label(row, LV_SYMBOL_RIGHT, THEME_FONT_ICON, THEME_COL_ACCENT);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -ui_px(4), 0);

    /* 值标签在箭头左侧 */
    lv_obj_t *v = theme_label(row, val, THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align_to(v, arrow, LV_ALIGN_OUT_LEFT_MID, -ui_px(4), 0);
    return row;
}

/* 展示行（主题/版本）：可聚焦（进编码器焦点组），随选项滚动可见 */
static lv_obj_t *make_show_row(lv_obj_t *scr, const char *key, const char *val, int y)
{
    lv_obj_t *row = theme_card(scr);
    lv_obj_set_size(row, ui_content_w(), ui_px(38));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
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
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);

    int y = THEME_TITLEBAR_H + ui_px(4);
    const int step = ui_px(39);

    theme_row_link(scr, "无线网络", "", y, open_wifi);
    y += step;
    theme_row_link(scr, "Moonraker 连接", "", y, open_moonraker);
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

    /* 自动息屏：下拉选择超时（立即生效） */
    static char so_opts[96];   /* 按当前语言拼接选项 */
    int so_len = 0, so_sel = (int)SO_COUNT - 1;
    int cur = settings_load_screen_off();
    for (unsigned i = 0; i < SO_COUNT; i++) {
        so_len += snprintf(so_opts + so_len, sizeof(so_opts) - so_len, "%s%s",
                           i ? "\n" : "", TR(so_labels[i]));
        if ((uint32_t)cur == so_values[i]) so_sel = (int)i;
    }
    make_dropdown_row(scr, "自动息屏", so_opts, THEME_TITLEBAR_H + ui_px(160), so_sel,
                      on_screen_off_select, NULL);

    /* 屏外展示行（补回）：主题 + 版本，随编码器滚动可见 */
    make_show_row(scr, "主题", "Dark", THEME_TITLEBAR_H + ui_px(199));
    make_show_row(scr, "版本", "v" KR_VERSION, THEME_TITLEBAR_H + ui_px(238));

    return scr;
}

panel_def_t panel_settings_def = {
    .name = "settings", .title = "设置",
    .create = create,
    .on_show = NULL,
    .on_tick = NULL,
};
