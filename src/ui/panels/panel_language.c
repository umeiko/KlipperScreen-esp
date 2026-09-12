/*
 * 语言选择：每种语言使用一整行，兼顾触摸和旋钮操作。
 * 选择后写入配置并渐暗重启，让所有已创建面板统一应用新语言。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_anim.h"
#include "app_settings.h"
#include "bsp.h"
#include <stdint.h>

static lv_obj_t *rows[UI_LANG_COUNT];
static lv_obj_t *names[UI_LANG_COUNT];
static lv_obj_t *states[UI_LANG_COUNT];

static void refresh(void)
{
    ui_lang_t current = ui_lang_get();
    for (unsigned i = 0; i < ui_lang_count(); i++) {
        int selected = i == (unsigned)current;
        lv_obj_set_style_bg_color(rows[i],
            theme_col(selected ? THEME_COL_OK : THEME_COL_SURFACE), 0);
        theme_focus_bg(rows[i], selected ? THEME_COL_OK : THEME_COL_ACCENT,
                       selected ? LV_OPA_COVER : LV_OPA_30);
        lv_obj_set_style_text_color(names[i],
            theme_col(selected ? THEME_COL_BG : THEME_COL_TEXT), 0);
        lv_label_set_text(states[i], selected ? TR("当前") : "");
        lv_obj_set_style_text_color(states[i],
            theme_col(selected ? THEME_COL_BG : THEME_COL_TEXT_DIM), 0);
    }
}

static void on_language_click(lv_event_t *e)
{
    ui_lang_t selected = (ui_lang_t)(uintptr_t)lv_event_get_user_data(e);
    if (selected == ui_lang_get()) return;
    if (!settings_save_language(ui_lang_code(selected))) {
        ui_toast("保存失败", THEME_COL_ERROR);
        return;
    }

    ui_lang_set(selected);
    refresh();
    lv_refr_now(NULL);
    bsp_fade_out(1000);
    bsp_restart();
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    int y = THEME_TITLEBAR_H + ui_px(4);
    const int step = ui_px(39);
    for (unsigned i = 0; i < ui_lang_count(); i++) {
        rows[i] = theme_action_card(scr);
        lv_obj_set_size(rows[i], ui_content_w(), ui_px(38));
        lv_obj_align(rows[i], LV_ALIGN_TOP_MID, 0, y);
        lv_obj_clear_flag(rows[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(rows[i], on_language_click, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        names[i] = theme_label(rows[i], ui_lang_name((ui_lang_t)i),
                               THEME_FONT_M, THEME_COL_TEXT);
        lv_obj_align(names[i], LV_ALIGN_LEFT_MID, ui_px(2), 0);
        states[i] = theme_label(rows[i], "", THEME_FONT_S, THEME_COL_TEXT_DIM);
        lv_obj_align(states[i], LV_ALIGN_RIGHT_MID, -ui_px(4), 0);
        y += step;
    }

    refresh();
    return scr;
}

panel_def_t panel_language_def = {
    .name = "language", .title = "语言",
    .create = create,
    .on_show = refresh,
    .on_tick = NULL,
    .hide_temps = 1,
};
