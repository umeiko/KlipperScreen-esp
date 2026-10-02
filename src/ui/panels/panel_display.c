/*
 * 显示设置：反色 / 180° 旋转 / 水平镜像（按 BSP 能力显示）+ 背光 / 自动息屏 / 主题。
 * 反色、旋转运行时立即生效并落盘 klipperscreen.conf；主题切换与语言同理——
 * 各面板在 create 时取色一次，热切换要全量重建 UI，故落盘后渐暗重启
 *（Android 无重启概念：改主题色全局变量后异步整树重建，立即生效）。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../ui_anim.h"
#include "app_settings.h"
#include "bsp.h"
#include "bsp_caps.h"
#include <stdio.h>
#include <string.h>

#if BSP_HAS_ENCODER_SETTINGS
#define ENCODER_TRIAL_MS 20000u
static lv_obj_t *encoder_dd, *encoder_overlay, *encoder_message;
static lv_group_t *encoder_group;
static lv_timer_t *encoder_timer;
static uint32_t encoder_started;
static int encoder_saved, encoder_before, encoder_candidate;
static const int encoder_values[] = { 1, 2, 4 };

static int encoder_index(int counts)
{
    for (unsigned i = 0; i < sizeof(encoder_values) / sizeof(encoder_values[0]); i++)
        if (encoder_values[i] == counts) return (int)i;
    return -1;
}

static void encoder_restore_selection(void)
{
    int index = encoder_index(encoder_saved);
    lv_dropdown_set_selected(encoder_dd, index < 0 ? 0 : index);
    /* 兼容旧预览/自编译的非标准值：如实显示当前数字，但菜单只提供三档。 */
    static char value[8];
    snprintf(value, sizeof(value), "%d", encoder_saved);
    lv_dropdown_set_text(encoder_dd, index < 0 ? value : NULL);
}

static void encoder_finish(bool keep)
{
    if (!encoder_overlay) return;
    /* 即使定时器尚未调度，也不允许在截止时间之后保存。 */
    bool expired = lv_tick_elaps(encoder_started) >= ENCODER_TRIAL_MS;
    bool failed = false;
    if (keep && !expired) {
        if (settings_save_encoder_counts(encoder_candidate))
            encoder_saved = encoder_candidate;
        else
            failed = true;
    }
    if (!keep || expired || failed)
        bsp_encoder_set_counts_per_detent(encoder_before);
    if (encoder_timer) lv_timer_delete(encoder_timer);
    encoder_timer = NULL;
    encoder_restore_selection();
    ui_nav_detach_scope(encoder_overlay);
    lv_obj_delete(encoder_overlay);
    encoder_overlay = encoder_message = NULL;
    ui_nav_modal_end(encoder_group);
    encoder_group = NULL;
    /* 不让关闭弹层的同一次按下继续激活底层控件。 */
    if (lv_indev_active()) lv_indev_wait_release(lv_indev_active());
    if (failed) ui_toast(TR("保存失败"), THEME_COL_ERROR);
}

static void encoder_cancel(void) { encoder_finish(false); }
static void encoder_keep(lv_event_t *e) { LV_UNUSED(e); encoder_finish(true); }
static void encoder_revert(lv_event_t *e) { LV_UNUSED(e); encoder_finish(false); }

static void encoder_nav_key(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_LEFT) lv_group_focus_prev(encoder_group);
    else if (key == LV_KEY_RIGHT) lv_group_focus_next(encoder_group);
}

static void encoder_update_message(void)
{
    uint32_t elapsed = lv_tick_elaps(encoder_started);
    unsigned left = elapsed >= ENCODER_TRIAL_MS ? 0 : (ENCODER_TRIAL_MS - elapsed + 999) / 1000;
    char text[240];
    snprintf(text, sizeof(text), TR("转动测试: 每格移动一项\n%u 秒内确认\n否则自动恢复"), left);
    lv_label_set_text(encoder_message, text);
}

static void encoder_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (lv_tick_elaps(encoder_started) >= ENCODER_TRIAL_MS) encoder_finish(false);
    else encoder_update_message();
}

static void encoder_owner_event(lv_event_t *e)
{
    /* 转场开始时即回退，必须早于 panel_mgr 回收本页导航组。
     * 删除事件是直接销毁屏幕的兜底，防止定时器持有已释放对象。 */
    if (!encoder_dd || lv_obj_get_screen(encoder_dd) != lv_event_get_target_obj(e)) return;
    encoder_finish(false);
    if (lv_event_get_code(e) == LV_EVENT_DELETE) encoder_dd = NULL;
}

static void on_encoder_select(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target_obj(e);
    unsigned index = lv_dropdown_get_selected(dd);
    if (index >= sizeof(encoder_values) / sizeof(encoder_values[0]) || encoder_overlay) return;
    int candidate = encoder_values[index];
    if (candidate == encoder_saved) return;
    encoder_dd = dd;
    lv_dropdown_set_text(dd, NULL);
    encoder_before = bsp_encoder_get_counts_per_detent();
    encoder_candidate = candidate;
    encoder_group = ui_nav_modal_begin();
    if (!encoder_group) {
        encoder_restore_selection();
        return;
    }
    /* 建立退路之后才修改运行参数；应用失败不修改已保存值。 */
    if (!bsp_encoder_set_counts_per_detent(candidate)) {
        ui_nav_modal_end(encoder_group);
        encoder_group = NULL;
        encoder_restore_selection();
        return;
    }
    ui_nav_modal_set_cancel(encoder_group, encoder_cancel);
    encoder_overlay = lv_obj_create(lv_layer_top());
    ui_nav_attach_scope(encoder_overlay, encoder_group);
    lv_obj_remove_style_all(encoder_overlay);
    lv_obj_set_size(encoder_overlay, ui_scr_w(), ui_scr_h());
    lv_obj_set_style_bg_color(encoder_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(encoder_overlay, LV_OPA_60, 0);
    lv_obj_add_flag(encoder_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(encoder_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = theme_card(encoder_overlay);
    int cw = LV_MIN(ui_px(288), ui_content_w());   /* 方屏（480x480）2x 换算 576px 超屏宽 */
    lv_obj_set_size(card, cw, ui_px(148));
    lv_obj_center(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    encoder_message = theme_label(card, "", THEME_FONT_S, THEME_COL_TEXT);
    lv_obj_set_width(encoder_message, cw - ui_px(24));
    lv_obj_set_style_text_align(encoder_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(encoder_message, LV_ALIGN_TOP_MID, 0, ui_px(8));

    lv_obj_t *cancel = theme_button(card, NULL, "恢复", 0);
    lv_obj_set_size(cancel, ui_px(120), ui_px(36));
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(cancel, encoder_revert, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(cancel, encoder_nav_key, LV_EVENT_KEY, NULL);
    lv_obj_t *keep = theme_button(card, NULL, "确认", 1);
    lv_obj_set_size(keep, ui_px(120), ui_px(36));
    lv_obj_align(keep, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(keep, encoder_keep, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(keep, encoder_nav_key, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(cancel);  /* 默认焦点是恢复，不是保存 */
    lv_group_set_editing(encoder_group, false);
    if (lv_indev_active()) lv_indev_wait_release(lv_indev_active());
    encoder_started = lv_tick_get();
    encoder_timer = lv_timer_create(encoder_tick, 250, NULL);
    if (!encoder_timer) { encoder_finish(false); return; }
    encoder_update_message();
}
#endif

#if BSP_HAS_BACKLIGHT
static void open_brightness(lv_event_t *e)
{
    LV_UNUSED(e);
    panel_mgr_open("brightness");
}
#endif

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

static void on_mirror_toggle(lv_event_t *e)
{
    int en = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    bsp_disp_set_mirror_x(en);               /* 立即生效（含触摸坐标翻转） */
    settings_save_display_mirror(en);
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(NULL);
}

static void open_color_order(lv_event_t *e)
{
    LV_UNUSED(e);
    panel_mgr_open("display_color");
}

#if BSP_HAS_BACKLIGHT
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
#endif

/* 主题：深色/浅色。切换后存 klipperscreen.conf，渐暗到黑再重启（同语言切换） */
static const char *theme_codes[] = { "dark", "light" };

#if defined(__ANDROID__)
/* Android 免重启：主题色是全局变量，改完异步整树重建即热切换
   （与桌面端语言切换同一机制，panel_mgr_reload 要求异步调用）。
   重建后回到本页（此时界面已是新主题）。 */
static void reload_panels_for_theme(void *ud)
{
    (void)ud;
    panel_mgr_reload();
    panel_mgr_open("settings");
    panel_mgr_open("display");
}
#endif

#if BSP_HAS_DISPLAY_ROTATION
/* 屏幕方向（桌面端软件旋转）：分辨率按交换后逻辑值重算，须重启重建布局 */
static const int rot_values[] = { 0, 90, 180, 270 };

static void on_rotation_select(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    if (sel >= sizeof(rot_values) / sizeof(rot_values[0])) return;
    if (rot_values[sel] == settings_load_display_rotation()) return;
    settings_save_display_rotation(rot_values[sel]);
    lv_refr_now(NULL);      /* 先把选中态画出来 */
    bsp_fade_out(1000);
    bsp_restart();
}
#endif

static void on_theme_select(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    if (sel > 1) return;
    char cur[8];
    settings_load_theme(cur, sizeof(cur));
    if (strcmp(cur, theme_codes[sel]) == 0) return;
    settings_save_theme(theme_codes[sel]);
#if defined(__ANDROID__)
    theme_set_dark(sel == 0);
    lv_async_call(reload_panels_for_theme, NULL);
#else
    lv_refr_now(NULL);      /* 先把选中态画出来 */
    bsp_fade_out(1000);
    bsp_restart();
#endif
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
    if (bsp_disp_can_mirror_x()) {
        theme_row_switch(scr, "水平镜像", y, settings_load_display_mirror(), on_mirror_toggle);
        y += step;
    }

#if BSP_HAS_DISPLAY_ROTATION
    /* 屏幕方向：0/90/180/270，重启后生效（布局按竖屏逻辑分辨率重建） */
    {
        int cur = settings_load_display_rotation();
        int sel = 0;
        for (unsigned i = 0; i < 4; i++)
            if (rot_values[i] == cur) sel = (int)i;
        theme_row_dropdown(scr, "屏幕方向", "0°\n90°\n180°\n270°", y, sel, on_rotation_select, NULL);
        y += step;
    }
#endif

#if BSP_HAS_BACKLIGHT
    /* 背光：行内显示当前亮度，点击进滑杆调节 */
    char br[8];
    snprintf(br, sizeof(br), "%d%%", settings_load_brightness());
    theme_row_link(scr, "背光", br, y, open_brightness);
    y += step;
#endif

    /* 用户只需常见 EC11 的 1/2/4 三档；显示当前数字，不显示“默认(4)”。 */
#if BSP_HAS_ENCODER_SETTINGS
    if (bsp_encoder_get_counts_per_detent() > 0) {
        encoder_saved = bsp_encoder_get_counts_per_detent();
        int index = encoder_index(encoder_saved);
        lv_obj_t *row = theme_row_dropdown(scr, "编码器步进", "1\n2\n4", y,
                                          index < 0 ? 0 : index, on_encoder_select, NULL);
        encoder_dd = lv_obj_get_child(row, 1); /* theme_row_dropdown 无图标时：标签、下拉框 */
        encoder_restore_selection();
        lv_obj_add_event_cb(scr, encoder_owner_event, LV_EVENT_SCREEN_UNLOAD_START, NULL);
        lv_obj_add_event_cb(scr, encoder_owner_event, LV_EVENT_DELETE, NULL);
        y += step;
    }

    /* 是否开放由后端能力决定，与编码器是否存在无关。 */
    if (bsp_disp_can_color_order()) {
        const char *names[] = { "默认", "RGB", "BGR" };
        theme_row_link(scr, "屏幕色序", names[bsp_disp_get_color_order()], y, open_color_order);
        y += step;
    }
#endif

#if BSP_HAS_BACKLIGHT
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
#endif

    /* 主题：下拉选择深/浅色，切换后渐暗重启生效（Android 动态重建免重启） */
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

    /* 纯列表页：左 = 返回、右 = 进入/确定（ui_nav 白名单） */
    ui_nav_group_set_list(lv_group_get_default(), true);
    return scr;
}

panel_def_t panel_display_def = {
    .name = "display", .title = "显示设置", .title_s = "显示",
    .create = create,
    .on_show = NULL,
    .on_tick = NULL,
    .hide_temps = 1,
};
