/*
 * Z 校准（KlipperScreen zcalibrate 简化版）：PROBE_CALIBRATE / Z_ENDSTOP_CALIBRATE
 *（可用性来自 printer.gcode.help，晚到时自动重建按钮）。
 * 校准中（manual_probe.is_active）：距离档位 + 上移/下移（TESTZ）+ 当前 Z +
 * 接受（ACCEPT）/中止（ABORT）。状态往返由订阅推送驱动。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../ui_anim.h"
#include "printer.h"
#include "../widgets/toggle_group.h"
#include "../widgets/confirm.h"
#include <stdio.h>

static lv_obj_t *body;
static lv_obj_t *lbl_z;
static int built_state = -1;      /* -2=命令未加载 -1=无命令 0=空闲 1=校准中 */
static int amt_idx = 2;           /* 默认 0.1mm */
static const float amt_table[] = {0.01f, 0.05f, 0.1f, 0.5f, 1.0f};

static void on_amt(int idx, void *ud) { LV_UNUSED(ud); amt_idx = idx; }

static void on_start(lv_event_t *e)
{
    const char *cmd = (const char *)lv_event_get_user_data(e);
    printer_zcal_start(cmd);
    ui_toast(TR("校准已开始"), THEME_COL_ACCENT);
}

static void on_move(lv_event_t *e)
{
    float dir = (intptr_t)lv_event_get_user_data(e) ? 1.0f : -1.0f;
    printer_zcal_testz(dir * amt_table[amt_idx]);
}

static void do_abort(void *ud) { LV_UNUSED(ud); printer_zcal_abort(); }

static void on_accept(lv_event_t *e)
{
    LV_UNUSED(e);
    printer_zcal_accept();
    ui_toast(TR("已接受校准值"), THEME_COL_OK);
}

static void on_abort(lv_event_t *e)
{
    LV_UNUSED(e);
    confirm_open(TR("中止校准？\n当前调整不会被保存"), TR("中止"), do_abort, NULL);
}

/* 空闲态：可用校准命令按钮 + 提示 */
static void build_idle(void)
{
    lv_obj_t *hint = theme_label(body,
        TR("将喷嘴移到探测点上方（探针/限位开关正上方）后开始："),
        THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_set_width(hint, ui_content_w() - 2 * THEME_PAD);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, ui_px(10));

    int n = printer_zcal_command_count();
    int y = ui_px(44);
    for (int i = 0; i < n; i++) {
        const char *cmd = printer_zcal_command(i);
        char label[48];
        snprintf(label, sizeof(label), "%s", cmd);
        lv_obj_t *btn = theme_button(body, NULL, label, 1);
        lv_obj_set_size(btn, ui_content_w() - ui_px(40), ui_px(36));
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_add_event_cb(btn, on_start, LV_EVENT_CLICKED, (void *)cmd);
        y += ui_px(44);
    }
    if (n == 0) {
        lv_obj_t *none = theme_label(body, TR("未发现可用的校准命令（无 probe/限位校准配置）"),
                                     THEME_FONT_S, THEME_COL_WARN);
        lv_obj_set_width(none, ui_content_w() - 2 * THEME_PAD);
        lv_label_set_long_mode(none, LV_LABEL_LONG_WRAP);
        lv_obj_align(none, LV_ALIGN_TOP_MID, 0, y);
    }
}

/* 校准中：距离档位 + 上下移动 + ACCEPT/ABORT */
static void build_active(void)
{
    static const char *amts[] = {"0.01", "0.05", "0.1", "0.5", "1"};
    lv_obj_t *tg = toggle_group_create(body, amts, 5, amt_idx, on_amt, NULL);
    lv_obj_set_size(tg, ui_content_w(), ui_px(28));
    lv_obj_align(tg, LV_ALIGN_TOP_MID, 0, ui_px(6));

    lbl_z = theme_label(body, "", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(lbl_z, LV_ALIGN_TOP_MID, 0, ui_px(44));

    int gap = ui_gap(8);
    int bw = (ui_content_w() - ui_px(24) - gap) / 2;
    int h_mid = ui_px(44);
    int y_mid = ui_px(72);

    lv_obj_t *b_up = theme_button(body, LV_SYMBOL_UP, "上移", 0);
    lv_obj_set_size(b_up, bw, h_mid);
    lv_obj_align(b_up, LV_ALIGN_TOP_LEFT, ui_px(12), y_mid);
    lv_obj_add_event_cb(b_up, on_move, LV_EVENT_CLICKED, (void *)(intptr_t)1);

    lv_obj_t *b_down = theme_button(body, LV_SYMBOL_DOWN, "下移", 0);
    lv_obj_set_size(b_down, bw, h_mid);
    lv_obj_align(b_down, LV_ALIGN_TOP_RIGHT, -ui_px(12), y_mid);
    lv_obj_add_event_cb(b_down, on_move, LV_EVENT_CLICKED, (void *)(intptr_t)0);

    int y_bot = y_mid + h_mid + ui_px(10);
    lv_obj_t *b_ok = theme_button(body, LV_SYMBOL_OK, "接受", 1);
    lv_obj_set_size(b_ok, bw, ui_px(34));
    lv_obj_align(b_ok, LV_ALIGN_TOP_LEFT, ui_px(12), y_bot);
    lv_obj_add_event_cb(b_ok, on_accept, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b_abort = theme_button(body, LV_SYMBOL_CLOSE, "中止", 0);
    lv_obj_set_style_bg_color(b_abort, theme_col(THEME_COL_ERROR), 0);
    theme_focus_bg(b_abort, THEME_COL_ERROR, LV_OPA_COVER);
    lv_obj_set_size(b_abort, bw, ui_px(34));
    lv_obj_align(b_abort, LV_ALIGN_TOP_RIGHT, -ui_px(12), y_bot);
    lv_obj_add_event_cb(b_abort, on_abort, LV_EVENT_CLICKED, NULL);
}

static void rebuild_if_needed(void)
{
    int n = printer_zcal_command_count();
    int want;
    if (printer_zcal_active()) want = 1;
    else if (n > 0) want = 0;
    else want = printer_zcal_commands_pending() ? -2 : -1;
    if (want == built_state) return;
    built_state = want;
    lv_obj_clean(body);
    if (want == 1) build_active();
    else if (want == 0) build_idle();
    else {
        lv_obj_t *lbl = theme_label(body, TR(want == -2 ? "正在读取可用校准命令…"
                                                        : "未发现可用的校准命令（无 probe/限位校准配置）"),
                                    THEME_FONT_S, THEME_COL_TEXT_DIM);
        lv_obj_set_width(lbl, ui_content_w() - 2 * THEME_PAD);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, ui_px(30));
    }
}

static void update_z(void)
{
    if (lbl_z && built_state == 1) {
        char buf[40];
        snprintf(buf, sizeof(buf), TR("当前 Z：%.2f"), (double)printer_pos(2));
        lv_label_set_text(lbl_z, buf);
    }
}

static void on_tick(void)
{
    rebuild_if_needed();
    update_z();
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, ui_content_w(), ui_scr_h() - THEME_TITLEBAR_H - ui_px(8));
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(4));

    built_state = -999;   /* 强制重建 */
    rebuild_if_needed();

    ui_nav_group_set_spatial(lv_group_get_default(), true);
    return scr;
}

panel_def_t panel_zcalibrate_def = {
    .name = "zcalibrate", .title = "Z 校准", .title_s = "校准",
    .create = create,
    .on_show = on_tick,
    .on_tick = on_tick,
    .hide_temps = 1,
};
