#include "keypad.h"
#include "../theme.h"
#include "../ui_nav.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static lv_obj_t *overlay;
static lv_group_t *nav_group;
static lv_obj_t *lbl_value;
static keypad_cb_t cb;
static void *ud;
static char buf[16];
static lv_obj_t *card;

static void keypad_close(int ok)
{
    keypad_cb_t done = cb;
    void *done_ud = ud;
    float value = (float)atof(buf);
    ui_nav_detach_scope(overlay);
    lv_obj_delete(overlay);
    overlay = NULL;
    ui_nav_modal_end(nav_group);
    nav_group = NULL;
    if (done) done(value, ok, done_ud);
}

static void on_key(lv_event_t *e)
{
    const char *k = lv_event_get_user_data(e);
    size_t len = strlen(buf);
    if (strcmp(k, "BS") == 0) {
        if (len) buf[len - 1] = 0;
    } else if (strcmp(k, "OK") == 0) {
        keypad_close(1);
        return;
    } else if (strcmp(k, "C") == 0) {
        keypad_close(0);
        return;
    } else {
        if (len >= sizeof(buf) - 1) return;
        if (k[0] == '.' && strchr(buf, '.')) return;
        strcat(buf, k);
    }
    lv_label_set_text(lbl_value, buf[0] ? buf : "0");
}

static void on_overlay_click(lv_event_t *e)
{
    if (lv_event_get_target(e) == overlay) keypad_close(0);
}

void keypad_open(const char *title, float initial, keypad_cb_t callback, void *user_data)
{
    if (overlay) return;   /* 已打开 */
    cb = callback;
    ud = user_data;
    snprintf(buf, sizeof(buf), "%d", (int)(initial + 0.5f));
    nav_group = ui_nav_modal_begin();

    overlay = lv_obj_create(lv_layer_top());
    ui_nav_attach_scope(overlay, nav_group);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, ui_scr_w(), ui_scr_h());
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, on_overlay_click, LV_EVENT_CLICKED, NULL);

    card = theme_card(overlay);
    lv_obj_set_size(card, ui_px(236), ui_px(210));
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, ui_px(6), 0);

    lv_obj_t *lbl_title = theme_label(card, title, THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_value = theme_label(card, buf, THEME_FONT_L, THEME_COL_TEXT);
    lv_obj_align(lbl_value, LV_ALIGN_TOP_RIGHT, 0, 0);

    /* 数字区 4x3 */
    static const char *keys[4][3] = {
        {"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {".", "0", "BS"},
    };
    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, ui_px(216), ui_px(120));
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, ui_px(6), 0);

    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 3; c++) {
            lv_obj_t *b = theme_button(grid, NULL, keys[r][c], 0);
            lv_obj_set_size(b, ui_px(66), ui_px(26));
            lv_obj_add_event_cb(b, on_key, LV_EVENT_CLICKED, (void *)keys[r][c]);
        }

    /* 底部：取消 / 确认 */
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, ui_px(216), ui_px(30));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *b_cancel = theme_button(row, LV_SYMBOL_CLOSE, "取消", 0);
    lv_obj_set_size(b_cancel, ui_px(100), ui_px(28));
    lv_obj_add_event_cb(b_cancel, on_key, LV_EVENT_CLICKED, "C");

    lv_obj_t *b_ok = theme_button(row, LV_SYMBOL_OK, "确定", 1);
    lv_obj_set_size(b_ok, ui_px(100), ui_px(28));
    lv_obj_add_event_cb(b_ok, on_key, LV_EVENT_CLICKED, "OK");

    /* 不做入场动画：卡片 opa/scale 动画会强制 LVGL 建中间层缓冲（~100KB），
       ESP32 堆分配失败会把 lvgl 任务卡死（看门狗），直接显示即可 */
}
