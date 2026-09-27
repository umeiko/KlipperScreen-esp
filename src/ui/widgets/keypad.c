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
static bool desktop_replace_pending;

static void keypad_close(int ok)
{
    keypad_cb_t done = cb;
    void *done_ud = ud;
    float value = (float)atof(buf);
    ui_desktop_input_end();
    ui_nav_detach_scope(overlay);
    lv_obj_delete(overlay);
    overlay = NULL;
    ui_nav_modal_end(nav_group);
    nav_group = NULL;
    if (done) done(value, ok, done_ud);
}

static void apply_key(const char *k)
{
    size_t len = strlen(buf);
    if (strcmp(k, "BS") == 0) {
        if (len) buf[len - 1] = 0;
    } else if (strcmp(k, "CLR") == 0) {
        buf[0] = 0;
    } else if (strcmp(k, "OK") == 0) {
        keypad_close(1);
        return;
    } else if (strcmp(k, "C") == 0) {
        keypad_close(0);
        return;
    } else {
        if (len >= sizeof(buf) - 1) return;
        if (k[0] == '.' && strchr(buf, '.')) return;
        if (len == 1 && buf[0] == '0' && k[0] != '.') {
            /* “0”只是零值本身：后续零无需追加，首个非零数字直接替换。 */
            if (k[0] != '0') strcpy(buf, k);
        } else if (k[0] == '.' && len == 0) {
            strcpy(buf, "0.");
        } else {
            strcat(buf, k);
        }
    }
    lv_label_set_text(lbl_value, buf[0] ? buf : "0");
}

static void on_key(lv_event_t *e)
{
    desktop_replace_pending = false;
    apply_key((const char *)lv_event_get_user_data(e));
}

static void on_desktop_input(ui_desktop_input_event_t event, const char *text, void *user_data)
{
    LV_UNUSED(user_data);
    if (event == UI_DESKTOP_INPUT_TEXT && text) {
        for (const char *p = text; *p; p++) {
            if ((*p >= '0' && *p <= '9') || *p == '.') {
                if (desktop_replace_pending) {
                    buf[0] = 0;
                    desktop_replace_pending = false;
                }
                char key[2] = {*p, 0};
                apply_key(key);
            }
        }
    } else if (event == UI_DESKTOP_INPUT_BACKSPACE || event == UI_DESKTOP_INPUT_DELETE) {
        if (desktop_replace_pending) {
            buf[0] = 0;
            desktop_replace_pending = false;
            lv_label_set_text(lbl_value, "0");
        } else {
            apply_key("BS");
        }
    } else if (event == UI_DESKTOP_INPUT_READY) {
        apply_key("OK");
    } else if (event == UI_DESKTOP_INPUT_CANCEL) {
        apply_key("C");
    }
}

void keypad_open(const char *title, float initial, keypad_cb_t callback, void *user_data)
{
    if (overlay) return;   /* 已打开 */
    cb = callback;
    ud = user_data;
    snprintf(buf, sizeof(buf), "%d", (int)(initial + 0.5f));
    desktop_replace_pending = true;
    nav_group = ui_nav_modal_begin();

    overlay = lv_obj_create(lv_layer_top());
    ui_nav_attach_scope(overlay, nav_group);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, ui_scr_w(), ui_scr_h());
    lv_obj_set_style_bg_color(overlay, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(overlay, LV_SCROLLBAR_MODE_OFF);

    const int margin = ui_px(8);
    const int gap = ui_gap(5);
    const int content_w = ui_scr_w() - margin * 2;
    const int header_h = ui_px(42);
    const int action_h = ui_px(38);
    const int grid_top = margin + header_h + gap;
    const int action_top = ui_scr_h() - margin - action_h;
    const int grid_h = action_top - gap - grid_top;
    const int key_w = (content_w - gap * 2) / 3;
    const int key_h = (grid_h - gap * 3) / 4;

    /* 固定头部：标题和值始终可见，不参与滚动。 */
    lv_obj_t *header = theme_card(overlay);
    lv_obj_set_size(header, content_w, header_h);
    lv_obj_set_pos(header, margin, margin);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_title = theme_label(header, title, THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, 0, 0);

    lbl_value = theme_label(header, buf, THEME_FONT_XL, THEME_COL_ACCENT);
    lv_obj_align(lbl_value, LV_ALIGN_RIGHT_MID, 0, 0);

    /* 三列数字区充分利用屏幕宽度；第四行提供清空、0、退格。 */
    static const char *keys[4][3] = {
        {"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {"CLR", "0", "BS"},
    };

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            const char *action = keys[r][c];
            const char *icon = strcmp(action, "BS") == 0 ? LV_SYMBOL_BACKSPACE :
                               strcmp(action, "CLR") == 0 ? LV_SYMBOL_TRASH : NULL;
            const char *text = icon ? NULL : action;
            lv_obj_t *b = theme_button(overlay, icon, text, 0);
            int x = margin + c * (key_w + gap);
            int w = c == 2 ? content_w - c * (key_w + gap) : key_w;
            lv_obj_set_size(b, w, key_h);
            lv_obj_set_pos(b, x, grid_top + r * (key_h + gap));
            lv_obj_add_event_cb(b, on_key, LV_EVENT_CLICKED, (void *)keys[r][c]);
            if (!icon) {
                lv_obj_t *label = lv_obj_get_child(b, 0);
                if (label) lv_obj_set_style_text_font(label, THEME_FONT_M, 0);
            }
        }
    }

    /* 底部操作区固定，不会被数字区挤走。 */
    int action_w = (content_w - gap) / 2;
    lv_obj_t *b_cancel = theme_button(overlay, LV_SYMBOL_CLOSE, "取消", 0);
    lv_obj_set_size(b_cancel, action_w, action_h);
    lv_obj_set_pos(b_cancel, margin, action_top);
    lv_obj_add_event_cb(b_cancel, on_key, LV_EVENT_CLICKED, "C");

    lv_obj_t *b_ok = theme_button(overlay, LV_SYMBOL_OK, "确定", 1);
    lv_obj_set_size(b_ok, content_w - action_w - gap, action_h);
    lv_obj_set_pos(b_ok, margin + action_w + gap, action_top);
    lv_obj_add_event_cb(b_ok, on_key, LV_EVENT_CLICKED, "OK");

    ui_desktop_input_begin(on_desktop_input, NULL);

    /* 不做入场动画：卡片 opa/scale 动画会强制 LVGL 建中间层缓冲（~100KB），
       ESP32 堆分配失败会把 lvgl 任务卡死（看门狗），直接显示即可 */
}
