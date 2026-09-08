/*
 * 温度控制：设备行（点击弹 keypad 设目标温） + 预热预设（对标 KlipperScreen temperature）
 */
#include "../theme.h"
#include "../ui_anim.h"
#include "../panel_mgr.h"
#include "printer.h"
#include "../widgets/keypad.h"
#include "../assets/icons.h"
#include <stdio.h>

static lv_obj_t *lbl_ext_cur, *lbl_ext_tgt;
static lv_obj_t *lbl_bed_cur, *lbl_bed_tgt;
static int ext_shown10 = -1, bed_shown10 = -1;   /* 0.1 度单位的显示值 */

static void temp_anim_cb(void *obj, int32_t v10)
{
    lv_label_set_text_fmt((lv_obj_t *)obj, "%ld.%ld", (long)(v10 / 10),
                          (long)(v10 < 0 ? -(v10 % 10) : v10 % 10));
}

static void set_ext_cb(float v, int ok, void *ud)
{
    LV_UNUSED(ud);
    if (ok) { printer_set_target_ext(v); ui_toast(v > 0 ? "喷嘴加热中" : "喷嘴已关闭", THEME_COL_EXTRUDER); }
}

static void set_bed_cb(float v, int ok, void *ud)
{
    LV_UNUSED(ud);
    if (ok) { printer_set_target_bed(v); ui_toast(v > 0 ? "热床加热中" : "热床已关闭", THEME_COL_BED); }
}

static void on_row_ext(lv_event_t *e) { LV_UNUSED(e); keypad_open("喷嘴目标温度", printer_target_ext(), set_ext_cb, NULL); }
static void on_row_bed(lv_event_t *e) { LV_UNUSED(e); keypad_open("热床目标温度", printer_target_bed(), set_bed_cb, NULL); }

static void on_preset(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    static const struct { float e, b; const char *name; } presets[] = {
        {210, 60, "PLA"}, {240, 80, "PETG"}, {250, 100, "ABS"}, {0, 0, "全部冷却"},
    };
    printer_set_target_ext(presets[idx].e);
    printer_set_target_bed(presets[idx].b);
    ui_toast(presets[idx].name, THEME_COL_ACCENT);
}

static lv_obj_t *make_row(lv_obj_t *parent, const char *name, uint32_t col,
                          const lv_image_dsc_t *icon, int h,
                          lv_obj_t **cur_out, lv_obj_t **tgt_out, lv_event_cb_t cb)
{
    lv_obj_t *row = theme_card(parent);
    lv_obj_set_size(row, ui_content_w(), h);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ic = theme_img(row, icon, col);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, ui_px(4), 0);

    lv_obj_t *name_lbl = theme_label(row, name, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, ui_px(44), 0);

    lv_obj_t *cur = theme_label(row, "--", THEME_FONT_L, col);
    lv_obj_align(cur, LV_ALIGN_RIGHT_MID, -ui_px(58), 0);

    lv_obj_t *tgt = theme_label(row, "/0°", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(tgt, LV_ALIGN_RIGHT_MID, -ui_px(4), ui_px(6));

    *cur_out = cur;
    *tgt_out = tgt;
    return row;
}

static void update_temps(void)
{
    int e10 = (int)(printer_temp_ext() * 10);
    int b10 = (int)(printer_temp_bed() * 10);
    if (ext_shown10 < 0) ext_shown10 = e10;   /* 首次直接到位 */
    if (bed_shown10 < 0) bed_shown10 = b10;
    if (e10 != ext_shown10) {
        ui_anim_to(lbl_ext_cur, temp_anim_cb, ext_shown10, e10, UI_ANIM_SLOW, lv_anim_path_ease_out);
        ext_shown10 = e10;
    }
    if (b10 != bed_shown10) {
        ui_anim_to(lbl_bed_cur, temp_anim_cb, bed_shown10, b10, UI_ANIM_SLOW, lv_anim_path_ease_out);
        bed_shown10 = b10;
    }
    lv_label_set_text_fmt(lbl_ext_tgt, "/%d" "\xC2\xB0", (int)(printer_target_ext() + 0.5f));
    lv_label_set_text_fmt(lbl_bed_tgt, "/%d" "\xC2\xB0", (int)(printer_target_bed() + 0.5f));
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    /* 两张设备卡撑满标题栏与预设行之间的空间（大屏不留空带） */
    int gap = ui_gap(6);
    int y0 = THEME_TITLEBAR_H + gap;
    int reserve_bottom = ui_px(36) + ui_px(12) + gap;   /* 预设行高 + 底边距 + 间隔 */
    int card_h = (ui_scr_h() - y0 - reserve_bottom - gap) / 2;

    lv_obj_t *row_ext = make_row(scr, "Extruder", THEME_COL_EXTRUDER, &img_nozzle_32, card_h,
                                 &lbl_ext_cur, &lbl_ext_tgt, on_row_ext);
    lv_obj_align(row_ext, LV_ALIGN_TOP_MID, 0, y0);

    lv_obj_t *row_bed = make_row(scr, "Heatbed", THEME_COL_BED, &img_bed_32, card_h,
                                 &lbl_bed_cur, &lbl_bed_tgt, on_row_bed);
    lv_obj_align(row_bed, LV_ALIGN_TOP_MID, 0, y0 + card_h + gap);

    /* 预设行 */
    static const char *names[] = {"PLA", "PETG", "ABS", "冷却"};
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, ui_content_w(), ui_px(36));
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -ui_px(12));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, 0);

    int pw = (ui_content_w() - 3 * gap) / 4;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = theme_button(row, NULL, names[i], 0);
        lv_obj_set_size(b, pw, ui_px(34));
        lv_obj_add_event_cb(b, on_preset, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    return scr;
}

panel_def_t panel_temperature_def = {
    .name = "temperature", .title = "温度控制",
    .create = create,
    .on_show = update_temps,
    .on_tick = update_temps,
};
