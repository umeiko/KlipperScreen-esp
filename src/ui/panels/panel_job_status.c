/*
 * 打印状态：进度环 + 文件名 + 时间 + 控制按钮（对标 KlipperScreen job_status 简化版）
 * 打印中/暂停：暂停 / 取消 / 急停；
 * 已完成/已取消/出错：状态文案取代百分比，底部变为 重启 / 主菜单。
 */
#include "../theme.h"
#include "../lang.h"
#include "../ui_anim.h"
#include "../panel_mgr.h"
#include "printer.h"
#include "../widgets/confirm.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *arc;
static lv_obj_t *lbl_pct;
static lv_obj_t *lbl_state;      /* 完成态文案（打印完成/已取消/打印出错），居中于进度环 */
static lv_obj_t *lbl_file;
static lv_obj_t *lbl_time;
static lv_obj_t *btn_pause;
static lv_obj_t *lbl_pause_icon;
static lv_obj_t *lbl_pause_text;
static lv_obj_t *btn_cancel;
static lv_obj_t *btn_estop;
static lv_obj_t *btn_restart;    /* 完成态：重启（重打同一文件） */
static lv_obj_t *btn_home;       /* 完成态：主菜单 */
static int cur_shown_pct10 = 0;   /* 当前显示的千分比（动画起点） */
static bool job_active = false;   /* 面板生命周期内见过活动任务（区分"已取消"与纯空闲） */
static char last_file[64];        /* 取消后 print_stats.filename 可能清空，留档给重启用 */

static void arc_anim_cb(void *obj, int32_t v)
{
    lv_arc_set_value((lv_obj_t *)obj, v / 10);
    lv_label_set_text_fmt(lbl_pct, "%ld.%ld%%", (long)(v / 10), (long)(v % 10));
}

static void fmt_time(char *buf, size_t n, uint32_t s)
{
    snprintf(buf, n, "%02u:%02u:%02u", (unsigned)(s / 3600), (unsigned)((s % 3600) / 60), (unsigned)(s % 60));
}

static void update_ui(void)
{
    printer_state_t s = printer_state();
    const char *fname = printer_filename();
    if (fname[0]) {   /* 留档：取消/完成后 klippy 可能清空 filename */
        strncpy(last_file, fname, sizeof(last_file) - 1);
        last_file[sizeof(last_file) - 1] = 0;
    }
    const char *disp = fname[0] ? fname : last_file;
    lv_label_set_text(lbl_file, disp[0] ? disp : TR("空闲"));

    int printing = (s == PRINTER_STATE_PRINTING);
    int paused = (s == PRINTER_STATE_PAUSED);
    if (printing || paused) job_active = true;

    /* 完成态：明确 complete，或见过活动任务后回到 standby/error（= 已取消/出错） */
    int finished = (s == PRINTER_STATE_COMPLETE) ||
                   (job_active && (s == PRINTER_STATE_STANDBY || s == PRINTER_STATE_ERROR));

    char t1[16], t2[16], buf[48];
    fmt_time(t1, sizeof(t1), printer_print_elapsed_s());
    fmt_time(t2, sizeof(t2), printer_print_eta_s());
    snprintf(buf, sizeof(buf), TR("已用 %s\n剩余 %s"), t1, printing ? t2 : "--:--:--");
    lv_label_set_text(lbl_time, buf);

    int32_t target = printer_progress_permille();
    if (target != cur_shown_pct10) {
        ui_anim_to(arc, arc_anim_cb, cur_shown_pct10, target, UI_ANIM_SLOW, lv_anim_path_ease_out);
        cur_shown_pct10 = target;
    }

    if (finished) {
        const char *txt; uint32_t col;
        if (s == PRINTER_STATE_COMPLETE)      { txt = "打印完成"; col = THEME_COL_OK; }
        else if (s == PRINTER_STATE_ERROR)    { txt = "打印出错"; col = THEME_COL_ERROR; }
        else                                  { txt = "已取消";   col = THEME_COL_WARN; }
        lv_label_set_text(lbl_state, ui_tr(txt));
        lv_obj_set_style_text_color(lbl_state, theme_col(col), 0);
        lv_obj_add_flag(lbl_pct, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(lbl_state, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(btn_pause, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(btn_cancel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(btn_estop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(btn_restart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(btn_home, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(lbl_pct, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_state, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(btn_restart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(btn_home, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(btn_estop, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_pause_icon, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
        lv_label_set_text(lbl_pause_text, paused ? TR("继续") : TR("暂停"));
        if (printing || paused) {
            lv_obj_remove_flag(btn_pause, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(btn_cancel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(btn_pause, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(btn_cancel, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void on_pause(lv_event_t *e)
{
    LV_UNUSED(e);
    if (printer_state() == PRINTER_STATE_PRINTING) printer_print_pause();
    else                                     printer_print_resume();
    update_ui();
}

static void on_cancel(lv_event_t *e)
{
    LV_UNUSED(e);
    printer_print_cancel();
    ui_toast("已取消打印", THEME_COL_WARN);
    /* 不返回上级：留在本面板，状态回 standby 后 update_ui 切到 完成态（重启/主菜单） */
}

static void on_restart(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!last_file[0]) {
        ui_toast("没有可重启的文件", THEME_COL_ERROR);
        return;
    }
    printer_print_start(last_file);   /* 重打同一文件，状态回 printing 后界面自动切回 */
    ui_toast("开始打印", THEME_COL_OK);
}

static void on_home(lv_event_t *e)
{
    LV_UNUSED(e);
    panel_mgr_home();
}

static void do_estop(void *ud)
{
    LV_UNUSED(ud);
    printer_emergency_stop();   /* 真实实现：Moonraker printer.emergency_stop */
    ui_toast("已急停（M112）", THEME_COL_ERROR);
}

static void on_estop(lv_event_t *e)
{
    LV_UNUSED(e);
    confirm_open("确认急停？\n打印机将立即停止所有运动和加热", "急停", do_estop, NULL);
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    /* 进度环：尺寸取可用空间，垂直居中于标题栏与底部按钮之间（大屏不留空带） */
    int gap = ui_gap(8);
    int content_top = THEME_TITLEBAR_H + ui_gap(8);
    int content_bot = ui_scr_h() - ui_px(52);                 /* 按钮行高 36 + 底边距 8 + 间隔 */
    int arc_sz = content_bot - content_top - ui_gap(8);
    int arc_cap = ui_px(140);                                  /* 小屏 140，大屏 280 */
    if (arc_sz > arc_cap) arc_sz = arc_cap;
    int arc_y = content_top + (content_bot - content_top - arc_sz) / 2;

    arc = lv_arc_create(scr);
    lv_obj_set_size(arc, arc_sz, arc_sz);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);   /* 清掉构造器默认的 135°~270° 指示弧 */
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(arc, theme_col(THEME_COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, ui_px(9), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, ui_px(9), LV_PART_INDICATOR);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_align(arc, LV_ALIGN_TOP_LEFT, ui_px(14), arc_y);

    lbl_pct = theme_label(arc, "0%", THEME_FONT_L, THEME_COL_TEXT);
    lv_obj_center(lbl_pct);

    /* 右侧信息（限制在进度环右边的竖栏内，避免与圆环重叠） */
    lbl_file = theme_label(scr, "", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_set_width(lbl_file, ui_scr_w() - ui_px(14) * 2 - arc_sz - ui_px(20));
    lv_label_set_long_mode(lbl_file, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(lbl_file, LV_ALIGN_TOP_RIGHT, -ui_px(10), arc_y + arc_sz / 2 - ui_px(22));

    lbl_time = theme_label(scr, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_set_width(lbl_time, ui_scr_w() - ui_px(14) * 2 - arc_sz - ui_px(20));
    lv_obj_set_style_text_align(lbl_time, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(lbl_time, LV_ALIGN_TOP_RIGHT, -ui_px(10), arc_y + arc_sz / 2 + ui_px(6));

    /* 底部按钮：暂停 / 取消 / 急停（宽度三分内容区） */
    int bw3 = (ui_content_w() - 2 * gap) / 3;
    btn_pause = theme_button(scr, NULL, NULL, 1);
    lv_obj_set_size(btn_pause, bw3, ui_px(36));
    lv_obj_align(btn_pause, LV_ALIGN_BOTTOM_LEFT, ui_px(10), -ui_px(8));
    lv_obj_add_event_cb(btn_pause, on_pause, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(btn_pause, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_pause, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_pause, ui_px(4), 0);
    lbl_pause_icon = theme_label(btn_pause, LV_SYMBOL_PAUSE, THEME_FONT_ICON, THEME_COL_TEXT);
    lbl_pause_text = theme_label(btn_pause, "暂停", THEME_FONT_S, THEME_COL_TEXT);

    /* 取消：次级危险操作，暗底红字；急停：最高优先级，实心红 */
    btn_cancel = theme_button(scr, LV_SYMBOL_STOP, "取消", 0);
    for (int i = 0, n = lv_obj_get_child_count(btn_cancel); i < n; i++)
        lv_obj_set_style_text_color(lv_obj_get_child(btn_cancel, i), theme_col(THEME_COL_ERROR), 0);
    lv_obj_set_size(btn_cancel, bw3, ui_px(36));
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_MID, 0, -ui_px(8));
    lv_obj_add_event_cb(btn_cancel, on_cancel, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_estop_ = theme_button(scr, LV_SYMBOL_WARNING, "急停", 0);
    btn_estop = btn_estop_;
    lv_obj_set_style_bg_color(btn_estop, theme_col(THEME_COL_ERROR), 0);
    theme_focus_bg(btn_estop, THEME_COL_ERROR, LV_OPA_COVER);
    lv_obj_set_size(btn_estop, bw3, ui_px(36));
    lv_obj_align(btn_estop, LV_ALIGN_BOTTOM_RIGHT, -ui_px(10), -ui_px(8));
    lv_obj_add_event_cb(btn_estop, on_estop, LV_EVENT_CLICKED, NULL);

    /* 完成态状态文案：居中于进度环，取代百分比 */
    lbl_state = theme_label(arc, "", THEME_FONT_M, THEME_COL_OK);
    lv_obj_center(lbl_state);
    lv_obj_add_flag(lbl_state, LV_OBJ_FLAG_HIDDEN);

    /* 完成态按钮：重启（重打同一文件）/ 主菜单；初始隐藏 */
    int bw2 = (ui_content_w() - gap) / 2;
    btn_restart = theme_button(scr, LV_SYMBOL_REFRESH, "重启", 1);
    lv_obj_set_size(btn_restart, bw2, ui_px(36));
    lv_obj_align(btn_restart, LV_ALIGN_BOTTOM_LEFT, ui_px(10), -ui_px(8));
    lv_obj_add_event_cb(btn_restart, on_restart, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(btn_restart, LV_OBJ_FLAG_HIDDEN);

    btn_home = theme_button(scr, LV_SYMBOL_HOME, "主菜单", 0);
    lv_obj_set_size(btn_home, bw2, ui_px(36));
    lv_obj_align(btn_home, LV_ALIGN_BOTTOM_RIGHT, -ui_px(10), -ui_px(8));
    lv_obj_add_event_cb(btn_home, on_home, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(btn_home, LV_OBJ_FLAG_HIDDEN);

    return scr;
}

panel_def_t panel_job_status_def = {
    .name = "job_status", .title = "打印状态",
    .create = create,
    .on_show = update_ui,
    .on_tick = update_ui,
};
