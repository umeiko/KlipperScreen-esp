/* 拓竹连接方式：云端只读监视 / 局域网 Developer Mode 控制。 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_anim.h"
#include "app_settings.h"
#include <stdint.h>

#define LINK_COUNT 2

static lv_obj_t *cards[LINK_COUNT];
static lv_obj_t *icons[LINK_COUNT];
static lv_obj_t *states[LINK_COUNT];
static lv_obj_t *names[LINK_COUNT];
static lv_obj_t *details[LINK_COUNT];

static void refresh(void)
{
    bambu_link_t current = settings_load_bambu_link();
    for (int i = 0; i < LINK_COUNT; i++) {
        bool selected = i == (int)current;
        lv_obj_set_style_bg_color(cards[i],
            theme_col(selected ? THEME_COL_OK : THEME_COL_SURFACE), 0);
        theme_focus_bg(cards[i], selected ? THEME_COL_OK : THEME_COL_ACCENT,
                       selected ? LV_OPA_COVER : LV_OPA_30);
        lv_obj_set_style_text_color(icons[i],
            theme_col(selected ? THEME_COL_BG : THEME_COL_ACCENT), 0);
        lv_obj_set_style_text_color(names[i],
            theme_col(selected ? THEME_COL_BG : THEME_COL_TEXT), 0);
        lv_obj_set_style_text_color(details[i],
            theme_col(selected ? THEME_COL_BG : THEME_COL_TEXT_DIM), 0);
        lv_label_set_text(states[i], selected ? TR("当前") : "");
        lv_obj_set_style_text_color(states[i],
            theme_col(selected ? THEME_COL_BG : THEME_COL_TEXT_DIM), 0);
    }
}

static void on_link_click(lv_event_t *e)
{
    bambu_link_t link = (bambu_link_t)(intptr_t)lv_event_get_user_data(e);
    if (link == settings_load_bambu_link()) return;
    if (!settings_save_bambu_link(link)) {
        ui_toast(TR("保存失败"), THEME_COL_ERROR);
        return;
    }
    refresh();
    ui_toast(link == BAMBU_LINK_LAN ? TR("已切换到局域网控制")
                                    : TR("已切换到云端监视"),
             THEME_COL_OK);
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    static const char *symbols[LINK_COUNT] = { LV_SYMBOL_EYE_OPEN, LV_SYMBOL_HOME };
    static const char *name_texts[LINK_COUNT] = { "云端监视", "局域网控制" };
    static const char *detail_texts[LINK_COUNT] = { "状态 / 温度 / 进度", "Developer Mode" };
    int gap = ui_px(8);
    int card_w = (ui_content_w() - gap) / 2;
    int x0 = (ui_scr_w() - (2 * card_w + gap)) / 2;
    int y = THEME_TITLEBAR_H + ui_px(16);

    for (int i = 0; i < LINK_COUNT; i++) {
        cards[i] = theme_action_card(scr);
        lv_obj_set_size(cards[i], card_w, ui_px(142));
        lv_obj_set_pos(cards[i], x0 + i * (card_w + gap), y);
        lv_obj_clear_flag(cards[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(cards[i], on_link_click, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        icons[i] = theme_label(cards[i], symbols[i], THEME_FONT_ICON, THEME_COL_ACCENT);
        lv_obj_align(icons[i], LV_ALIGN_TOP_MID, 0, ui_px(12));
        names[i] = theme_label(cards[i], name_texts[i], THEME_FONT_S, THEME_COL_TEXT);
        lv_obj_set_size(names[i], card_w - ui_px(12), ui_px(18));
        lv_label_set_long_mode(names[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(names[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(names[i], LV_ALIGN_TOP_MID, 0, ui_px(40));
        details[i] = theme_label(cards[i], detail_texts[i], THEME_FONT_S, THEME_COL_TEXT_DIM);
        lv_obj_set_size(details[i], card_w - ui_px(12), ui_px(34));
        lv_label_set_long_mode(details[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(details[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(details[i], LV_ALIGN_TOP_MID, 0, ui_px(64));
        states[i] = theme_label(cards[i], "", THEME_FONT_S, THEME_COL_TEXT_DIM);
        lv_obj_set_size(states[i], card_w - ui_px(12), ui_px(18));
        lv_obj_set_style_text_align(states[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(states[i], LV_ALIGN_TOP_MID, 0, ui_px(110));
    }
    refresh();
    return scr;
}

panel_def_t panel_bambu_link_def = {
    .name = "bambu_link", .title = "连接方式",
    .create = create, .on_show = refresh, .on_tick = NULL, .hide_temps = 1,
};
