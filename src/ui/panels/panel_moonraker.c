/*
 * Moonraker 连接设置：槽位/主机/端口/API Key 编辑 + 连接状态 + 保存并连接。
 * 最多 6 台打印机槽位（"切换打印机"行 → printers 面板选择）；编辑对象为当前槽。
 * 配置持久化到 moonraker.conf（app_settings → bsp_conf）。
 * 文本输入弹层复用 panel_wifi 密码弹层的 textarea + keyboard 模式。
 */
#include "../theme.h"
#include "../lang.h"
#include "../ui_anim.h"
#include "../panel_mgr.h"
#include "../widgets/keypad.h"
#include "../assets/icons.h"
#include "app_settings.h"
#include "moonraker_client.h"
#include "printer.h"
#include <string.h>
#include <stdio.h>

static moonraker_conf_t cfg;        /* 工作副本（当前槽），保存时才落盘 */
static lv_obj_t *lbl_switch;
static lv_obj_t *lbl_host;
static lv_obj_t *lbl_port;
static lv_obj_t *lbl_key;
static lv_obj_t *lbl_status;

/* ---------- 文本输入弹层 ---------- */
static lv_obj_t *txt_overlay;
static lv_obj_t *ta;
static char  *edit_target;          /* 指向 cfg.host 或 cfg.api_key */
static size_t edit_cap;
static lv_obj_t **edit_label;       /* 完成后刷新的行标签 */
static int   edit_masked;

static void tick(void);

static void refresh_row(lv_obj_t *lbl, const char *val, int masked)
{
    if (!masked) {
        lv_label_set_text(lbl, val[0] ? val : TR("未设置"));
        return;
    }
    /* API Key 掩码显示 */
    char m[24];
    strncpy(m, TR("未设置"), sizeof(m) - 1);
    m[sizeof(m) - 1] = 0;
    if (val[0]) {
        size_t n = strlen(val);
        memset(m, '*', n > 12 ? 12 : n);
        m[n > 12 ? 12 : n] = 0;
    }
    lv_label_set_text(lbl, m);
}

static void txt_overlay_close(void)
{
    if (txt_overlay) { lv_obj_delete(txt_overlay); txt_overlay = NULL; }
}

static void on_txt_ready(lv_event_t *e)
{
    LV_UNUSED(e);
    const char *v = lv_textarea_get_text(ta);
    strncpy(edit_target, v, edit_cap - 1);
    edit_target[edit_cap - 1] = 0;
    refresh_row(*edit_label, edit_target, edit_masked);
    txt_overlay_close();
}

static void on_txt_cancel(lv_event_t *e)
{
    LV_UNUSED(e);
    txt_overlay_close();
}

static void open_text_dialog(const char *title, char *target, size_t cap,
                             lv_obj_t **row_label, int masked)
{
    edit_target = target;
    edit_cap = cap;
    edit_label = row_label;
    edit_masked = masked;

    txt_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(txt_overlay);
    lv_obj_set_size(txt_overlay, ui_scr_w(), ui_scr_h());
    lv_obj_set_style_bg_color(txt_overlay, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_bg_opa(txt_overlay, LV_OPA_COVER, 0);

    lv_obj_t *lbl = theme_label(txt_overlay, title, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, ui_px(8));

    ta = lv_textarea_create(txt_overlay);
    lv_obj_set_style_text_font(ta, THEME_FONT_S, 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, target);
    lv_obj_set_width(ta, ui_px(300));
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, ui_px(34));

    lv_obj_t *kb = lv_keyboard_create(txt_overlay);
    lv_obj_set_size(kb, ui_scr_w(), ui_px(150));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    /* 按键字符随屏幕档位放大：用 montserrat 图标档（16→32），
       不能用 font_cjk —— 键盘的 确定/退格 等是 LV_SYMBOL 字形，CJK 字体不含会变方框 */
    lv_obj_set_style_text_font(kb, ui_font_icon(), LV_PART_ITEMS);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, on_txt_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_txt_cancel, LV_EVENT_CANCEL, NULL);
}

/* ---------- 行点击 ---------- */
static void on_host_click(lv_event_t *e)
{
    LV_UNUSED(e);
    open_text_dialog("Moonraker 主机（IP 或域名）", cfg.host, sizeof(cfg.host), &lbl_host, 0);
}

static void on_key_click(lv_event_t *e)
{
    LV_UNUSED(e);
    open_text_dialog("API Key（可留空）", cfg.api_key, sizeof(cfg.api_key), &lbl_key, 1);
}

static void on_port_done(float value, int ok, void *ud)
{
    LV_UNUSED(ud);
    if (!ok) return;
    if (value < 1 || value > 65535) return;
    cfg.port = (uint16_t)value;
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", (unsigned)cfg.port);
    lv_label_set_text(lbl_port, buf);
}

static void on_port_click(lv_event_t *e)
{
    LV_UNUSED(e);
    keypad_open("端口", cfg.port ? cfg.port : 7125, on_port_done, NULL);
}

static void on_save_click(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!cfg.host[0]) {
        ui_toast("请先填写主机地址", THEME_COL_ERROR);
        return;
    }
    cfg.valid = true;
    if (settings_save_moonraker(&cfg)) {
        ui_toast("已保存，正在连接", THEME_COL_OK);
        moonraker_reload();
    } else {
        ui_toast("保存失败", THEME_COL_ERROR);
    }
}

/* ---------- 界面 ---------- */
/* icon 非 NULL 时在行首文字前加 16px 小图标（参照设置页语言行） */
static lv_obj_t *make_row(lv_obj_t *parent, const char *key, lv_obj_t **val_lbl, int y,
                          const void *icon)
{
    lv_obj_t *row = theme_card(parent);
    lv_obj_set_size(row, ui_content_w(), ui_px(38));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    int text_x = ui_px(2);
    if (icon) {
        lv_obj_t *ic = theme_img(row, icon, THEME_COL_TEXT_DIM);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, ui_px(2), 0);
        text_x = ui_px(22);
    }
    lv_obj_t *k = theme_label(row, key, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, text_x, 0);

    *val_lbl = theme_label(row, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_set_width(*val_lbl, ui_px(200));
    lv_label_set_long_mode(*val_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(*val_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(*val_lbl, LV_ALIGN_RIGHT_MID, -ui_px(4), 0);
    return row;
}

static void update_rows(void)
{
    /* 槽位行：当前槽号 + 该槽主机 */
    char sw[80];
    snprintf(sw, sizeof(sw), "%d · %s", settings_load_active_printer() + 1,
             cfg.host[0] ? cfg.host : TR("未设置"));
    lv_label_set_text(lbl_switch, sw);
    refresh_row(lbl_host, cfg.host, 0);
    refresh_row(lbl_key, cfg.api_key, 1);
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", (unsigned)(cfg.port ? cfg.port : 7125));
    lv_label_set_text(lbl_port, buf);
}

static void on_show(void)
{
    /* 从槽位选择页返回时重读当前槽（可能刚切换过） */
    memset(&cfg, 0, sizeof(cfg));
    settings_load_moonraker(&cfg);
    if (!cfg.port) cfg.port = 7125;
    update_rows();
    tick();
}

static void tick(void)
{
    const char *s;
    uint32_t col;
    char buf[40];
    switch (moonraker_state()) {
    case MOONRAKER_READY:
        /* 已连接时附应用层心跳延迟（5s 一跳，0=还没测到） */
        if (printer_rtt_ms() > 0)
            snprintf(buf, sizeof(buf), TR("已连接 %dms"), printer_rtt_ms());
        else
            snprintf(buf, sizeof(buf), "%s", TR("已连接"));
        s = buf; col = THEME_COL_OK;
        break;
    case MOONRAKER_CONNECTING: s = TR("连接中…");   col = THEME_COL_WARN;  break;
    default:                   s = cfg.valid ? TR("离线（自动重连中）") : TR("未配置");
                               col = cfg.valid ? THEME_COL_WARN : THEME_COL_TEXT_DIM; break;
    }
    lv_label_set_text(lbl_status, s);
    lv_obj_set_style_text_color(lbl_status, theme_col(col), 0);
}

static void on_switch_click(lv_event_t *e)
{
    LV_UNUSED(e);
    panel_mgr_open("printers");
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);   /* 6 行超出 240 高，允许上下滚动 */

    int y = THEME_TITLEBAR_H + ui_px(6);
    lv_obj_t *r;
    r = make_row(scr, "切换打印机", &lbl_switch, y, ui_icon(&img_swap_16, &img_swap_32));
    lv_obj_add_event_cb(r, on_switch_click, LV_EVENT_CLICKED, NULL);
    r = make_row(scr, "主机", &lbl_host, y + ui_px(44), NULL);
    lv_obj_add_event_cb(r, on_host_click, LV_EVENT_CLICKED, NULL);
    r = make_row(scr, "端口", &lbl_port, y + ui_px(88), NULL);
    lv_obj_add_event_cb(r, on_port_click, LV_EVENT_CLICKED, NULL);
    r = make_row(scr, "API Key", &lbl_key, y + ui_px(132), NULL);
    lv_obj_add_event_cb(r, on_key_click, LV_EVENT_CLICKED, NULL);

    /* 连接状态行（不可点） */
    lv_obj_t *srow = theme_card(scr);
    lv_obj_set_size(srow, ui_content_w(), ui_px(38));
    lv_obj_align(srow, LV_ALIGN_TOP_MID, 0, y + ui_px(176));
    lv_obj_t *k = theme_label(srow, "状态", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, ui_px(2), 0);
    lbl_status = theme_label(srow, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(lbl_status, LV_ALIGN_RIGHT_MID, -ui_px(4), 0);

    lv_obj_t *btn = theme_button(scr, LV_SYMBOL_SAVE, "保存并连接", 1);
    lv_obj_set_size(btn, ui_content_w(), ui_px(36));
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y + ui_px(222));
    lv_obj_add_event_cb(btn, on_save_click, LV_EVENT_CLICKED, NULL);

    on_show();   /* 读当前槽并刷新行 */
    return scr;
}

panel_def_t panel_moonraker_def = {
    .name = "moonraker", .title = "Moonraker 连接",
    .create = create,
    .on_show = on_show,
    .on_tick = tick,
    .hide_temps = 1,   /* 标题长，关掉右侧温度避免遮挡 */
};
