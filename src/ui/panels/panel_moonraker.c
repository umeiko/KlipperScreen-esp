/*
 * 打印机连接设置：机器模式 + Klipper/Moonraker 槽位、主机、端口与 API Key。
 * 最多 6 台打印机槽位（"切换打印机"行 → printers 面板选择）；编辑对象为当前槽。
 * 配置持久化到 moonraker.conf（app_settings → bsp_conf）。
 * 文本输入弹层复用 panel_wifi 密码弹层的 textarea + keyboard 模式。
 */
#include "../theme.h"
#include "../lang.h"
#include "../ui_anim.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../widgets/keypad.h"
#include "../assets/icons.h"
#include "app_settings.h"
#include "moonraker_client.h"
#include "printer.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

static moonraker_conf_t cfg;        /* 工作副本（当前槽），保存时才落盘 */
static lv_obj_t *lbl_machine_mode;
static lv_obj_t *lbl_switch;
static lv_obj_t *lbl_host;
static lv_obj_t *lbl_port;
static lv_obj_t *lbl_key;
static lv_obj_t *lbl_status;

/* ---------- 文本输入弹层 ---------- */
static lv_obj_t *txt_overlay;
static lv_group_t *txt_nav_group;
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
    if (txt_overlay) {
        ui_nav_detach_scope(txt_overlay);
        lv_obj_delete(txt_overlay);
        txt_overlay = NULL;
        ui_nav_modal_end(txt_nav_group);
        txt_nav_group = NULL;
    }
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
    txt_nav_group = ui_nav_modal_begin();

    txt_overlay = lv_obj_create(lv_layer_top());
    ui_nav_attach_scope(txt_overlay, txt_nav_group);
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
    /* The encoder edits the keyboard; the textarea only displays its text. */
    lv_group_remove_obj(ta);
    theme_focusable(kb);
    lv_group_focus_obj(kb);
    lv_group_set_editing(txt_nav_group, true);
}

/* ---------- 旋钮专用 IPv4 编辑弹层 ---------- */
static lv_obj_t *ip_overlay;
static lv_group_t *ip_nav_group;
static lv_obj_t *ip_field[4];
static lv_obj_t *ip_value_label[4];
static lv_obj_t *ip_hint;
static lv_obj_t *ip_btn_save;
static int ip_value[4];
static int ip_edit_index = -1;
static int ip_speed_score;
static int ip_step_size = 1;
static uint32_t ip_last_step_at;

static void ip_render_field(int index)
{
    lv_label_set_text_fmt(ip_value_label[index], "%d", ip_value[index]);
}

static void ip_render_hint(void)
{
    if (ip_edit_index >= 0)
        lv_label_set_text_fmt(ip_hint, "OCTET %d/4   STEP %d", ip_edit_index + 1, ip_step_size);
    else
        lv_label_set_text(ip_hint, "OK");
}

static int ip_accelerated_step(uint32_t now)
{
    uint32_t dt = ip_last_step_at ? lv_tick_elaps(ip_last_step_at) : 1000;
    ip_last_step_at = now;
    if (dt <= 90) ip_speed_score += 3;
    else if (dt <= 170) ip_speed_score += 2;
    else if (dt <= 280) ip_speed_score += 1;
    else ip_speed_score = 0;
    if (ip_speed_score > 10) ip_speed_score = 10;
    return ip_speed_score >= 8 ? 10 : ip_speed_score >= 5 ? 5 : ip_speed_score >= 3 ? 2 : 1;
}

static void ip_begin_edit(int index)
{
    if (index < 0 || index >= 4) return;
    if (ip_nav_group && lv_group_get_editing(ip_nav_group))
        lv_group_set_editing(ip_nav_group, false);
    ip_edit_index = index;
    ip_speed_score = 0;
    ip_step_size = 1;
    ip_last_step_at = 0;
    lv_group_focus_obj(ip_field[index]);
    lv_group_set_editing(ip_nav_group, true);
    ip_render_hint();
}

static void ip_finish_octet(void)
{
    if (ip_edit_index < 0) return;
    int finished = ip_edit_index;
    ip_edit_index = -1;
    lv_group_set_editing(ip_nav_group, false);
    if (finished < 3) {
        ip_begin_edit(finished + 1);
    } else {
        lv_group_focus_obj(ip_btn_save);
        ip_render_hint();
    }
}

static void ip_overlay_close(void)
{
    if (!ip_overlay) return;
    ui_nav_detach_scope(ip_overlay);
    lv_obj_delete(ip_overlay);
    ip_overlay = NULL;
    ip_edit_index = -1;
    ui_nav_modal_end(ip_nav_group);
    ip_nav_group = NULL;
}

static void on_ip_field(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_KEY && ip_edit_index == index) {
        uint32_t key = lv_event_get_key(e);
        if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
            ip_step_size = ip_accelerated_step(lv_tick_get());
            ip_value[index] += key == LV_KEY_RIGHT ? ip_step_size : -ip_step_size;
            if (ip_value[index] < 0) ip_value[index] = 0;
            if (ip_value[index] > 255) ip_value[index] = 255;
            ip_render_field(index);
            ip_render_hint();
        } else if (key == LV_KEY_ENTER) {
            ip_finish_octet();
        }
    } else if (code == LV_EVENT_CLICKED) {
        if (ip_edit_index == index)
            ip_finish_octet();
        else
            ip_begin_edit(index);
    }
}

static void on_ip_save(lv_event_t *e)
{
    LV_UNUSED(e);
    snprintf(cfg.host, sizeof(cfg.host), "%d.%d.%d.%d",
             ip_value[0], ip_value[1], ip_value[2], ip_value[3]);
    refresh_row(lbl_host, cfg.host, 0);
    ip_overlay_close();
}

static void on_ip_cancel(lv_event_t *e)
{
    LV_UNUSED(e);
    ip_overlay_close();
}

static void open_ip_dialog(void)
{
    if (ip_overlay) return;

    int a, b, c, d;
    char trailing;
    if (sscanf(cfg.host, "%d.%d.%d.%d%c", &a, &b, &c, &d, &trailing) == 4 &&
        a >= 0 && a <= 255 && b >= 0 && b <= 255 &&
        c >= 0 && c <= 255 && d >= 0 && d <= 255) {
        ip_value[0] = a; ip_value[1] = b; ip_value[2] = c; ip_value[3] = d;
    } else {
        ip_value[0] = 192; ip_value[1] = 168; ip_value[2] = 1; ip_value[3] = 100;
    }

    ip_nav_group = ui_nav_modal_begin();
    if (!ip_nav_group) return;

    ip_overlay = lv_obj_create(lv_layer_top());
    ui_nav_attach_scope(ip_overlay, ip_nav_group);
    lv_obj_remove_style_all(ip_overlay);
    lv_obj_set_size(ip_overlay, ui_scr_w(), ui_scr_h());
    lv_obj_set_style_bg_color(ip_overlay, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_bg_opa(ip_overlay, LV_OPA_COVER, 0);

    lv_obj_t *title = theme_label(ip_overlay, "IPv4 地址", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, ui_px(8));
    ip_hint = theme_label(ip_overlay, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(ip_hint, LV_ALIGN_TOP_MID, 0, ui_px(31));

    int field_w = ui_px(58);
    int dot_w = ui_px(10);
    int total_w = field_w * 4 + dot_w * 3;
    int x0 = (ui_scr_w() - total_w) / 2;
    int field_y = ui_px(57);
    for (int i = 0; i < 4; i++) {
        ip_field[i] = theme_action_card(ip_overlay);
        lv_obj_set_size(ip_field[i], field_w, ui_px(66));
        lv_obj_set_pos(ip_field[i], x0 + i * (field_w + dot_w), field_y);
        lv_obj_set_style_bg_color(ip_field[i], theme_col(THEME_COL_ACCENT),
                                  LV_STATE_FOCUS_KEY | LV_STATE_EDITED);
        lv_obj_set_style_bg_opa(ip_field[i], LV_OPA_30,
                                LV_STATE_FOCUS_KEY | LV_STATE_EDITED);
        lv_obj_set_style_outline_width(ip_field[i], ui_px(3),
                                       LV_STATE_FOCUS_KEY | LV_STATE_EDITED);
        lv_obj_add_event_cb(ip_field[i], on_ip_field, LV_EVENT_ALL, (void *)(intptr_t)i);

        ip_value_label[i] = theme_label(ip_field[i], "", THEME_FONT_L, THEME_COL_TEXT);
        lv_obj_center(ip_value_label[i]);
        ip_render_field(i);

        if (i < 3) {
            lv_obj_t *dot = theme_label(ip_overlay, ".", THEME_FONT_L, THEME_COL_TEXT_DIM);
            lv_obj_set_width(dot, dot_w);
            lv_obj_set_style_text_align(dot, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(dot, x0 + field_w + i * (field_w + dot_w), field_y + ui_px(23));
        }
    }

    int gap = ui_gap(8);
    int btn_w = (ui_content_w() - gap) / 2;
    /* Save is registered immediately after the fields, so reverse rotation
       from it returns directly to the fourth octet for a quick correction. */
    ip_btn_save = theme_button(ip_overlay, LV_SYMBOL_OK, "确定", 1);
    lv_obj_set_size(ip_btn_save, btn_w, ui_px(38));
    lv_obj_align(ip_btn_save, LV_ALIGN_BOTTOM_RIGHT, -ui_px(8), -ui_px(13));
    lv_obj_add_event_cb(ip_btn_save, on_ip_save, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel = theme_button(ip_overlay, LV_SYMBOL_CLOSE, "取消", 0);
    lv_obj_set_size(cancel, btn_w, ui_px(38));
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, ui_px(8), -ui_px(13));
    lv_obj_add_event_cb(cancel, on_ip_cancel, LV_EVENT_CLICKED, NULL);

    ip_begin_edit(0);
}

/* ---------- 行点击 ---------- */
static void on_host_click(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER)
        open_ip_dialog();
    else
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
    lv_obj_t *row = theme_action_card(parent);
    lv_obj_set_size(row, ui_content_w(), ui_px(38));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);

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
    lv_label_set_text(lbl_machine_mode,
        settings_load_machine_mode() == MACHINE_MODE_BAMBU ? TR("拓竹") : "Klipper");

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

static void on_machine_mode_click(lv_event_t *e)
{
    LV_UNUSED(e);
    panel_mgr_open("machine_mode");
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);   /* 机器模式 + 连接项超出 240 高，允许上下滚动 */

    int y = THEME_TITLEBAR_H + ui_px(6);
    lv_obj_t *r;
    r = make_row(scr, "机器模式", &lbl_machine_mode, y, NULL);
    lv_obj_add_event_cb(r, on_machine_mode_click, LV_EVENT_CLICKED, NULL);
    r = make_row(scr, "切换打印机", &lbl_switch, y + ui_px(44), ui_icon(&img_swap_16, &img_swap_32));
    lv_obj_add_event_cb(r, on_switch_click, LV_EVENT_CLICKED, NULL);
    r = make_row(scr, "主机", &lbl_host, y + ui_px(88), NULL);
    lv_obj_add_event_cb(r, on_host_click, LV_EVENT_CLICKED, NULL);
    r = make_row(scr, "端口", &lbl_port, y + ui_px(132), NULL);
    lv_obj_add_event_cb(r, on_port_click, LV_EVENT_CLICKED, NULL);
    r = make_row(scr, "API Key", &lbl_key, y + ui_px(176), NULL);
    lv_obj_add_event_cb(r, on_key_click, LV_EVENT_CLICKED, NULL);

    /* 连接状态行（不可点） */
    lv_obj_t *srow = theme_card(scr);
    lv_obj_set_size(srow, ui_content_w(), ui_px(38));
    lv_obj_align(srow, LV_ALIGN_TOP_MID, 0, y + ui_px(220));
    lv_obj_t *k = theme_label(srow, "状态", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, ui_px(2), 0);
    lbl_status = theme_label(srow, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(lbl_status, LV_ALIGN_RIGHT_MID, -ui_px(4), 0);

    lv_obj_t *btn = theme_button(scr, LV_SYMBOL_SAVE, "保存并连接", 1);
    lv_obj_set_size(btn, ui_content_w(), ui_px(36));
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y + ui_px(266));
    lv_obj_add_event_cb(btn, on_save_click, LV_EVENT_CLICKED, NULL);

    on_show();   /* 读当前槽并刷新行 */
    return scr;
}

panel_def_t panel_moonraker_def = {
    .name = "moonraker", .title = "打印机连接设置",
    .create = create,
    .on_show = on_show,
    .on_tick = tick,
    .hide_temps = 1,   /* 标题长，关掉右侧温度避免遮挡 */
};
