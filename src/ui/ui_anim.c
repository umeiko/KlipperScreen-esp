#include "ui_anim.h"
#include "theme.h"
#include "lang.h"

void ui_anim_to(void *var, lv_anim_exec_xcb_t cb, int32_t from, int32_t to,
                uint32_t dur, lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, dur);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_start(&a);
}

void ui_anim_after(uint32_t delay_ms, lv_anim_completed_cb_t cb, void *var)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_values(&a, 0, 1);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_duration(&a, 1);
    lv_anim_set_completed_cb(&a, cb);
    lv_anim_start(&a);
}

/* ---------- Toast ---------- */
static void toast_delete_cb(lv_anim_t *a)
{
    lv_obj_delete((lv_obj_t *)a->var);
}

static void toast_slide_out(lv_anim_t *a)
{
    lv_obj_t *toast = (lv_obj_t *)a->var;
    lv_anim_t t;
    lv_anim_init(&t);
    lv_anim_set_var(&t, toast);
    /* toast 高度随内容变化，滑出目标按实际高度完全移出屏幕 */
    lv_anim_set_values(&t, lv_obj_get_y(toast), -lv_obj_get_height(toast) - 8);
    lv_anim_set_duration(&t, UI_ANIM_NORMAL);
    lv_anim_set_path_cb(&t, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&t, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_completed_cb(&t, toast_delete_cb);
    lv_anim_start(&t);
}

void ui_toast(const char *text, uint32_t accent_hex)
{
    lv_obj_t *top = lv_layer_top();
    lv_obj_t *toast = theme_card(top);
    /* 宽度给足，高度随内容：长错误文本自动换行，不溢出 */
    lv_obj_set_size(toast, ui_px(300), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(toast, ui_px(36), 0);
    lv_obj_set_style_bg_color(toast, theme_col(accent_hex), 0);
    lv_obj_set_style_radius(toast, ui_px(18), 0);

    lv_obj_t *lbl = lv_label_create(toast);
    lv_label_set_text(lbl, ui_tr(text));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, ui_px(300) - 2 * THEME_PAD);
    lv_obj_set_style_text_font(lbl, THEME_FONT_S, 0);   /* 不设会落到 montserrat_14，中文变方框 */
    lv_obj_set_style_text_color(lbl, theme_col(0xFFFFFF), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);

    lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, -ui_px(80));
    /* 停在常驻标题栏下方，避免遮挡标题 */
    ui_anim_to(toast, (lv_anim_exec_xcb_t)lv_obj_set_y, -ui_px(80), THEME_TITLEBAR_H + ui_px(6), 350, lv_anim_path_ease_out);
    ui_anim_after(2600, toast_slide_out, toast);
}

/* ---------- 转场 ----------
 * 用同步 lv_screen_load：动画版 lv_screen_load_anim 在动画期间活动屏仍是旧屏，
 * show() 里立即重建焦点组会收集到旧页面对象（新页无法聚焦、返回后编码器失灵）。 */
void ui_screen_push(lv_obj_t *scr)
{
    lv_screen_load(scr);
}

void ui_screen_pop(lv_obj_t *scr)
{
    lv_screen_load(scr);
}
