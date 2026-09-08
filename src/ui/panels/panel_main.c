/*
 * 主菜单：状态卡片 + 功能网格 + 急停/重启（对标 KlipperScreen main_menu）
 */
#include "../theme.h"
#include "../lang.h"
#include "../assets/icons.h"
#include "../panel_mgr.h"
#include "../ui_anim.h"
#include "printer.h"
#include "app_settings.h"
#include "../widgets/confirm.h"
#include <stdio.h>

static lv_obj_t *lbl_state;
static lv_obj_t *img_state;
static lv_obj_t *lbl_file;
static lv_obj_t *card_status;

static void on_menu(lv_event_t *e)
{
    panel_mgr_open((const char *)lv_event_get_user_data(e));
}

static void do_estop(void *ud)
{
    LV_UNUSED(ud);
    printer_emergency_stop();   /* 真实实现：Moonraker printer.emergency_stop */
    ui_toast("已急停（M112）", THEME_COL_ERROR);
}

static void do_restart(void *ud)
{
    LV_UNUSED(ud);
    printer_firmware_restart(); /* 真实实现：Moonraker printer.firmware_restart */
    ui_toast("已发送重启指令", THEME_COL_WARN);
}

static void on_estop(lv_event_t *e)
{
    LV_UNUSED(e);
    confirm_open("确认急停？\n打印机将立即停止所有运动和加热", "急停", do_estop, NULL);
}

static void on_restart(lv_event_t *e)
{
    LV_UNUSED(e);
    confirm_open("确认重启下位机？\n（FIRMWARE_RESTART）", "重启", do_restart, NULL);
}

static void update_state(void)
{
    static const char *st_text[] = {
        [PRINTER_STATE_STANDBY]  = "空闲",
        [PRINTER_STATE_PRINTING] = "打印中",
        [PRINTER_STATE_PAUSED]   = "已暂停",
        [PRINTER_STATE_COMPLETE] = "打印完成",
    };
    printer_state_t s = printer_state();

    /* 整卡按状态着色：断连=黄 + 断链图标；Klipper 异常=红 + 感叹号；已连接=绿 + 链接图标 */
    uint32_t card_col;
    const lv_image_dsc_t *icon;
    switch (s) {
    case PRINTER_STATE_DISCONNECTED: card_col = THEME_COL_WARN;  icon = ui_icon(&img_link_off, &img_link_off_32);         break;
    case PRINTER_STATE_ERROR:        card_col = THEME_COL_ERROR; icon = ui_icon(&img_alert_circle, &img_alert_circle_32); break;
    default:                         card_col = THEME_COL_OK;    icon = ui_icon(&img_link, &img_link_32);                 break;
    }
    lv_obj_set_style_bg_color(card_status, theme_col(card_col), 0);
    lv_image_set_src(img_state, icon);
    lv_obj_set_style_image_recolor(img_state, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_text_color(lbl_state, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_text_color(lbl_file, theme_col(THEME_COL_BG), 0);

    if (s == PRINTER_STATE_DISCONNECTED) {
        /* 断连高可见性：附目标地址 */
        moonraker_conf_t mc;
        settings_load_moonraker(&mc);
        char buf[96];
        snprintf(buf, sizeof(buf), "%s: %.40s:%u", TR("未连接 Moonraker"),
                 mc.valid ? mc.host : "?", mc.port);
        lv_label_set_text(lbl_state, buf);
        lv_obj_set_style_text_font(lbl_state, THEME_FONT_S, 0);
        lv_label_set_text(lbl_file, "");
        return;
    }

    lv_obj_set_style_text_font(lbl_state, THEME_FONT_M, 0);
    lv_label_set_text(lbl_state, s == PRINTER_STATE_ERROR ? TR("Klipper 异常") : TR(st_text[s]));
    if (s == PRINTER_STATE_PRINTING || s == PRINTER_STATE_PAUSED) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s  %d.%d%%", printer_filename(),
                 printer_progress_permille() / 10, printer_progress_permille() % 10);
        lv_label_set_text(lbl_file, buf);
    } else {
        lv_label_set_text(lbl_file, "");
    }
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    /* 状态卡片 */
    card_status = theme_card(scr);
    lv_obj_set_size(card_status, ui_content_w(), ui_px(40));
    lv_obj_align(card_status, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(4));
    /* 状态卡是纯展示（空闲时点击无操作），不参与编码器选中 */

    /* 状态图标（链接/断链/感叹号，着色随卡片底色反色） */
    img_state = theme_img(card_status, ui_icon(&img_link, &img_link_32), THEME_COL_BG);
    lv_obj_align(img_state, LV_ALIGN_LEFT_MID, 0, 0);

    lbl_state = theme_label(card_status, "", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(lbl_state, LV_ALIGN_LEFT_MID, ui_px(18), 0);

    lbl_file = theme_label(card_status, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(lbl_file, LV_ALIGN_RIGHT_MID, -ui_px(4), 0);

    /* 功能网格 3x2（KlipperScreen material-dark 图标；大屏用 56px 变体） */
    const struct { const lv_image_dsc_t *icon; const char *text, *panel; } items[] = {
        {ui_icon(&img_heater,   &img_heater_56),   "温度", "temperature"},
        {ui_icon(&img_move,     &img_move_56),     "移动", "move"},
        {ui_icon(&img_extrude,  &img_extrude_56),  "挤出", "extrude"},
        {ui_icon(&img_files,    &img_files_56),    "文件", "files"},
        {ui_icon(&img_printer,  &img_printer_56),  "打印", "job_status"},
        {ui_icon(&img_settings, &img_settings_56), "设置", "settings"},
    };
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_remove_style_all(grid);
    /* 网格撑满状态卡与底部按钮之间的空间，按钮尺寸由可用空间反推（大屏不再留大片空白） */
    int gap = ui_gap(6);
    int grid_y = THEME_TITLEBAR_H + ui_px(48);
    int grid_h = ui_scr_h() - grid_y - ui_px(36) - gap;
    lv_obj_set_size(grid, ui_content_w(), grid_h);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, grid_y);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    /* 居中 + 固定次线性格距：SPACE_BETWEEN 在 800 宽屏上会拉开近百 px 的空隙 */
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grid, gap, 0);
    lv_obj_set_style_pad_column(grid, gap, 0);

    int bw = (ui_content_w() - 2 * gap) / 3;
    int bh = (grid_h - gap) / 2;
    for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        lv_obj_t *b = theme_menu_button_img(grid, items[i].icon, items[i].text);
        lv_obj_set_size(b, bw, bh);
        lv_obj_add_event_cb(b, on_menu, LV_EVENT_CLICKED, (void *)items[i].panel);
    }

    /* 底部：急停（高优先级，红色实心）+ 重启下位机 */
    int bw2 = (ui_content_w() - gap) / 2;
    lv_obj_t *b_estop = theme_button(scr, LV_SYMBOL_WARNING, "急停", 0);
    lv_obj_set_style_bg_color(b_estop, theme_col(THEME_COL_ERROR), 0);
    lv_obj_set_size(b_estop, bw2, ui_px(28));
    lv_obj_align(b_estop, LV_ALIGN_BOTTOM_LEFT, ui_px(8), -ui_px(4));
    lv_obj_add_event_cb(b_estop, on_estop, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b_restart = theme_button(scr, LV_SYMBOL_POWER, "重启下位机", 0);
    lv_obj_set_size(b_restart, bw2, ui_px(28));
    lv_obj_align(b_restart, LV_ALIGN_BOTTOM_RIGHT, -ui_px(8), -ui_px(4));
    lv_obj_add_event_cb(b_restart, on_restart, LV_EVENT_CLICKED, NULL);

    return scr;
}

panel_def_t panel_main_def = {
    .name = "main", .title = "",   /* 空标题 → 标题栏显示时钟 */
    .create = create,
    .on_show = update_state,
    .on_tick = update_state,
};
