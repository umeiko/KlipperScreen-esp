/*
 * Z 校准（布局对标 KlipperScreen zcalibrate）：
 * 左列大按钮抬/降喷嘴（TESTZ），中间 Start（空闲）+ Z 位置 + 探测偏移
 *（已存=probe/bltouch.z_offset，新值=gcode_move.homing_origin[2]），
 * 右列接受/中止，底部移动距离六档（.01/.05/.1/.5/1/5）。
 * 校准命令可用性来自 printer.gcode.help（PROBE_CALIBRATE / Z_ENDSTOP_CALIBRATE，
 * 晚到重建下拉）；活跃态由 manual_probe.is_active 订阅驱动。
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

static lv_obj_t *btn_up, *btn_down, *btn_accept, *btn_abort, *btn_start;
static lv_obj_t *dd_cmd, *lbl_z, *lbl_saved, *lbl_new;
static int built_cmd_cnt = -1;
static int amt_idx = 3;   /* 默认 0.5mm（KlipperScreen 默认 1mm，六档里 0.5 居中偏细） */
static const float amt_table[] = {0.01f, 0.05f, 0.1f, 0.5f, 1.0f, 5.0f};

static void on_amt(int idx, void *ud) { LV_UNUSED(ud); amt_idx = idx; }

static void set_enabled(lv_obj_t *obj, bool on)
{
    if (!obj) return;
    if (on) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
        lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
        lv_obj_set_style_opa(obj, LV_OPA_30, 0);
    }
}

static void on_start(lv_event_t *e)
{
    LV_UNUSED(e);
    int sel = lv_dropdown_get_selected(dd_cmd);
    const char *cmd = printer_zcal_command(sel);
    if (!cmd[0]) return;
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

static void rebuild_cmd_dropdown(void)
{
    int n = printer_zcal_command_count();
    if (n == built_cmd_cnt) return;
    built_cmd_cnt = n;
    static char opts[96];
    int len = 0;
    for (int i = 0; i < n; i++)
        len += snprintf(opts + len, sizeof(opts) - len, "%s%s",
                        i ? "\n" : "", printer_zcal_command(i));
    if (n == 0) {
        const char *s;
        if (printer_state() == PRINTER_STATE_DISCONNECTED ||
            printer_state() == PRINTER_STATE_ERROR)
            s = TR("未连接");
        else if (printer_zcal_commands_pending())
            s = TR("读取中…");
        else
            s = TR("无可用校准命令");
        snprintf(opts, sizeof(opts), "%s", s);
    }
    lv_dropdown_set_options(dd_cmd, opts);
    set_enabled(btn_start, n > 0);
}

static void update_state(void)
{
    rebuild_cmd_dropdown();

    bool active = printer_zcal_active();
    set_enabled(btn_up, true);          /* 空闲也允许微调走位（KlipperScreen 同） */
    set_enabled(btn_down, true);
    set_enabled(btn_accept, active);
    set_enabled(btn_abort, active);
    set_enabled(btn_start, !active && built_cmd_cnt > 0);
    if (dd_cmd) set_enabled(dd_cmd, !active);

    char buf[48];
    if (active) {
        snprintf(buf, sizeof(buf), "Z: %.2f", (double)printer_pos(2));
    } else {
        snprintf(buf, sizeof(buf), "Z: %.2f", (double)printer_pos(2));
    }
    lv_label_set_text(lbl_z, buf);

    float v;
    if (printer_probe_z_offset(&v)) snprintf(buf, sizeof(buf), "%.2f", (double)v);
    else                            snprintf(buf, sizeof(buf), "--");
    lv_label_set_text(lbl_saved, buf);

    if (printer_homing_origin_z(&v)) snprintf(buf, sizeof(buf), "%.2f", (double)v);
    else                             snprintf(buf, sizeof(buf), "--");
    lv_label_set_text(lbl_new, buf);
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    int top = THEME_TITLEBAR_H + ui_px(4);
    int strip_h = ui_px(44);                       /* 底部距离条（标签+档位） */
    int mid_h = ui_scr_h() - top - strip_h - ui_px(4);
    int col_l_w = ui_content_w() * 26 / 100;
    int col_r_w = ui_content_w() * 22 / 100;
    int col_c_x = ui_px(8) + col_l_w + ui_px(6);
    int col_c_w = ui_content_w() - col_l_w - col_r_w - ui_px(8) - 2 * ui_px(6);

    /* 左列：抬升/下降喷嘴（高度平分） */
    int bh = (mid_h - ui_px(6)) / 2;
    btn_up = theme_button(scr, LV_SYMBOL_UP, "抬升", 0);
    lv_obj_set_size(btn_up, col_l_w, bh);
    lv_obj_align(btn_up, LV_ALIGN_TOP_LEFT, ui_px(8), top);
    lv_obj_add_event_cb(btn_up, on_move, LV_EVENT_CLICKED, (void *)(intptr_t)1);

    btn_down = theme_button(scr, LV_SYMBOL_DOWN, "下降", 0);
    lv_obj_set_size(btn_down, col_l_w, bh);
    lv_obj_align(btn_down, LV_ALIGN_TOP_LEFT, ui_px(8), top + bh + ui_px(6));
    lv_obj_add_event_cb(btn_down, on_move, LV_EVENT_CLICKED, (void *)(intptr_t)0);

    /* 右列：接受/中止 */
    btn_accept = theme_button(scr, LV_SYMBOL_OK, "接受", 0);
    lv_obj_set_style_bg_color(btn_accept, theme_col(THEME_COL_OK), 0);
    theme_focus_bg(btn_accept, THEME_COL_OK, LV_OPA_COVER);
    lv_obj_set_size(btn_accept, col_r_w, bh);
    lv_obj_align(btn_accept, LV_ALIGN_TOP_RIGHT, -ui_px(8), top);
    lv_obj_add_event_cb(btn_accept, on_accept, LV_EVENT_CLICKED, NULL);

    btn_abort = theme_button(scr, LV_SYMBOL_CLOSE, "中止", 0);
    lv_obj_set_style_bg_color(btn_abort, theme_col(THEME_COL_ERROR), 0);
    theme_focus_bg(btn_abort, THEME_COL_ERROR, LV_OPA_COVER);
    lv_obj_set_size(btn_abort, col_r_w, bh);
    lv_obj_align(btn_abort, LV_ALIGN_TOP_RIGHT, -ui_px(8), top + bh + ui_px(6));
    lv_obj_add_event_cb(btn_abort, on_abort, LV_EVENT_CLICKED, NULL);

    /* 中列：命令下拉 + Start + Z + 探测偏移 */
    dd_cmd = lv_dropdown_create(scr);
    lv_dropdown_set_options(dd_cmd, TR("读取中…"));
    lv_obj_set_size(dd_cmd, col_c_w, ui_px(30));
    lv_obj_align(dd_cmd, LV_ALIGN_TOP_LEFT, col_c_x, top);
    theme_dropdown_finish(dd_cmd);   /* 列表字体/配色/顶层化与设置页下拉一致 */

    btn_start = theme_button(scr, LV_SYMBOL_PLAY, "开始", 1);
    lv_obj_set_size(btn_start, col_c_w, ui_px(38));
    lv_obj_align(btn_start, LV_ALIGN_TOP_LEFT, col_c_x, top + ui_px(34));
    lv_obj_add_event_cb(btn_start, on_start, LV_EVENT_CLICKED, NULL);

    lbl_z = theme_label(scr, "Z: --", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(lbl_z, LV_ALIGN_TOP_LEFT, col_c_x, top + ui_px(80));

    lv_obj_t *cap_off = theme_label(scr, TR("探测偏移"), THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(cap_off, LV_ALIGN_TOP_LEFT, col_c_x, top + ui_px(102));
    lv_obj_t *cap_saved = theme_label(scr, TR("已保存"), THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(cap_saved, LV_ALIGN_TOP_LEFT, col_c_x + ui_px(8), top + ui_px(118));
    lbl_saved = theme_label(scr, "--", THEME_FONT_S, THEME_COL_TEXT);
    lv_obj_align(lbl_saved, LV_ALIGN_TOP_LEFT, col_c_x + ui_px(52), top + ui_px(118));
    lv_obj_t *cap_new = theme_label(scr, TR("新值"), THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(cap_new, LV_ALIGN_TOP_LEFT, col_c_x + ui_px(8), top + ui_px(134));
    lbl_new = theme_label(scr, "--", THEME_FONT_S, THEME_COL_TEXT);
    lv_obj_align(lbl_new, LV_ALIGN_TOP_LEFT, col_c_x + ui_px(52), top + ui_px(134));

    /* 底部：移动距离六档 */
    lv_obj_t *cap_dist = theme_label(scr, TR("移动距离 (mm)"), THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(cap_dist, LV_ALIGN_BOTTOM_MID, 0, -(strip_h - ui_px(12)));
    static const char *amts[] = {".01", ".05", ".1", ".5", "1", "5"};
    lv_obj_t *tg = toggle_group_create(scr, amts, 6, amt_idx, on_amt, NULL);
    lv_obj_set_size(tg, ui_content_w(), ui_px(26));
    lv_obj_align(tg, LV_ALIGN_BOTTOM_MID, 0, -ui_px(2));

    /* 网格布局：四方向键几何走位（ui_nav 白名单） */
    ui_nav_group_set_spatial(lv_group_get_default(), true);
    built_cmd_cnt = -1;
    update_state();
    return scr;
}

panel_def_t panel_zcalibrate_def = {
    .name = "zcalibrate", .title = "Z 校准", .title_s = "校准",
    .create = create,
    .on_show = update_state,
    .on_tick = update_state,
};
