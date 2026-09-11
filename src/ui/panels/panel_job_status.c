/*
 * 打印状态仪表页：左侧集中进度/状态，右侧以卡片显示任务、时间和温度。
 * 底部操作由打印机后端能力决定；云端监视模式显示明确的只读状态条。
 */
#include "../theme.h"
#include "../lang.h"
#include "../ui_anim.h"
#include "../panel_mgr.h"
#include "../assets/icons.h"
#include "printer.h"
#include "app_settings.h"
#include "../widgets/confirm.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *arc;
static lv_obj_t *lbl_pct;
static lv_obj_t *lbl_state;
static lv_obj_t *lbl_mode;
static lv_obj_t *lbl_file_cap;
static lv_obj_t *lbl_file;
static lv_obj_t *lbl_elapsed;
static lv_obj_t *lbl_remaining;
static lv_obj_t *lbl_ext;
static lv_obj_t *lbl_bed;
static lv_obj_t *btn_pause;
static lv_obj_t *lbl_pause_icon;
static lv_obj_t *lbl_pause_text;
static lv_obj_t *btn_cancel;
static lv_obj_t *btn_estop;
static lv_obj_t *btn_restart;
static lv_obj_t *btn_home;
static lv_obj_t *readonly_card;
static int cur_shown_pct10;
static bool job_active;
static char last_file[64];

static void arc_anim_cb(void *obj, int32_t value)
{
    lv_arc_set_value((lv_obj_t *)obj, value / 10);
    lv_label_set_text_fmt(lbl_pct, "%ld.%ld%%", (long)(value / 10), (long)(value % 10));
}

static void fmt_time(char *buf, size_t cap, uint32_t seconds)
{
    snprintf(buf, cap, "%02u:%02u:%02u", (unsigned)(seconds / 3600),
             (unsigned)((seconds % 3600) / 60), (unsigned)(seconds % 60));
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    lv_obj_set_flag(obj, LV_OBJ_FLAG_HIDDEN, hidden);
}

static void layout_active_buttons(void)
{
    lv_obj_t *visible[3];
    int count = 0;
    if (!lv_obj_has_flag(btn_pause, LV_OBJ_FLAG_HIDDEN)) visible[count++] = btn_pause;
    if (!lv_obj_has_flag(btn_cancel, LV_OBJ_FLAG_HIDDEN)) visible[count++] = btn_cancel;
    if (!lv_obj_has_flag(btn_estop, LV_OBJ_FLAG_HIDDEN)) visible[count++] = btn_estop;
    if (!count) return;

    int gap = ui_gap(6);
    int width = (ui_content_w() - (count - 1) * gap) / count;
    int x = ui_px(8);
    for (int i = 0; i < count; i++) {
        lv_obj_set_size(visible[i], width, ui_px(36));
        lv_obj_align(visible[i], LV_ALIGN_BOTTOM_LEFT, x, -ui_px(8));
        x += width + gap;
    }
}

static void update_ui(void)
{
    printer_state_t state = printer_state();
    bool printing = state == PRINTER_STATE_PRINTING;
    bool paused = state == PRINTER_STATE_PAUSED;
    if (printing || paused) job_active = true;

    const char *filename = printer_filename();
    if (filename[0]) {
        strncpy(last_file, filename, sizeof(last_file) - 1);
        last_file[sizeof(last_file) - 1] = 0;
    }
    const char *shown_file = filename[0] ? filename : last_file;
    int layer = printer_layer_current();
    int layers = printer_layer_total();
    char task_caption[64];
    if (layers > 0)
        snprintf(task_caption, sizeof(task_caption), "%s  ·  %d/%d",
                 TR("当前任务"), layer, layers);
    else
        snprintf(task_caption, sizeof(task_caption), "%s", TR("当前任务"));
    lv_label_set_text(lbl_file_cap, task_caption);
    lv_label_set_text(lbl_file, shown_file[0] ? shown_file : TR("暂无任务"));

    bool finished = state == PRINTER_STATE_COMPLETE ||
                    (job_active && (state == PRINTER_STATE_STANDBY || state == PRINTER_STATE_ERROR));
    const char *state_text = "空闲";
    uint32_t state_col = THEME_COL_TEXT;
    if (state == PRINTER_STATE_PRINTING) { state_text = "打印中"; state_col = THEME_COL_ACCENT; }
    else if (state == PRINTER_STATE_PAUSED) { state_text = "已暂停"; state_col = THEME_COL_WARN; }
    else if (state == PRINTER_STATE_COMPLETE) { state_text = "打印完成"; state_col = THEME_COL_OK; }
    else if (state == PRINTER_STATE_ERROR) { state_text = "打印出错"; state_col = THEME_COL_ERROR; }
    else if (finished) { state_text = "已取消"; state_col = THEME_COL_WARN; }
    else if (state == PRINTER_STATE_DISCONNECTED) { state_text = "未连接"; state_col = THEME_COL_WARN; }
    lv_label_set_text(lbl_state, TR(state_text));
    lv_obj_set_style_text_color(lbl_state, theme_col(state_col), 0);
    lv_obj_set_style_arc_color(arc, theme_col(state_col), LV_PART_INDICATOR);

    machine_mode_t mode = settings_load_machine_mode();
    const char *mode_text = "Klipper";
    if (mode == MACHINE_MODE_BAMBU)
        mode_text = settings_load_bambu_link() == BAMBU_LINK_LAN ? TR("局域网控制") : TR("云端监视");
    lv_label_set_text(lbl_mode, mode_text);

    char buf[24];
    fmt_time(buf, sizeof(buf), printer_print_elapsed_s());
    lv_label_set_text(lbl_elapsed, buf);
    if (printing) fmt_time(buf, sizeof(buf), printer_print_eta_s());
    else snprintf(buf, sizeof(buf), "--:--:--");
    lv_label_set_text(lbl_remaining, buf);
    char current[12], target_temp[12], temp_text[32];
    theme_fmt_float(current, sizeof(current), printer_temp_ext(), 1);
    theme_fmt_float(target_temp, sizeof(target_temp), printer_target_ext(), 0);
    snprintf(temp_text, sizeof(temp_text), "%s / %s" "\xC2\xB0", current, target_temp);
    lv_label_set_text(lbl_ext, temp_text);
    theme_fmt_float(current, sizeof(current), printer_temp_bed(), 1);
    theme_fmt_float(target_temp, sizeof(target_temp), printer_target_bed(), 0);
    snprintf(temp_text, sizeof(temp_text), "%s / %s" "\xC2\xB0", current, target_temp);
    lv_label_set_text(lbl_bed, temp_text);

    int32_t target = printer_progress_permille();
    if (target != cur_shown_pct10) {
        ui_anim_to(arc, arc_anim_cb, cur_shown_pct10, target,
                   UI_ANIM_SLOW, lv_anim_path_ease_out);
        cur_shown_pct10 = target;
    } else {
        arc_anim_cb(arc, target);
    }

    set_hidden(btn_pause, true);
    set_hidden(btn_cancel, true);
    set_hidden(btn_estop, true);
    set_hidden(btn_restart, true);
    set_hidden(btn_home, true);
    set_hidden(readonly_card, true);

    printer_capabilities_t caps = printer_capabilities();
    if (finished) {
        set_hidden(btn_restart, !(caps & PRINTER_CAP_PRINT_START));
        set_hidden(btn_home, false);
        int gap = ui_gap(6);
        if (lv_obj_has_flag(btn_restart, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_set_size(btn_home, ui_content_w(), ui_px(36));
            lv_obj_align(btn_home, LV_ALIGN_BOTTOM_MID, 0, -ui_px(8));
        } else {
            int width = (ui_content_w() - gap) / 2;
            lv_obj_set_size(btn_restart, width, ui_px(36));
            lv_obj_align(btn_restart, LV_ALIGN_BOTTOM_LEFT, ui_px(8), -ui_px(8));
            lv_obj_set_size(btn_home, width, ui_px(36));
            lv_obj_align(btn_home, LV_ALIGN_BOTTOM_RIGHT, -ui_px(8), -ui_px(8));
        }
    } else if (printing || paused) {
        bool can_pause = paused ? (caps & PRINTER_CAP_RESUME) : (caps & PRINTER_CAP_PAUSE);
        set_hidden(btn_pause, !can_pause);
        set_hidden(btn_cancel, !(caps & PRINTER_CAP_CANCEL));
        set_hidden(btn_estop, !(caps & PRINTER_CAP_EMERGENCY_STOP));
        bool read_only = lv_obj_has_flag(btn_pause, LV_OBJ_FLAG_HIDDEN) &&
                         lv_obj_has_flag(btn_cancel, LV_OBJ_FLAG_HIDDEN) &&
                         lv_obj_has_flag(btn_estop, LV_OBJ_FLAG_HIDDEN);
        set_hidden(readonly_card, !read_only);
        if (!read_only) layout_active_buttons();
    } else {
        set_hidden(readonly_card, mode != MACHINE_MODE_BAMBU || caps != 0);
    }

    lv_label_set_text(lbl_pause_icon, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    lv_label_set_text(lbl_pause_text, paused ? TR("继续") : TR("暂停"));
}

static void on_pause(lv_event_t *e)
{
    LV_UNUSED(e);
    if (printer_state() == PRINTER_STATE_PRINTING && printer_has_capability(PRINTER_CAP_PAUSE))
        printer_print_pause();
    else if (printer_has_capability(PRINTER_CAP_RESUME))
        printer_print_resume();
    update_ui();
}

static void on_cancel(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!printer_has_capability(PRINTER_CAP_CANCEL)) return;
    printer_print_cancel();
    ui_toast("已取消打印", THEME_COL_WARN);
}

static void on_restart(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!printer_has_capability(PRINTER_CAP_PRINT_START)) return;
    if (!last_file[0]) { ui_toast("没有可重启的文件", THEME_COL_ERROR); return; }
    printer_print_start(last_file);
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
    printer_emergency_stop();
    ui_toast("已急停（M112）", THEME_COL_ERROR);
}

static void on_estop(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!printer_has_capability(PRINTER_CAP_EMERGENCY_STOP)) return;
    confirm_open("确认急停？\n打印机将立即停止所有运动和加热", "急停", do_estop, NULL);
}

static lv_obj_t *make_info_card(lv_obj_t *parent, int x, int y, int width, int height,
                                const char *caption, lv_obj_t **value, uint32_t color)
{
    lv_obj_t *card = theme_card(parent);
    lv_obj_set_size(card, width, height);
    lv_obj_set_pos(card, x, y);
    lv_obj_t *cap = theme_label(card, caption, THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, -ui_px(1));
    *value = theme_label(card, "--", THEME_FONT_S, color);
    lv_obj_align(*value, LV_ALIGN_BOTTOM_LEFT, 0, ui_px(1));
    return card;
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    int gap = ui_gap(6);
    int x0 = ui_px(8);
    int y0 = THEME_TITLEBAR_H + ui_px(6);
    int body_h = ui_scr_h() - y0 - ui_px(52);
    int progress_w = ui_px(102);
    int right_x = x0 + progress_w + gap;
    int right_w = ui_scr_w() - right_x - ui_px(8);

    lv_obj_t *progress_card = theme_card(scr);
    lv_obj_set_size(progress_card, progress_w, body_h);
    lv_obj_set_pos(progress_card, x0, y0);

    int arc_size = progress_w - ui_px(16);
    arc = lv_arc_create(progress_card);
    lv_obj_set_size(arc, arc_size, arc_size);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, ui_px(5));
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(arc, theme_col(THEME_COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, ui_px(7), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, ui_px(7), LV_PART_INDICATOR);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lbl_pct = theme_label(arc, "0.0%", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_center(lbl_pct);

    lbl_state = theme_label(progress_card, "空闲", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(lbl_state, LV_ALIGN_BOTTOM_MID, 0, -ui_px(22));
    lbl_mode = theme_label(progress_card, "Klipper", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(lbl_mode, LV_ALIGN_BOTTOM_MID, 0, -ui_px(3));

    lv_obj_t *file_card = theme_card(scr);
    lv_obj_set_size(file_card, right_w, ui_px(46));
    lv_obj_set_pos(file_card, right_x, y0);
    lbl_file_cap = theme_label(file_card, "当前任务", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(lbl_file_cap, LV_ALIGN_TOP_LEFT, 0, -ui_px(1));
    lbl_file = theme_label(file_card, "", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_set_width(lbl_file, right_w - ui_px(12));
    lv_label_set_long_mode(lbl_file, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(lbl_file, LV_ALIGN_BOTTOM_LEFT, 0, ui_px(1));

    int cell_w = (right_w - gap) / 2;
    int row2_y = y0 + ui_px(46) + gap;
    make_info_card(scr, right_x, row2_y, cell_w, ui_px(44), "已用", &lbl_elapsed, THEME_COL_TEXT);
    make_info_card(scr, right_x + cell_w + gap, row2_y, cell_w, ui_px(44),
                   "剩余", &lbl_remaining, THEME_COL_TEXT);
    int row3_y = row2_y + ui_px(44) + gap;
    make_info_card(scr, right_x, row3_y, cell_w, body_h - (row3_y - y0),
                   "喷嘴", &lbl_ext, THEME_COL_EXTRUDER);
    make_info_card(scr, right_x + cell_w + gap, row3_y, cell_w, body_h - (row3_y - y0),
                   "热床", &lbl_bed, THEME_COL_BED);

    btn_pause = theme_button(scr, NULL, NULL, 1);
    lv_obj_add_event_cb(btn_pause, on_pause, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(btn_pause, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_pause, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_pause, ui_px(4), 0);
    lbl_pause_icon = theme_label(btn_pause, LV_SYMBOL_PAUSE, THEME_FONT_ICON, THEME_COL_TEXT);
    lbl_pause_text = theme_label(btn_pause, "暂停", THEME_FONT_S, THEME_COL_TEXT);

    btn_cancel = theme_button(scr, LV_SYMBOL_STOP, "取消", 0);
    for (int i = 0, count = lv_obj_get_child_count(btn_cancel); i < count; i++)
        lv_obj_set_style_text_color(lv_obj_get_child(btn_cancel, i), theme_col(THEME_COL_ERROR), 0);
    lv_obj_add_event_cb(btn_cancel, on_cancel, LV_EVENT_CLICKED, NULL);

    btn_estop = theme_button(scr, LV_SYMBOL_WARNING, "急停", 0);
    lv_obj_set_style_bg_color(btn_estop, theme_col(THEME_COL_ERROR), 0);
    theme_focus_bg(btn_estop, THEME_COL_ERROR, LV_OPA_COVER);
    lv_obj_add_event_cb(btn_estop, on_estop, LV_EVENT_CLICKED, NULL);

    btn_restart = theme_button(scr, LV_SYMBOL_REFRESH, "重启", 1);
    lv_obj_add_event_cb(btn_restart, on_restart, LV_EVENT_CLICKED, NULL);
    btn_home = theme_button(scr, LV_SYMBOL_HOME, "主菜单", 0);
    lv_obj_add_event_cb(btn_home, on_home, LV_EVENT_CLICKED, NULL);

    readonly_card = theme_card(scr);
    lv_obj_set_size(readonly_card, ui_content_w(), ui_px(36));
    lv_obj_align(readonly_card, LV_ALIGN_BOTTOM_MID, 0, -ui_px(8));
    lv_obj_set_flex_flow(readonly_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(readonly_card, LV_FLEX_ALIGN_CENTER,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(readonly_card, ui_px(6), 0);
    theme_img(readonly_card, ui_icon(&img_link, &img_link_32), THEME_COL_ACCENT);
    theme_label(readonly_card, "云端监视 · 只读", THEME_FONT_M, THEME_COL_TEXT);

    update_ui();
    return scr;
}

panel_def_t panel_job_status_def = {
    .name = "job_status", .title = "打印状态",
    .create = create, .on_show = update_ui, .on_tick = update_ui,
};
