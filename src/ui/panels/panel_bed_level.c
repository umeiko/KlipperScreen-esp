#include "bsp_caps.h"
#if BSP_HAS_BED_LEVEL
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../ui_anim.h"
#include "../assets/icons.h"
#include "../widgets/confirm.h"
#include "bed_level.h"
#include "printer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static lv_obj_t *diagram, *plate, *points[4], *result_rows[4], *results[4], *turns[4];
static lv_obj_t *btn_off, *btn_calc, *btn_retry, *message, *info, *name_label, *detail_label;
static bool compact;
static int selected, side, node_size, result_width;
static void update_ui(void);

static void enabled(lv_obj_t *o, bool on)
{
    if (on) {
        lv_obj_remove_state(o, LV_STATE_DISABLED);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_state(o, LV_STATE_DISABLED);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    }
}
static void visible(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static void text_changed(lv_obj_t *o, const char *s)
{
    if (strcmp(lv_label_get_text(o), s)) lv_label_set_text(o, s);
}
static void do_off(void *unused)
{
    LV_UNUSED(unused);
    if (!bed_level_motors_off()) ui_toast(TR("操作未发送"), THEME_COL_WARN);
}
static void on_off(lv_event_t *e)
{
    LV_UNUSED(e);
    confirm_open(TR("确认关闭电机？\n所有步进电机将失去保持力矩（M84）"), TR("关闭电机"), do_off, NULL);
}
static void on_point(lv_event_t *e)
{
    int point = (int)(intptr_t)lv_event_get_user_data(e);
    const bed_level_data_t *d = bed_level_poll();
    if (!d->can_move || d->busy) {
        update_ui();
        return;
    }
    if (bed_level_move(point)) selected = point;
    else ui_toast(TR("操作未发送"), THEME_COL_WARN);
    update_ui();
}
static void on_focus(lv_event_t *e)
{
    selected = (int)(intptr_t)lv_event_get_user_data(e);
    update_ui(); /* Focusing a screw only previews its name, never moves the printer. */
}
static void on_calc(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!bed_level_calculate()) ui_toast(TR("操作未发送"), THEME_COL_WARN);
}
static void on_retry(lv_event_t *e) { LV_UNUSED(e); bed_level_reset(); }

static void place_points(const bed_level_data_t *d)
{
    float x0 = d->points[0].x, x1 = x0, y0 = d->points[0].y, y1 = y0;
    for (int i = 1; i < d->count; i++) {
        x0 = fminf(x0, d->points[i].x); x1 = fmaxf(x1, d->points[i].x);
        y0 = fminf(y0, d->points[i].y); y1 = fmaxf(y1, d->points[i].y);
    }
    int rh = LV_MAX(lv_font_get_line_height(THEME_FONT_S), lv_font_get_line_height(ui_font_value_compact()));
    int pad = compact ? node_size / 2 + ui_gap(6) :
        LV_MAX(result_width / 2 + ui_gap(4), node_size / 2 + rh + ui_gap(5));
    pad = LV_MIN(pad, side / 3);
    int extent = LV_MAX(1, side - 2 * pad);
    float sx = x1 > x0 ? extent / (x1-x0) : INFINITY;
    float sy = y1 > y0 ? extent / (y1-y0) : INFINITY;
    float scale = fminf(sx, sy);
    if (!isfinite(scale)) scale = 1;
    /* One XY scale, centered in a square bed. Separate X/Y normalization
       squashes a three-point layout on square/portrait screens. Larger Y is rear. */
    for (int i = 0; i < 4; i++) {
        bool show = i < d->count;
        visible(points[i], show);
        /* Tiny screens keep the selected result in the information bar; four
           simultaneous result captions would crowd the screw hit targets. */
        visible(result_rows[i], show && !compact && d->points[i].result[0]);
        if (!show) continue;
        int cx = (int)lroundf(side/2.0f + (d->points[i].x-(x0+x1)/2)*scale);
        int cy = (int)lroundf(side/2.0f - (d->points[i].y-(y0+y1)/2)*scale);
        lv_obj_set_pos(points[i], cx-node_size/2, cy-node_size/2);
        lv_obj_set_pos(result_rows[i], cx-result_width/2, cy+node_size/2+ui_gap(1));
        enabled(points[i], d->can_move);
        if (i == selected) lv_obj_add_state(points[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(points[i], LV_STATE_CHECKED);

        const char *raw = d->points[i].result;
        bool base = !strcmp(raw, "base");
        const char *adjust = strchr(raw, ' ');
        adjust = adjust ? adjust+1 : raw;
        text_changed(results[i], base ? TR("基准") : adjust);
        lv_obj_set_style_text_font(results[i], base ? THEME_FONT_S : ui_font_value_compact(), 0);
        bool arrow = !base && raw[0] && !compact;
        visible(turns[i], arrow);
        if (arrow) {
            bool ccw = !strncmp(raw, "CCW", 3);
            lv_image_set_src(turns[i], ccw ? ui_icon(&img_bed_turn_ccw, &img_bed_turn_ccw_20)
                                         : ui_icon(&img_bed_turn_cw, &img_bed_turn_cw_20));
        }
        int tw = arrow ? lv_obj_get_width(turns[i]) + ui_gap(2) : 0;
        lv_obj_set_width(results[i], result_width-tw);
        lv_obj_set_pos(results[i], tw, 0);
        lv_obj_set_style_text_align(results[i], arrow ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    }
}
static void update_ui(void)
{
    const bed_level_data_t *d = bed_level_poll();
    bool ready = d->state == BED_LEVEL_READY && d->count >= 3 && d->count <= 4;
    visible(diagram, ready); visible(info, ready);
    visible(btn_off, ready); visible(btn_calc, ready && d->probe);
    if (ready) {
        if (selected >= d->count) selected = 0;
        enabled(btn_off, d->can_move); enabled(btn_calc, d->can_move);
        place_points(d);
        char name[80], detail[48];
        snprintf(name, sizeof(name), "%d  %s", selected+1, d->points[selected].name);
        if (d->points[selected].result[0])
            snprintf(detail, sizeof(detail), "%s", !strcmp(d->points[selected].result, "base") ?
                     TR("基准") : d->points[selected].result);
        else snprintf(detail, sizeof(detail), "%.0f, %.0f", d->points[selected].x, d->points[selected].y);
        text_changed(name_label, name); text_changed(detail_label, detail);
        lv_obj_set_style_text_font(detail_label, !strcmp(d->points[selected].result, "base") ? THEME_FONT_S : ui_font_value_compact(), 0);
        lv_obj_update_layout(detail_label);
        int dw = LV_MIN(lv_obj_get_content_width(info)/2, lv_obj_get_width(detail_label));
        lv_obj_set_width(name_label, lv_obj_get_content_width(info)-dw-ui_gap(6));
    }
    bool offline = printer_state() == PRINTER_STATE_DISCONNECTED || printer_state() == PRINTER_STATE_ERROR;
    const char *s = offline ? TR("未连接 Moonraker") :
        d->state == BED_LEVEL_MISSING ? TR("请配置 [bed_screws] 或 [screws_tilt_adjust]") :
        d->state == BED_LEVEL_INVALID ? TR("请配置有效的三点或四点螺丝坐标") :
        d->state == BED_LEVEL_FAILED ? TR("获取失败，请检查连接") :
        d->state == BED_LEVEL_LOADING ? TR("读取中…") :
        d->operation_failed ? TR("操作失败，请检查控制台") :
        d->busy ? TR("执行中…") :
        d->status_failed ? TR("读取中…") :
        printer_state() == PRINTER_STATE_PRINTING || printer_state() == PRINTER_STATE_PAUSED ? TR("打印中不可调平") : "";
    text_changed(message, s);
    lv_obj_set_style_text_color(message, theme_col(d->operation_failed ? THEME_COL_WARN : THEME_COL_TEXT_DIM), 0);
    visible(btn_retry, d->state == BED_LEVEL_FAILED && !offline);
    lv_obj_set_size(message, ui_content_w(), ready ? lv_font_get_line_height(THEME_FONT_S) : ui_px(compact ? 48 : 70));
    lv_label_set_long_mode(message, ready ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
    lv_obj_align(message, ready ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_CENTER, 0, ready ? -ui_gap(2) : 0);
}

static lv_obj_t *action_button(lv_obj_t *scr, const char *icon, const char *caption,
                               int x, int y, int w, int h, bool rail, lv_event_cb_t cb)
{
    lv_obj_t *b = theme_button(scr, NULL, NULL, 0);
    lv_obj_set_size(b, w, h); lv_obj_set_pos(b, x, y);
    lv_obj_set_style_pad_all(b, ui_gap(3), 0); lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *im = theme_label(b, icon, THEME_FONT_ICON, THEME_COL_ACCENT);
    lv_obj_t *label = theme_label(b, caption, THEME_FONT_S, THEME_COL_TEXT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    if (rail) {
        lv_obj_align(im, LV_ALIGN_TOP_MID, 0, ui_gap(2));
        lv_obj_set_width(label, w-2*ui_gap(4));
        lv_obj_set_height(label, LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(label, 2*lv_font_get_line_height(THEME_FONT_S), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -ui_gap(1));
    } else {
        lv_obj_align(im, LV_ALIGN_LEFT_MID, ui_gap(3), 0);
        int iw = lv_font_get_line_height(THEME_FONT_ICON);
        lv_obj_set_width(label, w-iw-4*ui_gap(4));
        lv_obj_set_height(label, LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(label, LV_MIN(2*lv_font_get_line_height(THEME_FONT_S), h-2*ui_gap(3)), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, iw+2*ui_gap(4), 0);
    }
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}
static lv_obj_t *decor(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}
static lv_obj_t *create_bed_level(void)
{
    selected = 0; compact = ui_scale() < 1;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    int gap = ui_gap(6), margin = ui_px(8);
    int y = THEME_TITLEBAR_H + gap, w = ui_content_w();
    int status_h = lv_font_get_line_height(THEME_FONT_S) + ui_gap(4);
    int info_h = lv_font_get_line_height(THEME_FONT_S) + 2*ui_gap(4);
    int body_h = ui_scr_h()-y-status_h-ui_gap(4);
    bool rail = ui_scr_w() > ui_scr_h()*1.2f;
    int action_w = rail ? ui_px(compact ? 70 : 88) : (w-gap)/2;
    int action_h = rail ? (body_h-gap)/2 : LV_MAX(ui_px(compact ? 30 : 34),
                                               lv_font_get_line_height(THEME_FONT_S)+2*ui_gap(5));
    int plot_w = rail ? w-action_w-gap : w;
    int plot_h = body_h-info_h-gap-(rail ? 0 : action_h+gap);
    side = LV_MIN(plot_w, plot_h);
    side = LV_MAX(side, ui_px(40));
    int px = margin+(rail ? action_w+gap : 0)+(plot_w-side)/2;
    int py = y+(plot_h-side)/2;
    diagram = decor(scr); lv_obj_set_size(diagram, side, side); lv_obj_set_pos(diagram, px, py);
    plate = decor(diagram); lv_obj_set_size(plate, side, side);
    lv_obj_set_style_radius(plate, ui_px(8), 0);
    lv_obj_set_style_bg_color(plate, theme_col(THEME_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(plate, LV_OPA_50, 0);
    lv_obj_set_style_border_width(plate, LV_MAX(1, ui_px(1)), 0);
    lv_obj_set_style_border_color(plate, theme_col(THEME_COL_TEXT_DIM), 0);
    lv_obj_set_style_border_opa(plate, LV_OPA_40, 0);
    /* The shallow bed outline is not a grid of large cards. All four controls
       have fixed small hit areas; captions/results never create scroll timers. */
    node_size = LV_MAX(ui_px(30), LV_MIN(ui_px(40), side/4));
    result_width = LV_MAX(ui_px(28), LV_MIN(ui_px(56), side/2-ui_gap(3)));
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = points[i] = lv_button_create(diagram);
        lv_obj_remove_style_all(b); lv_obj_set_size(b, node_size, node_size);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, theme_col(THEME_COL_ACCENT), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(b, theme_col(THEME_COL_ACCENT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_PRESSED);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_set_style_clip_corner(b, false, 0);
        theme_focusable(b);
        lv_obj_add_event_cb(b, on_point, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, on_focus, LV_EVENT_FOCUSED, (void *)(intptr_t)i);
        lv_obj_t *head = theme_img(b, ui_icon(&img_bed_screw, &img_bed_screw_48), THEME_COL_TEXT);
        lv_obj_set_style_image_recolor(head, theme_col(THEME_COL_TEXT_DIM), LV_STATE_DISABLED);
        lv_obj_set_style_image_recolor_opa(head, LV_OPA_50, LV_STATE_DISABLED);
        lv_obj_center(head);

        result_rows[i] = decor(diagram);
        lv_obj_set_size(result_rows[i], result_width, LV_MAX(lv_font_get_line_height(THEME_FONT_S),
                                                            lv_font_get_line_height(ui_font_value_compact())));
        turns[i] = theme_img(result_rows[i], ui_icon(&img_bed_turn_cw, &img_bed_turn_cw_20), THEME_COL_ACCENT);
        lv_obj_align(turns[i], LV_ALIGN_LEFT_MID, 0, 0);
        results[i] = theme_label(result_rows[i], "", ui_font_value_compact(), THEME_COL_ACCENT);
        lv_label_set_long_mode(results[i], LV_LABEL_LONG_DOT);
    }
    int info_y = y+plot_h+gap;
    info = theme_card(scr);
    lv_obj_set_size(info, plot_w, info_h);
    lv_obj_set_pos(info, margin+(rail ? action_w+gap : 0), info_y);
    lv_obj_set_style_pad_all(info, ui_gap(4), 0);
    name_label = theme_label(info, "", THEME_FONT_S, THEME_COL_TEXT);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 0, 0);
    detail_label = theme_label(info, "", ui_font_value_compact(), THEME_COL_TEXT_DIM);
    lv_obj_align(detail_label, LV_ALIGN_RIGHT_MID, 0, 0);
    int action_y = rail ? y : info_y+info_h+gap;
    btn_off = action_button(scr, LV_SYMBOL_POWER, compact && rail ? "M84" : TR("关闭电机"),
                            margin, action_y, action_w, action_h, rail, on_off);
    btn_calc = action_button(scr, LV_SYMBOL_REFRESH, compact && rail ? "TILT" : TR("螺丝调整"),
                             rail ? margin : margin+action_w+gap, rail ? y+action_h+gap : action_y,
                             action_w, action_h, rail, on_calc);
    message = theme_label(scr, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    btn_retry = theme_button(scr, LV_SYMBOL_REFRESH, TR("重试"), 1);
    lv_obj_set_size(btn_retry, ui_px(90), ui_px(28));
    lv_obj_align(btn_retry, LV_ALIGN_BOTTOM_MID, 0, -ui_px(6));
    lv_obj_add_event_cb(btn_retry, on_retry, LV_EVENT_CLICKED, NULL);
    ui_nav_group_set_spatial(lv_group_get_default(), true);
    lv_obj_update_layout(scr); update_ui(); return scr;
}
panel_def_t panel_bed_level_def = {
    .name = "bed_level", .title = "手动调平", .title_s = "调平",
    .create = create_bed_level, .on_show = update_ui, .on_tick = update_ui, .hide_temps = 1,
};
#endif
