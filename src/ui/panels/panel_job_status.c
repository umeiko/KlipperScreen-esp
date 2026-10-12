/*
 * 打印状态仪表页：左侧集中进度/状态，右侧以卡片显示任务、时间和温度。
 * 底部操作由打印机后端能力决定；云端监视模式显示明确的只读状态条。
 */
#include "../theme.h"
#include "../lang.h"
#include "../ui_anim.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../assets/icons.h"
#include "printer.h"
#include "app_settings.h"
#include "../widgets/confirm.h"
#include "../widgets/gcode_thumb.h"
#include "bsp_caps.h"
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
static int compact(void);   /* 定义在 make_info_card 处 */
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
static bool narrow_info_rows;

#if BSP_HAS_GCODE_THUMB
static lv_obj_t *thumb_parent, *job_thumb;
static char thumb_file[96];
static int thumb_w, thumb_h, thumb_offset;
static bool thumb_landscape, thumb_was_active;
static uint32_t thumb_elapsed, thumb_attempt_at;
static unsigned thumb_attempts;

static void update_thumbnail(const char *filename, bool active)
{
    uint32_t elapsed = printer_print_elapsed_s();
    bool new_job = active && (!thumb_was_active || elapsed < thumb_elapsed);
    thumb_was_active = active;
    thumb_elapsed = elapsed;
    if (settings_load_machine_mode() != MACHINE_MODE_KLIPPER) filename = "";
    if (new_job || strcmp(filename, thumb_file) != 0) {
        /* DELETE detaches an in-flight worker from its old image. Its eventual
         * callback frees its own result instead of painting the next task. */
        if (job_thumb) lv_obj_delete(job_thumb);
        job_thumb = NULL;
        snprintf(thumb_file, sizeof(thumb_file), "%s", filename);
        thumb_attempts = 0;
    }
    if (!thumb_parent || !thumb_file[0] || thumb_h <= ui_px(24)) return;
    if (job_thumb && gcode_thumb_state(job_thumb) != GCODE_THUMB_FAILED) return;
    /* Metadata may not be ready at boot/upload time. Retry failed deliveries
     * three times, 3/6/12 seconds apart, rather than starting a worker per tick. */
    if (thumb_attempts >= 4 || (thumb_attempts &&
        lv_tick_elaps(thumb_attempt_at) < (3000u << (thumb_attempts - 1)))) return;
    if (job_thumb) lv_obj_delete(job_thumb);
    thumb_attempt_at = lv_tick_get();
    thumb_attempts++;
    job_thumb = gcode_thumb_create(thumb_parent, thumb_file, thumb_w, thumb_h);
    if (job_thumb)
        lv_obj_align(job_thumb, thumb_landscape ? LV_ALIGN_TOP_MID : LV_ALIGN_CENTER,
                     0, thumb_offset);
}
#endif

typedef enum {
    INFO_ICON_NONE,
    INFO_ICON_ELAPSED,
    INFO_ICON_REMAINING,
    INFO_ICON_NOZZLE,
    INFO_ICON_BED,
} info_icon_t;

static void set_temp_value(lv_obj_t *label, const char *text)
{
    const lv_font_t *font = THEME_FONT_S;
    int width = lv_obj_get_width(label);
    if (width > 0 && lv_text_get_width(text, (uint32_t)strlen(text), font, 0) > width)
        font = ui_font_value_compact();
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_text(label, text);
}

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
    /* A new active task with late filename metadata must not show the previous
     * task's name/image. Keep last_file only for the completed/restart view. */
    const char *shown_file = filename[0] ? filename : (printing || paused) ? "" : last_file;
#if BSP_HAS_GCODE_THUMB
    update_thumbnail(shown_file, printing || paused);
#endif
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
    /* 方屏/竖屏的窄右栏只保留当前温度：目标温度可在点入温度页后查看，
       比压缩或裁掉当前值更可靠，也彻底消除多语言标题对数值宽度的影响。 */
    if (narrow_info_rows) {
        theme_fmt_float(current, sizeof(current), printer_temp_ext(), 1);
        snprintf(temp_text, sizeof(temp_text), "%s" "\xC2\xB0", current);
        set_temp_value(lbl_ext, temp_text);
        theme_fmt_float(current, sizeof(current), printer_temp_bed(), 1);
        snprintf(temp_text, sizeof(temp_text), "%s" "\xC2\xB0", current);
        set_temp_value(lbl_bed, temp_text);
    } else {
        /* 小屏紧凑格：去掉空格与度号，防止折行盖住标题行 */
        const char *fmt = compact() ? "%s/%s" : "%s / %s" "\xC2\xB0";
        theme_fmt_float(current, sizeof(current), printer_temp_ext(), 1);
        theme_fmt_float(target_temp, sizeof(target_temp), printer_target_ext(), 0);
        snprintf(temp_text, sizeof(temp_text), fmt, current, target_temp);
        set_temp_value(lbl_ext, temp_text);
        theme_fmt_float(current, sizeof(current), printer_temp_bed(), 1);
        theme_fmt_float(target_temp, sizeof(target_temp), printer_target_bed(), 0);
        snprintf(temp_text, sizeof(temp_text), fmt, current, target_temp);
        set_temp_value(lbl_bed, temp_text);
    }

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

static void on_temperature(lv_event_t *e)
{
    LV_UNUSED(e);
    if (settings_load_machine_mode() == MACHINE_MODE_KLIPPER)
        panel_mgr_open("temperature");
}

static int compact(void) { return ui_scale() < 1.0f; }   /* 小屏：行高放不下两行带边距文本 */

static lv_obj_t *make_info_card(lv_obj_t *parent, int x, int y, int width, int height,
                                const char *caption, lv_obj_t **value, uint32_t color,
                                int row_mode, int temperature, info_icon_t row_icon)
{
    int temp_action = temperature && settings_load_machine_mode() == MACHINE_MODE_KLIPPER;
    lv_obj_t *card = temp_action ? theme_action_card(parent) : theme_card(parent);
    if (temp_action) lv_obj_add_event_cb(card, on_temperature, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(card, width, height);
    lv_obj_set_pos(card, x, y);
    int tight = compact() || row_mode || height < ui_px(40);   /* 小屏卡高只有 23~31px，必须走紧凑排布 */
    if (tight) lv_obj_set_style_pad_ver(card, ui_px(1), 0);
    *value = theme_label(card, "--", THEME_FONT_S, color);
    if (row_mode) {   /* 窄右栏：中/繁中文保留短标题，其余语言改用图标 */
        int inner_w = width - 2 * THEME_PAD;
        int left_lane_w;
        ui_lang_t lang = ui_lang_get();
        if (lang == UI_LANG_ZH || lang == UI_LANG_ZH_TW) {
            lv_obj_t *cap = theme_label(card, caption, THEME_FONT_S, THEME_COL_TEXT_DIM);
            int text_w = lv_text_get_width(lv_label_get_text(cap),
                                           (uint32_t)strlen(lv_label_get_text(cap)),
                                           THEME_FONT_S, 0);
            left_lane_w = LV_MIN(text_w + ui_px(5), inner_w / 2);
            lv_obj_set_width(cap, left_lane_w);
            lv_label_set_long_mode(cap, LV_LABEL_LONG_CLIP);
            lv_obj_align(cap, LV_ALIGN_LEFT_MID, 0, 0);
        } else {
            left_lane_w = ui_px(22);
            lv_obj_t *icon_obj;
            if (row_icon == INFO_ICON_NOZZLE) {
                icon_obj = theme_img(card, ui_icon(&img_nozzle_16, &img_nozzle_32), THEME_COL_EXTRUDER);
            } else if (row_icon == INFO_ICON_BED) {
                icon_obj = theme_img(card, ui_icon(&img_bed_16, &img_bed_32), THEME_COL_BED);
            } else {
                const char *symbol = row_icon == INFO_ICON_REMAINING ? LV_SYMBOL_NEXT : LV_SYMBOL_PLAY;
                icon_obj = theme_label(card, symbol, THEME_FONT_ICON, THEME_COL_TEXT_DIM);
            }
            lv_obj_align(icon_obj, LV_ALIGN_LEFT_MID, 0, 0);
        }
        int value_w = inner_w - left_lane_w;
        lv_obj_set_width(*value, value_w);
        lv_label_set_long_mode(*value, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(*value, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(*value, LV_ALIGN_RIGHT_MID, 0, 0);
        return card;
    }
    lv_obj_t *cap = theme_label(card, caption, THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, tight ? 0 : -ui_px(1));
    if (tight) {   /* 防折行盖住标题行：限宽，放不下就滚动显示 */
        lv_obj_set_width(*value, width - 2 * THEME_PAD);
        lv_label_set_long_mode(*value, temperature ? LV_LABEL_LONG_CLIP
                                                   : LV_LABEL_LONG_SCROLL_CIRCULAR);
    } else if (temperature) {
        /* 温度永远固定单行；空间不足由 update_ui 降一档字号，不做跑马灯。 */
        lv_obj_set_width(*value, width - 2 * THEME_PAD);
        lv_label_set_long_mode(*value, LV_LABEL_LONG_CLIP);
    }
    lv_obj_align(*value, LV_ALIGN_BOTTOM_LEFT, 0, tight ? 0 : ui_px(1));
    return card;
}

static lv_obj_t *create_job_status(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    int gap = ui_gap(6);
    int x0 = ui_px(8);
    int y0 = THEME_TITLEBAR_H + ui_px(6);
    /* 小屏底部按钮区压缩预留（按钮本身只有 ui_px(28) 高），把高度还给信息卡 */
    int body_h = ui_scr_h() - y0 - ui_px(compact() ? 40 : 52);
    int progress_w = ui_px(102);
    int right_x = x0 + progress_w + gap;
    int right_w = ui_scr_w() - right_x - ui_px(8);
#if BSP_HAS_GCODE_THUMB
    /* 横屏高度放不下「圆环 + 缩略图」纵向堆叠：有缩略图能力的平台
     * 改为缩略图占主视觉区，百分比下沉到原模式副标题的位置。 */
    /* 方屏的主体同样是左右两栏，也应采用横向缩略图布局。 */
    thumb_landscape = ui_scr_w() >= ui_scr_h();
    job_thumb = NULL;
    thumb_file[0] = 0;
    thumb_attempts = 0;
    thumb_was_active = false;
    thumb_elapsed = 0;
#endif

    lv_obj_t *progress_card = theme_card(scr);
    lv_obj_set_size(progress_card, progress_w, body_h);
    lv_obj_set_pos(progress_card, x0, y0);
#if BSP_HAS_GCODE_THUMB
    thumb_parent = progress_card;
    thumb_w = progress_w - ui_px(16);
    if (thumb_landscape) {
        thumb_h = body_h - ui_px(48);
        thumb_offset = ui_px(5);
    } else {
        int region_y = ui_px(5) + progress_w - ui_px(16) + ui_px(4);
        thumb_h = body_h - ui_px(40) - region_y;
        thumb_offset = region_y + thumb_h / 2 - body_h / 2;
    }
#endif

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
#if BSP_HAS_GCODE_THUMB
    if (thumb_landscape) {
        lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
        lbl_pct = theme_label(progress_card, "0.0%", THEME_FONT_S, THEME_COL_TEXT);
        lv_obj_align(lbl_pct, LV_ALIGN_BOTTOM_MID, 0, -ui_px(3));
    } else
#endif
    {
        lbl_pct = theme_label(arc, "0.0%", THEME_FONT_M, THEME_COL_TEXT);
        lv_obj_center(lbl_pct);
    }

    lbl_state = theme_label(progress_card, "空闲", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(lbl_state, LV_ALIGN_BOTTOM_MID, 0, -ui_px(22));
    lbl_mode = theme_label(progress_card, "Klipper", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(lbl_mode, LV_ALIGN_BOTTOM_MID, 0, -ui_px(3));
#if BSP_HAS_GCODE_THUMB
    if (thumb_landscape) lv_obj_add_flag(lbl_mode, LV_OBJ_FLAG_HIDDEN);
#endif

    lv_obj_t *file_card = theme_card(scr);
    lv_obj_set_size(file_card, right_w, ui_px(46));
    lv_obj_set_pos(file_card, right_x, y0);
    if (compact()) lv_obj_set_style_pad_ver(file_card, ui_px(1), 0);
    lbl_file_cap = theme_label(file_card, "当前任务", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(lbl_file_cap, LV_ALIGN_TOP_LEFT, 0, compact() ? 0 : -ui_px(1));
    lbl_file = theme_label(file_card, "", compact() ? THEME_FONT_S : THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_set_width(lbl_file, right_w - ui_px(12));
    lv_label_set_long_mode(lbl_file, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(lbl_file, LV_ALIGN_BOTTOM_LEFT, 0, compact() ? 0 : ui_px(1));

    int cell_w = (right_w - gap) / 2;
    int row2_y = y0 + ui_px(46) + gap;
    narrow_info_rows = cell_w < ui_px(70);
    if (narrow_info_rows) {
        /* 方屏和竖屏：右列两格分栏后过窄，改为语言无关图标 + 数值的竖排。 */
        int rows_h = body_h - ui_px(46) - gap;
        int rh = (rows_h - 3 * gap) / 4;
        make_info_card(scr, right_x, row2_y, right_w, rh, "已用", &lbl_elapsed,
                       THEME_COL_TEXT, 1, 0, INFO_ICON_ELAPSED);
        make_info_card(scr, right_x, row2_y + (rh + gap), right_w, rh, "剩余", &lbl_remaining,
                       THEME_COL_TEXT, 1, 0, INFO_ICON_REMAINING);
        make_info_card(scr, right_x, row2_y + 2 * (rh + gap), right_w, rh, "喷嘴", &lbl_ext,
                       THEME_COL_EXTRUDER, 1, 1, INFO_ICON_NOZZLE);
        make_info_card(scr, right_x, row2_y + 3 * (rh + gap), right_w, rh, "热床", &lbl_bed,
                       THEME_COL_BED, 1, 1, INFO_ICON_BED);
    } else {
        make_info_card(scr, right_x, row2_y, cell_w, ui_px(44), "已用", &lbl_elapsed,
                       THEME_COL_TEXT, 0, 0, INFO_ICON_NONE);
        make_info_card(scr, right_x + cell_w + gap, row2_y, cell_w, ui_px(44),
                       "剩余", &lbl_remaining, THEME_COL_TEXT, 0, 0, INFO_ICON_NONE);
        int row3_y = row2_y + ui_px(44) + gap;
        make_info_card(scr, right_x, row3_y, cell_w, body_h - (row3_y - y0),
                       "喷嘴", &lbl_ext, THEME_COL_EXTRUDER, 0, 1, INFO_ICON_NONE);
        make_info_card(scr, right_x + cell_w + gap, row3_y, cell_w, body_h - (row3_y - y0),
                       "热床", &lbl_bed, THEME_COL_BED, 0, 1, INFO_ICON_NONE);
    }

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

    /* 底部按钮横向排列：方向键几何走位（ui_nav 白名单） */
    ui_nav_group_set_spatial(lv_group_get_default(), true);
    return scr;
}

panel_def_t panel_job_status_def = {
    .name = "job_status", .title = "打印状态", .title_s = "打印",
    .create = create_job_status, .on_show = update_ui, .on_tick = update_ui,
    .hide_temps = 1,   /* 页面内已有温度显示，长语言标题（如法语）避免与标题栏温度遮挡 */
    .show_fine_tune = 1,
};
