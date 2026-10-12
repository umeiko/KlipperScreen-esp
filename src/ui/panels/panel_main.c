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
static lv_obj_t *lbl_progress;
static lv_obj_t *lbl_file;
static lv_obj_t *lbl_printer;
static lv_obj_t *card_status;
static lv_obj_t *menu_btns[6];
static lv_obj_t *btn_estop;
static lv_obj_t *btn_restart;

static void set_available(lv_obj_t *obj, bool available)
{
    if (!obj) return;
    if (available) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
        lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
        lv_obj_set_style_opa(obj, LV_OPA_30, 0);
    }
}

static void on_menu(lv_event_t *e)
{
    panel_mgr_open((const char *)lv_event_get_user_data(e));
}

static void on_status_click(lv_event_t *e)
{
    LV_UNUSED(e);
    if (printer_state() == PRINTER_STATE_PRINTING || printer_state() == PRINTER_STATE_PAUSED)
        panel_mgr_open("job_status");
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
    bool has_job_detail = s == PRINTER_STATE_PRINTING || s == PRINTER_STATE_PAUSED;
    lv_obj_set_flag(lbl_progress, LV_OBJ_FLAG_HIDDEN, !has_job_detail);
    lv_obj_set_flag(lbl_file, LV_OBJ_FLAG_HIDDEN, !has_job_detail);
    bool bambu = settings_load_machine_mode() == MACHINE_MODE_BAMBU;
    bool cloud = bambu && settings_load_bambu_link() == BAMBU_LINK_CLOUD_MONITOR;
    moonraker_conf_t mc;
    settings_load_moonraker(&mc);
    char fallback_name[24], endpoint[80], identity[160];
    const char *display_name = fallback_name;
    snprintf(fallback_name, sizeof(fallback_name), TR("打印机 %d"),
             settings_load_active_printer() + 1);
    if (bambu) {
        bambu_device_conf_t device;
        bool has_device = settings_load_bambu_device(&device);
        if (has_device && device.name[0]) display_name = device.name;
        snprintf(endpoint, sizeof(endpoint), "%s · %s",
                 has_device && device.model[0] ? device.model : TR("拓竹"),
                 cloud ? TR("云端") : "LAN");
    } else {
        if (mc.name[0]) display_name = mc.name;
        snprintf(endpoint, sizeof(endpoint), "%s",
                 mc.host[0] ? mc.host : TR("未设置"));
    }
    snprintf(identity, sizeof(identity), "%s · %s", display_name, endpoint);
    lv_label_set_text(lbl_printer, identity);

    /* 温度和打印页始终保留为监视入口；其余入口由后端能力开放。 */
    set_available(menu_btns[0], true);
    set_available(menu_btns[1], printer_has_capability(PRINTER_CAP_MOVE));
    set_available(menu_btns[2], printer_has_capability(PRINTER_CAP_EXTRUDE));
    set_available(menu_btns[3], printer_has_capability(PRINTER_CAP_FILES));
    set_available(menu_btns[4], true);
    set_available(menu_btns[5], true);
    set_available(btn_estop, printer_has_capability(PRINTER_CAP_EMERGENCY_STOP));
    set_available(btn_restart, printer_has_capability(PRINTER_CAP_FIRMWARE_RESTART));

    /* 整卡按状态着色：断连=黄 + 断链图标；Klipper 异常=红 + 感叹号；已连接=绿 + 链接图标 */
    uint32_t card_col;
    const lv_image_dsc_t *icon;
    switch (s) {
    case PRINTER_STATE_DISCONNECTED: card_col = THEME_COL_WARN;  icon = ui_icon(&img_link_off, &img_link_off_32);         break;
    case PRINTER_STATE_ERROR:        card_col = THEME_COL_ERROR; icon = ui_icon(&img_alert_circle, &img_alert_circle_32); break;
    default:                         card_col = THEME_COL_OK;    icon = ui_icon(&img_link, &img_link_32);                 break;
    }
    lv_obj_set_style_bg_color(card_status, theme_col(card_col), 0);
    theme_focus_bg(card_status, card_col, LV_OPA_COVER);
    lv_image_set_src(img_state, icon);
    lv_obj_set_style_image_recolor(img_state, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_text_color(lbl_state, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_text_color(lbl_progress, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_text_color(lbl_file, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_text_color(lbl_printer, theme_col(THEME_COL_BG), 0);

    if (s == PRINTER_STATE_DISCONNECTED) {
        /* 目标地址已在卡片第二行展示，首行只保留高可见状态。 */
        char buf[96];
        if (bambu) snprintf(buf, sizeof(buf), "%s", TR("未连接拓竹"));
        else
            snprintf(buf, sizeof(buf), "%s", TR("未连接 Moonraker"));
        lv_label_set_text(lbl_state, buf);
        lv_obj_set_style_text_font(lbl_state, THEME_FONT_S, 0);
        return;
    }

    lv_obj_set_style_text_font(lbl_state, THEME_FONT_M, 0);
    if (s == PRINTER_STATE_ERROR) {
        lv_label_set_text(lbl_state, bambu ? TR("打印机异常") : TR("Klipper 异常"));
    } else {
        lv_label_set_text(lbl_state, TR(st_text[s]));
    }
    if (s == PRINTER_STATE_PRINTING || s == PRINTER_STATE_PAUSED) {
        char progress[16];
        int permille = printer_progress_permille();
        snprintf(progress, sizeof(progress), "%d.%d%%", permille / 10, permille % 10);
        lv_label_set_text(lbl_progress, progress);
        lv_label_set_text(lbl_file, printer_filename());
    } else {
        lv_label_set_text(lbl_progress, "");
    }
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    /* 状态卡片 */
    card_status = theme_action_card(scr);
    lv_obj_set_size(card_status, ui_content_w(), ui_px(48));
    lv_obj_align(card_status, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(4));
    lv_obj_add_event_cb(card_status, on_status_click, LV_EVENT_CLICKED, NULL);

    /* 左侧状态区用轻微深色底和竖线形成稳定边界；右侧文本无论如何滚动，
       都不会在视觉上与状态/进度连成一句。 */
    lv_obj_t *status_zone = lv_obj_create(card_status);
    lv_obj_remove_style_all(status_zone);
    lv_obj_remove_flag(status_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(status_zone, ui_px(100), ui_px(32));
    lv_obj_align(status_zone, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(status_zone, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_bg_opa(status_zone, LV_OPA_10, 0);
    lv_obj_set_style_radius(status_zone, ui_px(7), 0);

    lv_obj_t *divider = lv_obj_create(card_status);
    lv_obj_remove_style_all(divider);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(divider, ui_px(1), ui_px(26));
    lv_obj_align(divider, LV_ALIGN_LEFT_MID, ui_px(104), 0);
    lv_obj_set_style_bg_color(divider, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_30, 0);

    /* 状态图标（链接/断链/感叹号，着色随卡片底色反色） */
    img_state = theme_img(card_status, ui_icon(&img_link, &img_link_32), THEME_COL_BG);
    lv_obj_align(img_state, LV_ALIGN_TOP_LEFT, 0, ui_px(1));

    lbl_state = theme_label(card_status, "", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_set_width(lbl_state, ui_px(76));
    lv_label_set_long_mode(lbl_state, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(lbl_state, LV_ALIGN_TOP_LEFT, ui_px(18), 0);

    lbl_progress = theme_label(card_status, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_set_width(lbl_progress, ui_px(76));
    lv_obj_align(lbl_progress, LV_ALIGN_BOTTOM_LEFT, ui_px(18), 0);

    lbl_file = theme_label(card_status, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    int inner_w = ui_content_w() - 2 * THEME_PAD;
    lv_obj_set_width(lbl_file, inner_w - ui_px(116));
    lv_label_set_long_mode(lbl_file, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(lbl_file, LV_ALIGN_TOP_LEFT, ui_px(112), ui_px(1));

    lbl_printer = theme_label(card_status, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_set_width(lbl_printer, inner_w - ui_px(116));
    lv_label_set_long_mode(lbl_printer, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(lbl_printer, LV_ALIGN_BOTTOM_LEFT, ui_px(112), 0);

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
    int grid_y = THEME_TITLEBAR_H + ui_px(56);
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
        menu_btns[i] = theme_menu_button_img(grid, items[i].icon, items[i].text);
        lv_obj_set_size(menu_btns[i], bw, bh);
        lv_obj_add_event_cb(menu_btns[i], on_menu, LV_EVENT_CLICKED, (void *)items[i].panel);
    }

    /* 底部：急停（高优先级，红色实心）+ 重启下位机 */
    int bw2 = (ui_content_w() - gap) / 2;
    btn_estop = theme_button(scr, LV_SYMBOL_WARNING, "急停", 0);
    lv_obj_set_style_bg_color(btn_estop, theme_col(THEME_COL_ERROR), 0);
    theme_focus_bg(btn_estop, THEME_COL_ERROR, LV_OPA_COVER);
    lv_obj_set_size(btn_estop, bw2, ui_px(28));
    lv_obj_align(btn_estop, LV_ALIGN_BOTTOM_LEFT, ui_px(8), -ui_px(4));
    lv_obj_add_event_cb(btn_estop, on_estop, LV_EVENT_CLICKED, NULL);

    btn_restart = theme_button(scr, LV_SYMBOL_POWER, "重启下位机", 0);
    lv_obj_set_size(btn_restart, bw2, ui_px(28));
    lv_obj_align(btn_restart, LV_ALIGN_BOTTOM_RIGHT, -ui_px(8), -ui_px(4));
    lv_obj_add_event_cb(btn_restart, on_restart, LV_EVENT_CLICKED, NULL);

    return scr;
}

panel_def_t panel_main_def = {
    .name = "main", .title = "",   /* 空标题 → 标题栏显示时钟 */
    .create = create,
    .on_show = update_state,
    .on_tick = update_state,
};
