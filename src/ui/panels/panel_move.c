/*
 * 点动移动：距离档 + X/Y/Z 步进 + 归位（对标 KlipperScreen move 简化版）
 */
#include "../theme.h"
#include "../ui_anim.h"
#include "../panel_mgr.h"
#include "printer.h"
#include "../widgets/toggle_group.h"
#include <stdio.h>

static const float dist_table[] = {0.1f, 1.0f, 10.0f, 50.0f};
static int dist_idx = 1;
static lv_obj_t *lbl_pos[3];

static void on_dist(int idx, void *ud)
{
    LV_UNUSED(ud);
    dist_idx = idx;
}

static void on_jog(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);   /* axis*2 + (dir>0) */
    int axis = code / 2;
    float dir = (code % 2) ? 1.0f : -1.0f;
    printer_jog(axis, dir * dist_table[dist_idx]);
}

static void on_home(lv_event_t *e)
{
    int axis = (int)(intptr_t)lv_event_get_user_data(e);
    printer_home(axis);
    ui_toast(axis < 0 ? "全部轴归位" : "轴归位", THEME_COL_ACCENT);
}

static void update_pos(void)
{
    static const char axis_name[] = {'X', 'Y', 'Z'};
    char vbuf[16];
    for (int a = 0; a < 3; a++) {
        if (printer_homed(a)) {
            theme_fmt_float(vbuf, sizeof(vbuf), printer_pos(a), 1);
            lv_label_set_text_fmt(lbl_pos[a], "%c %s", axis_name[a], vbuf);
        } else {
            lv_label_set_text_fmt(lbl_pos[a], "%c ?", axis_name[a]);
        }
    }
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    /* 距离档 */
    static const char *dists[] = {"0.1", "1", "10", "50"};
    lv_obj_t *tg = toggle_group_create(scr, dists, 4, dist_idx, on_dist, NULL);
    lv_obj_set_size(tg, ui_content_w(), ui_px(28));
    lv_obj_align(tg, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(4));

    /* 三轴步进：每行  [-] [轴 位置] [+]，行宽/行距按可用空间撑满（大屏不右侧留白） */
    static const char *minus[3] = {LV_SYMBOL_LEFT,  LV_SYMBOL_DOWN, LV_SYMBOL_DOWN};
    static const char *plus[3]  = {LV_SYMBOL_RIGHT, LV_SYMBOL_UP,   LV_SYMBOL_UP};
    int gap = ui_gap(6);
    int y0 = THEME_TITLEBAR_H + ui_px(4) + ui_px(28) + gap;          /* 距离档下方 */
    int ybot = ui_scr_h() - ui_px(6) - ui_px(30) - gap;              /* 归位行上方 */
    int pitch = (ybot - y0) / 3;
    int row_h = pitch - gap;
    int side_w = ui_px(76);
    int card_w = ui_content_w() - 2 * side_w - 2 * gap;
    for (int a = 0; a < 3; a++) {
        int y = y0 + a * pitch;
        lv_obj_t *bm = theme_button(scr, minus[a], NULL, 0);
        lv_obj_set_size(bm, side_w, row_h);
        lv_obj_align(bm, LV_ALIGN_TOP_LEFT, ui_px(8), y);
        lv_obj_add_event_cb(bm, on_jog, LV_EVENT_CLICKED, (void *)(intptr_t)(a * 2));

        lv_obj_t *card = theme_card(scr);
        lv_obj_set_size(card, card_w, row_h);
        lv_obj_align(card, LV_ALIGN_TOP_LEFT, ui_px(8) + side_w + gap, y);
        lbl_pos[a] = theme_label(card, "?", THEME_FONT_M, THEME_COL_TEXT);
        lv_obj_center(lbl_pos[a]);

        lv_obj_t *bp = theme_button(scr, plus[a], NULL, 0);
        lv_obj_set_size(bp, side_w, row_h);
        lv_obj_align(bp, LV_ALIGN_TOP_LEFT, ui_px(8) + side_w + gap + card_w + gap, y);
        lv_obj_add_event_cb(bp, on_jog, LV_EVENT_CLICKED, (void *)(intptr_t)(a * 2 + 1));
    }

    /* 归位行：三枚按钮三分内容宽 */
    static const struct { const char *icon, *t; int axis; } homes[] = {
        {LV_SYMBOL_HOME, "XY", 0}, {LV_SYMBOL_HOME, "Z", 2}, {LV_SYMBOL_HOME, "全部", -1},
    };
    int hw = (ui_content_w() - 2 * gap) / 3;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = theme_button(scr, homes[i].icon, homes[i].t, i == 2);
        lv_obj_set_size(b, hw, ui_px(30));
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, ui_px(8) + i * (hw + gap), -ui_px(6));
        lv_obj_add_event_cb(b, on_home, LV_EVENT_CLICKED, (void *)(intptr_t)homes[i].axis);
    }
    /* 注：mock 的 Home XY 简化为归 X（演示用） */

    return scr;
}

panel_def_t panel_move_def = {
    .name = "move", .title = "移动",
    .create = create,
    .on_show = update_pos,
    .on_tick = update_pos,
};
