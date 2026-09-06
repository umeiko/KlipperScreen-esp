#include "confirm.h"
#include "../theme.h"

static lv_obj_t *overlay;
static confirm_cb_t cb;
static void *ud;

static void close(int ok)
{
    if (ok && cb) cb(ud);
    lv_obj_delete(overlay);
    overlay = NULL;
}

static void on_ok(lv_event_t *e)     { LV_UNUSED(e); close(1); }
static void on_cancel(lv_event_t *e) { LV_UNUSED(e); close(0); }

static void on_overlay_click(lv_event_t *e)
{
    if (lv_event_get_target(e) == overlay) close(0);   /* 点遮罩取消 */
}

void confirm_open(const char *text, const char *ok_text, confirm_cb_t callback, void *user_data)
{
    if (overlay) return;   /* 已打开 */
    cb = callback;
    ud = user_data;

    overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, ui_scr_w(), ui_scr_h());
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, on_overlay_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = theme_card(overlay);
    lv_obj_set_size(card, ui_px(264), ui_px(128));
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = theme_label(card, text, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_set_width(lbl, ui_px(232));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, ui_px(232), ui_px(38));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *b_cancel = theme_button(row, LV_SYMBOL_CLOSE, "取消", 0);
    lv_obj_set_size(b_cancel, ui_px(110), ui_px(34));
    lv_obj_add_event_cb(b_cancel, on_cancel, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b_ok = theme_button(row, LV_SYMBOL_OK, ok_text ? ok_text : "确认", 0);
    lv_obj_set_style_bg_color(b_ok, theme_col(THEME_COL_ERROR), 0);
    lv_obj_set_size(b_ok, ui_px(110), ui_px(34));
    lv_obj_add_event_cb(b_ok, on_ok, LV_EVENT_CLICKED, NULL);
}
