/*
 * 机器模式选择：当前模式用绿色卡片标识；选择另一模式时先二次确认。
 * 连接后端根据保存的模式与能力表决定监视和控制范围。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_anim.h"
#include "../widgets/confirm.h"
#include "../assets/icons.h"
#include "app_settings.h"
#include <stdint.h>
#include <stdio.h>

#define MODE_COUNT 2

static const char *mode_names[MODE_COUNT] = { "Klipper", "拓竹" };
static const lv_image_dsc_t *mode_logos[MODE_COUNT];
static lv_obj_t *cards[MODE_COUNT];
static lv_obj_t *logos[MODE_COUNT];
static lv_obj_t *states[MODE_COUNT];
static machine_mode_t current_mode;

static void refresh(void)
{
    current_mode = settings_load_machine_mode();
    for (int i = 0; i < MODE_COUNT; i++) {
        int selected = i == (int)current_mode;
        lv_obj_set_style_bg_color(cards[i],
            theme_col(selected ? THEME_COL_OK : THEME_COL_SURFACE), 0);
        theme_focus_bg(cards[i], selected ? THEME_COL_OK : THEME_COL_ACCENT,
                       selected ? LV_OPA_COVER : LV_OPA_30);
        if (i == MACHINE_MODE_KLIPPER) {
            /* Klipper 标志保留官方红灰双色。 */
            lv_obj_set_style_image_recolor_opa(logos[i], LV_OPA_TRANSP, 0);
        } else {
            lv_obj_set_style_image_recolor_opa(logos[i], LV_OPA_COVER, 0);
            lv_obj_set_style_image_recolor(logos[i],
                theme_col(selected ? THEME_COL_BG : THEME_COL_TEXT), 0);
        }
        lv_label_set_text(states[i], selected ? TR("当前") : "");
        lv_obj_set_style_text_color(states[i],
            theme_col(selected ? THEME_COL_BG : THEME_COL_TEXT_DIM), 0);
    }
}

static void confirm_switch(void *ud)
{
    machine_mode_t mode = (machine_mode_t)(intptr_t)ud;
    if (!settings_save_machine_mode(mode)) {
        ui_toast("保存失败", THEME_COL_ERROR);
        return;
    }
    refresh();
}

static void on_mode_click(lv_event_t *e)
{
    machine_mode_t mode = (machine_mode_t)(intptr_t)lv_event_get_user_data(e);
    if (mode == current_mode) return;

    char text[64];
    snprintf(text, sizeof(text), TR("是否切换为 %s 型号？"), TR(mode_names[mode]));
    confirm_open(text, "切换", confirm_switch, (void *)(intptr_t)mode);
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    int gap = ui_px(8);
    int card_w = (ui_content_w() - gap) / 2;
    int x0 = (ui_scr_w() - (2 * card_w + gap)) / 2;
    int y = THEME_TITLEBAR_H + ui_px(22);
    mode_logos[MACHINE_MODE_KLIPPER] = ui_icon(&img_klipper_logo_56, &img_klipper_logo_112);
    mode_logos[MACHINE_MODE_BAMBU] = ui_icon(&img_bambu_logo_56, &img_bambu_logo_112);

    for (int i = 0; i < MODE_COUNT; i++) {
        cards[i] = theme_action_card(scr);
        lv_obj_set_size(cards[i], card_w, ui_px(132));
        lv_obj_set_pos(cards[i], x0 + i * (card_w + gap), y);
        lv_obj_clear_flag(cards[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(cards[i], on_mode_click, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        logos[i] = theme_img(cards[i], mode_logos[i], THEME_COL_TEXT);
        lv_obj_align(logos[i], LV_ALIGN_CENTER, 0, -ui_px(12));
        states[i] = theme_label(cards[i], "", THEME_FONT_S, THEME_COL_TEXT_DIM);
        lv_obj_align(states[i], LV_ALIGN_CENTER, 0, ui_px(28));
    }

    refresh();
    return scr;
}

panel_def_t panel_machine_mode_def = {
    .name = "machine_mode", .title = "机器模式", .title_s = "模式",
    .create = create,
    .on_show = refresh,
    .on_tick = NULL,
    .hide_temps = 1,
};
