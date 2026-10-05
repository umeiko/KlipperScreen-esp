/*
 * 传感器状态：X/Y/Z 限位开关状态（QUERY_ENDSTOPS 经 gcode 响应回流解析，
 * 页面打开期间每 2s 自动刷新）+ 断料传感器（filament_switch/motion_sensor）
 * 的检出状态与启用开关。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../ui_anim.h"
#include "printer.h"
#include <stdio.h>

static lv_obj_t *lbl_endstop[3];
static lv_obj_t *lbl_endstop_hint;
static lv_timer_t *refresh_timer;
static uint32_t last_refresh_ms;

#define FIL_ROW_MAX 4
static lv_obj_t *fil_rows[FIL_ROW_MAX];
static lv_obj_t *lbl_fil_state[FIL_ROW_MAX];
static lv_obj_t *sw_fil[FIL_ROW_MAX];
static int fil_row_cnt;

static const char *endstop_text(int axis)
{
    switch (printer_endstop_state(axis)) {
    case 1:  return TR("触发");
    case 0:  return TR("未触发");
    default: return TR("未知");
    }
}

static uint32_t endstop_color(int axis)
{
    switch (printer_endstop_state(axis)) {
    case 1:  return THEME_COL_OK;
    case 0:  return THEME_COL_TEXT_DIM;
    default: return THEME_COL_WARN;
    }
}

static void update_view(void)
{
    for (int i = 0; i < 3; i++) {
        if (!lbl_endstop[i]) continue;
        lv_label_set_text(lbl_endstop[i], endstop_text(i));
        lv_obj_set_style_text_color(lbl_endstop[i], theme_col(endstop_color(i)), 0);
    }
    if (lbl_endstop_hint) {
        uint32_t age = printer_endstop_age_ms();
        char buf[48];
        if (age == UINT32_MAX) snprintf(buf, sizeof(buf), "%s", TR("未刷新"));
        else snprintf(buf, sizeof(buf), TR("更新于 %u 秒前"), (unsigned)(age / 1000));
        lv_label_set_text(lbl_endstop_hint, buf);
    }
    for (int i = 0; i < fil_row_cnt; i++) {
        lv_label_set_text(lbl_fil_state[i],
                          printer_filsensor_detected(i) ? TR("有料") : TR("无料"));
        lv_obj_set_style_text_color(lbl_fil_state[i],
            theme_col(printer_filsensor_detected(i) ? THEME_COL_OK : THEME_COL_WARN), 0);
        if (lv_obj_has_state(sw_fil[i], LV_STATE_CHECKED) != printer_filsensor_enabled(i))
            lv_obj_set_state(sw_fil[i], LV_STATE_CHECKED, printer_filsensor_enabled(i));
    }
}

static void refresh_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    /* 页面存活期间每 2s 查询一次限位 */
    printer_endstop_refresh();
    last_refresh_ms = lv_tick_get();
}

static void on_show(void)
{
    printer_endstop_refresh();
    last_refresh_ms = lv_tick_get();
    if (!refresh_timer)
        refresh_timer = lv_timer_create(refresh_timer_cb, 2000, NULL);
    update_view();
}

static void on_hide_destroy(lv_event_t *e)
{
    /* 页面销毁/离开时停掉自动刷新 */
    if (lv_event_get_code(e) == LV_EVENT_DELETE && refresh_timer) {
        lv_timer_delete(refresh_timer);
        refresh_timer = NULL;
    }
}

static void on_fil_toggle(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bool en = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (en == printer_filsensor_enabled(idx)) return;   /* UI 同步回写不重复发 */
    printer_filsensor_set_enabled(idx, en);
    ui_toast(en ? TR("断料检测已启用") : TR("断料检测已暂停"), THEME_COL_WARN);
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);
    lv_obj_add_event_cb(scr, on_hide_destroy, LV_EVENT_DELETE, NULL);

    int y = THEME_TITLEBAR_H + ui_px(4);
    const int step = ui_px(39);

    /* ---- 限位开关 ---- */
    lv_obj_t *hdr = theme_label(scr, TR("限位开关"), THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, y);
    y += ui_px(18);

    static const char axis_names[] = { 'X', 'Y', 'Z' };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = theme_card(scr);
        lv_obj_set_size(row, ui_content_w(), ui_px(30));
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        char title[8] = { axis_names[i], 0 };
        lv_obj_t *k = theme_label(row, title, THEME_FONT_M, THEME_COL_TEXT);
        lv_obj_align(k, LV_ALIGN_LEFT_MID, ui_px(4), 0);
        lbl_endstop[i] = theme_label(row, "", THEME_FONT_M, THEME_COL_TEXT_DIM);
        lv_obj_set_width(lbl_endstop[i], ui_px(80));
        lv_obj_set_style_text_align(lbl_endstop[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(lbl_endstop[i], LV_ALIGN_RIGHT_MID, -ui_px(4), 0);
        y += ui_px(32);
    }

    lbl_endstop_hint = theme_label(scr, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(lbl_endstop_hint, LV_ALIGN_TOP_MID, 0, y);
    y += ui_px(20);

    /* ---- 断料传感器 ---- */
    fil_row_cnt = printer_filsensor_count();
    if (fil_row_cnt > FIL_ROW_MAX) fil_row_cnt = FIL_ROW_MAX;
    if (fil_row_cnt > 0) {
        lv_obj_t *fh = theme_label(scr, TR("断料检测"), THEME_FONT_S, THEME_COL_TEXT_DIM);
        lv_obj_align(fh, LV_ALIGN_TOP_MID, 0, y);
        y += ui_px(18);
        for (int i = 0; i < fil_row_cnt; i++) {
            lv_obj_t *row = theme_card(scr);
            lv_obj_set_size(row, ui_content_w(), ui_px(34));
            lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            fil_rows[i] = row;

            lv_obj_t *name = theme_label(row, printer_filsensor_name(i),
                                         THEME_FONT_M, THEME_COL_TEXT);
            lv_obj_set_width(name, ui_px(90));
            lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, ui_px(4), 0);

            lbl_fil_state[i] = theme_label(row, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
            lv_obj_set_width(lbl_fil_state[i], ui_px(36));
            lv_obj_set_style_text_align(lbl_fil_state[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(lbl_fil_state[i], LV_ALIGN_RIGHT_MID, -ui_px(52), 0);

            sw_fil[i] = lv_switch_create(row);
            lv_obj_set_size(sw_fil[i], ui_px(36), ui_px(20));
            lv_obj_align(sw_fil[i], LV_ALIGN_RIGHT_MID, -ui_px(4), 0);
            lv_obj_set_style_bg_color(sw_fil[i], theme_col(THEME_COL_OK),
                                      LV_PART_INDICATOR | LV_STATE_CHECKED);
            /* 先同步模型状态再挂回调：初始同步不应触发一次多余的写入 */
            lv_obj_set_state(sw_fil[i], LV_STATE_CHECKED, printer_filsensor_enabled(i));
            lv_obj_add_event_cb(sw_fil[i], on_fil_toggle, LV_EVENT_VALUE_CHANGED,
                                (void *)(intptr_t)i);
            y += ui_px(36);
        }
    }

    /* 纯列表页：左 = 返回（ui_nav 白名单） */
    ui_nav_group_set_list(lv_group_get_default(), true);
    return scr;
}

panel_def_t panel_sensors_def = {
    .name = "sensors", .title = "传感器状态", .title_s = "传感",
    .create = create,
    .on_show = on_show,
    .on_tick = update_view,
    .hide_temps = 1,
};
