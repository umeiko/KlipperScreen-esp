/*
 * 打印机槽位选择（Moonraker 设置的三级页）：3×2 网格共 6 槽。
 * 点击某槽 → 设为当前连接槽（绿色高亮）并立即重连 Moonraker；
 * 返回二级页即可编辑该槽的主机/端口/API Key。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "app_settings.h"
#include "moonraker_client.h"
#include <stdio.h>
#include <string.h>

/* 基准 320x240 下的像素值，使用时经 ui_px() 换算 */
#define SLOT_W 148
#define SLOT_H 56
#define SLOT_GAP 8

static lv_obj_t *cards[PRINTER_SLOTS];
static lv_obj_t *lbl_name[PRINTER_SLOTS];
static lv_obj_t *lbl_host[PRINTER_SLOTS];

static void refresh(void)
{
    int active = settings_load_active_printer();
    for (int i = 0; i < PRINTER_SLOTS; i++) {
        moonraker_conf_t c;
        settings_load_moonraker_slot(i, &c);

        char name[24];
        snprintf(name, sizeof(name), TR("打印机 %d"), i + 1);
        lv_label_set_text(lbl_name[i], name);
        lv_label_set_text(lbl_host[i], c.host[0] ? c.host : TR("未设置"));

        /* 当前槽整卡变绿，其余恢复默认卡片色 */
        int on = (i == active);
        lv_obj_set_style_bg_color(cards[i],
            theme_col(on ? THEME_COL_OK : THEME_COL_SURFACE), 0);
        lv_obj_set_style_text_color(lbl_name[i],
            theme_col(on ? THEME_COL_BG : THEME_COL_TEXT), 0);
        lv_obj_set_style_text_color(lbl_host[i],
            theme_col(on ? THEME_COL_BG : THEME_COL_TEXT_DIM), 0);
    }
}

static void on_slot_click(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (slot == settings_load_active_printer()) return;
    settings_save_active_printer(slot);
    moonraker_reload();   /* 立即重连新槽位 */
    refresh();
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    /* 槽宽按内容区动态分配（320 基准下 148，与原来一致），网格水平居中 */
    int gap = ui_px(SLOT_GAP);
    int slot_w = (ui_content_w() - gap) / 2;
    int x0 = (ui_scr_w() - (2 * slot_w + gap)) / 2;

    for (int i = 0; i < PRINTER_SLOTS; i++) {
        int col = i % 2, row = i / 2;
        lv_obj_t *card = theme_card(scr);
        lv_obj_set_size(card, slot_w, ui_px(SLOT_H));
        lv_obj_set_pos(card, x0 + col * (slot_w + gap),
                       THEME_TITLEBAR_H + ui_px(6) + row * (ui_px(SLOT_H) + gap));
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(card, on_slot_click, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lbl_name[i] = theme_label(card, "", THEME_FONT_M, THEME_COL_TEXT);
        lv_obj_align(lbl_name[i], LV_ALIGN_TOP_LEFT, 0, 0);
        lbl_host[i] = theme_label(card, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
        lv_obj_set_width(lbl_host[i], slot_w - ui_px(20));
        lv_label_set_long_mode(lbl_host[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl_host[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);

        cards[i] = card;
    }
    refresh();
    return scr;
}

panel_def_t panel_printers_def = {
    .name = "printers", .title = "切换打印机",
    .create = create,
    .on_show = refresh,
    .on_tick = NULL,
    .hide_temps = 1,
};
