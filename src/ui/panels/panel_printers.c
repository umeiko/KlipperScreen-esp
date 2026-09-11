/*
 * 打印机槽位选择（打印机连接设置的三级页）：3×2 网格共 6 槽。
 * 已配置槽位按机器模式显示 Logo；点击某槽 → 设为当前连接槽（绿色高亮）并重连；
 * 返回二级页即可编辑该槽的主机/端口/API Key。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../assets/icons.h"
#include "app_settings.h"
#include "bambu_cloud.h"
#include "moonraker_client.h"
#include <stdio.h>
#include <string.h>

/* 基准 320x240 下的像素值，使用时经 ui_px() 换算 */
#define SLOT_W 148
#define SLOT_H 56
#define SLOT_GAP 8

static lv_obj_t *cards[PRINTER_SLOTS];
static lv_obj_t *logos[PRINTER_SLOTS];
static lv_obj_t *lbl_name[PRINTER_SLOTS];
static lv_obj_t *lbl_host[PRINTER_SLOTS];

static void refresh(void)
{
    int active = settings_load_active_printer();
    int slot_w = (ui_content_w() - ui_px(SLOT_GAP)) / 2;
    for (int i = 0; i < PRINTER_SLOTS; i++) {
        moonraker_conf_t c;
        bambu_device_conf_t bambu_device;
        settings_load_moonraker_slot(i, &c);
        settings_load_bambu_device_slot(i, &bambu_device);
        machine_mode_t mode = settings_load_machine_mode_slot(i);
        bambu_link_t link = settings_load_bambu_link_slot(i);

        char fallback_name[24];
        snprintf(fallback_name, sizeof(fallback_name), TR("打印机 %d"), i + 1);
        const char *display_name = mode == MACHINE_MODE_BAMBU
            ? (bambu_device.name[0] ? bambu_device.name : fallback_name)
            : (c.name[0] ? c.name : fallback_name);
        lv_label_set_text(lbl_name[i], display_name);
        const char *address = c.host[0] ? c.host :
            (mode == MACHINE_MODE_BAMBU && link == BAMBU_LINK_CLOUD_MONITOR
                ? TR("云端") : TR("未设置"));
        lv_label_set_text(lbl_host[i], address);

        lv_image_set_src(logos[i], mode == MACHINE_MODE_BAMBU
            ? ui_icon(&img_bambu_logo_56, &img_bambu_logo_112)
            : ui_icon(&img_klipper_logo_56, &img_klipper_logo_112));

        /* 选择了拓竹模式即显示其 Logo；账号或局域网参数可稍后配置。 */
        bool show_logo = c.host[0] || mode == MACHINE_MODE_BAMBU;
        if (show_logo) lv_obj_clear_flag(logos[i], LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(logos[i], LV_OBJ_FLAG_HIDDEN);
        int text_x = show_logo ? ui_px(40) : 0;
        lv_obj_set_width(lbl_name[i], slot_w - text_x - ui_px(8));
        lv_obj_align(lbl_name[i], LV_ALIGN_TOP_LEFT, text_x, 0);
        lv_obj_set_width(lbl_host[i], slot_w - ui_px(show_logo ? 60 : 20));
        lv_obj_align(lbl_host[i], LV_ALIGN_BOTTOM_LEFT, text_x, 0);

        /* 当前槽整卡变绿，其余恢复默认卡片色 */
        int on = (i == active);
        lv_obj_set_style_bg_color(cards[i],
            theme_col(on ? THEME_COL_OK : THEME_COL_SURFACE), 0);
        theme_focus_bg(cards[i], on ? THEME_COL_OK : THEME_COL_ACCENT,
                       on ? LV_OPA_COVER : LV_OPA_30);
        lv_obj_set_style_text_color(lbl_name[i],
            theme_col(on ? THEME_COL_BG : THEME_COL_TEXT), 0);
        lv_obj_set_style_text_color(lbl_host[i],
            theme_col(on ? THEME_COL_BG : THEME_COL_TEXT_DIM), 0);
        if (mode == MACHINE_MODE_KLIPPER) {
            /* Klipper 标志保留官方红灰双色。 */
            lv_obj_set_style_image_recolor_opa(logos[i], LV_OPA_TRANSP, 0);
        } else {
            lv_obj_set_style_image_recolor_opa(logos[i], LV_OPA_COVER, 0);
            lv_obj_set_style_image_recolor(logos[i], lv_color_hex(0xFFFFFF), 0);
        }
    }
}

static void on_slot_click(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (slot == settings_load_active_printer()) return;
    settings_save_active_printer(slot);
    if (settings_load_machine_mode() == MACHINE_MODE_BAMBU) {
        /* 已保存的云会话可直接复用；切槽后立刻刷新所选设备状态。 */
        bambu_cloud_init();
        if (settings_load_bambu_link() == BAMBU_LINK_CLOUD_MONITOR)
            bambu_cloud_refresh_devices();
    } else {
        moonraker_reload();   /* 立即重连新的 Klipper 槽位 */
    }
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
        lv_obj_t *card = theme_action_card(scr);
        lv_obj_set_size(card, slot_w, ui_px(SLOT_H));
        lv_obj_set_pos(card, x0 + col * (slot_w + gap),
                       THEME_TITLEBAR_H + ui_px(6) + row * (ui_px(SLOT_H) + gap));
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(card, on_slot_click, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        logos[i] = theme_img(card, ui_icon(&img_klipper_logo_56, &img_klipper_logo_112),
                             THEME_COL_ACCENT);
        lv_image_set_scale(logos[i], 146);   /* 56→约 32px；大屏 112→约 64px */
        /* 缩放以图像中心为轴，按原始边界对齐会留下 12/24px 空白。 */
        lv_obj_align(logos[i], LV_ALIGN_LEFT_MID, -ui_px(12), 0);

        lbl_name[i] = theme_label(card, "", THEME_FONT_M, THEME_COL_TEXT);
        lv_label_set_long_mode(lbl_name[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl_name[i], LV_ALIGN_TOP_LEFT, ui_px(40), 0);
        lbl_host[i] = theme_label(card, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
        lv_obj_set_width(lbl_host[i], slot_w - ui_px(60));
        lv_label_set_long_mode(lbl_host[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl_host[i], LV_ALIGN_BOTTOM_LEFT, ui_px(40), 0);

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
