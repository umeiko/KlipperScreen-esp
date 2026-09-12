/*
 * 无线网络：扫描列表 → 点选 AP → 密码弹层（textarea + keyboard）→
 * 连接中转圈弹层 → toast 反馈结果。系统调用走 bsp_wifi（三端实现）。
 */
#include "../theme.h"
#include "../lang.h"
#include "../ui_anim.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../assets/icons.h"
#include "bsp_wifi.h"
#include "app_settings.h"
#include <string.h>
#include <stdio.h>

static lv_obj_t *list;              /* AP 列表容器 */
static lv_obj_t *lbl_hint;          /* 扫描中/失败/空列表提示（挂在 scr 上，不被 list 清掉） */
static lv_obj_t *pwd_overlay;       /* 密码输入弹层 */
static lv_obj_t *conn_overlay;      /* 连接中转圈弹层 */
static lv_group_t *pwd_nav_group;
static lv_group_t *conn_nav_group;
static lv_obj_t *ta_pwd;
static char      sel_ssid[BSP_WIFI_SSID_MAX + 1];
static char      pwd_buf[BSP_WIFI_PASS_MAX + 1];   /* windows 实现要求密码在连接期间保持有效 */
static int       scanning;          /* 等待扫描结果中 */
static int       connecting;        /* 处于连接流程，tick 里轮询状态 */
static int       connect_ticks;     /* 连接超时兜底：30s 无结果视为失败 */

static bsp_wifi_ap_t aps[16];       /* 静态：行点击事件要引用，不能放栈上 */

static void show_hint(const char *text)
{
    lv_label_set_text(lbl_hint, ui_tr(text));
    lv_obj_remove_flag(lbl_hint, LV_OBJ_FLAG_HIDDEN);
}

static void start_scan(void)
{
    bsp_wifi_scan_start();
    scanning = 1;
    lv_obj_clean(list);
    show_hint("扫描中…");
}

static void refresh_row_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    start_scan();
}

/* ---------- 连接结果弹层 ---------- */

static void conn_overlay_close(void)
{
    if (conn_overlay) {
        ui_nav_detach_scope(conn_overlay);
        lv_obj_delete(conn_overlay);
        conn_overlay = NULL;
        ui_nav_modal_end(conn_nav_group);
        conn_nav_group = NULL;
    }
    connecting = 0;
}

static void start_connect(const char *ssid, const char *pwd)
{
    strncpy(sel_ssid, ssid, sizeof(sel_ssid) - 1);
    sel_ssid[sizeof(sel_ssid) - 1] = 0;
    strncpy(pwd_buf, pwd ? pwd : "", sizeof(pwd_buf) - 1);
    pwd_buf[sizeof(pwd_buf) - 1] = 0;
    bsp_wifi_connect(sel_ssid, pwd_buf);
    connecting = 1;
    connect_ticks = 0;
    conn_nav_group = ui_nav_modal_begin();

    conn_overlay = lv_obj_create(lv_layer_top());
    ui_nav_attach_scope(conn_overlay, conn_nav_group);
    lv_obj_remove_style_all(conn_overlay);
    lv_obj_set_size(conn_overlay, ui_scr_w(), ui_scr_h());
    lv_obj_set_style_bg_color(conn_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(conn_overlay, LV_OPA_60, 0);

    lv_obj_t *card = theme_card(conn_overlay);
    lv_obj_set_size(card, ui_px(180), ui_px(110));
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, ui_px(10), 0);

    lv_obj_t *sp = lv_spinner_create(card);
    lv_obj_set_size(sp, ui_px(44), ui_px(44));
    lv_obj_set_style_arc_color(sp, theme_col(THEME_COL_ACCENT), LV_PART_INDICATOR);

    char msg[64];
    snprintf(msg, sizeof(msg), TR("正在连接 %s"), sel_ssid);
    theme_label(card, msg, THEME_FONT_S, THEME_COL_TEXT);
}

/* ---------- 密码输入弹层 ---------- */

static void pwd_overlay_close(void)
{
    if (pwd_overlay) {
        ui_desktop_input_end();
        ui_nav_detach_scope(pwd_overlay);
        lv_obj_delete(pwd_overlay);
        pwd_overlay = NULL;
        ui_nav_modal_end(pwd_nav_group);
        pwd_nav_group = NULL;
    }
}

static void on_kb_ready(lv_event_t *e)
{
    LV_UNUSED(e);
    char ssid[BSP_WIFI_SSID_MAX + 1];
    char pwd[BSP_WIFI_PASS_MAX + 1];
    /* 必须先拷出来再关弹层：textarea 随弹层删除，直接拿指针会读到已释放内存 */
    strncpy(ssid, sel_ssid, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = 0;
    strncpy(pwd, lv_textarea_get_text(ta_pwd), sizeof(pwd) - 1);
    pwd[sizeof(pwd) - 1] = 0;
    pwd_overlay_close();
    start_connect(ssid, pwd);
}

static void on_kb_cancel(lv_event_t *e)
{
    LV_UNUSED(e);
    pwd_overlay_close();
}

static void open_password_dialog(const char *ssid)
{
    strncpy(sel_ssid, ssid, sizeof(sel_ssid) - 1);
    sel_ssid[sizeof(sel_ssid) - 1] = 0;
    pwd_nav_group = ui_nav_modal_begin();

    pwd_overlay = lv_obj_create(lv_layer_top());
    ui_nav_attach_scope(pwd_overlay, pwd_nav_group);
    lv_obj_remove_style_all(pwd_overlay);
    lv_obj_set_size(pwd_overlay, ui_scr_w(), ui_scr_h());
    lv_obj_set_style_bg_color(pwd_overlay, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_bg_opa(pwd_overlay, LV_OPA_COVER, 0);

    char title[64];
    snprintf(title, sizeof(title), TR("连接到 %s"), ssid);
    lv_obj_t *lbl = theme_label(pwd_overlay, title, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, ui_px(8));

    ta_pwd = lv_textarea_create(pwd_overlay);
    lv_obj_set_style_text_font(ta_pwd, THEME_FONT_S, 0);   /* 占位符是中文，默认 montserrat 会变方框 */
    lv_textarea_set_one_line(ta_pwd, true);
    lv_textarea_set_max_length(ta_pwd, BSP_WIFI_PASS_MAX);
    /* 不回显掩码：电阻屏点按本来就难，明文便于确认输没输对 */
    lv_textarea_set_placeholder_text(ta_pwd, TR("密码"));
    lv_obj_add_event_cb(ta_pwd, on_kb_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ta_pwd, on_kb_cancel, LV_EVENT_CANCEL, NULL);
    lv_obj_set_width(ta_pwd, ui_px(300));
    lv_obj_align(ta_pwd, LV_ALIGN_TOP_MID, 0, ui_px(34));

    lv_obj_t *kb = lv_keyboard_create(pwd_overlay);
    lv_obj_set_size(kb, ui_scr_w(), ui_px(150));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    /* 按键字符随屏幕档位放大：用 montserrat 图标档（16→32），
       不能用 font_cjk —— 键盘的 确定/退格 等是 LV_SYMBOL 字形，CJK 字体不含会变方框 */
    lv_obj_set_style_text_font(kb, ui_font_icon(), LV_PART_ITEMS);
    lv_keyboard_set_textarea(kb, ta_pwd);
    lv_obj_add_event_cb(kb, on_kb_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_kb_cancel, LV_EVENT_CANCEL, NULL);
    /* The encoder edits the keyboard; the textarea only displays its text. */
    lv_group_remove_obj(ta_pwd);
    theme_focusable(kb);
    lv_group_focus_obj(kb);
    lv_group_set_editing(pwd_nav_group, true);
    ui_desktop_textarea_begin(ta_pwd);
}

/* ---------- AP 列表 ---------- */

static void on_ap_clicked(lv_event_t *e)
{
    const bsp_wifi_ap_t *ap = lv_event_get_user_data(e);
    if (ap->secure) open_password_dialog(ap->ssid);
    else            start_connect(ap->ssid, NULL);
}

static void add_ap_row(int idx)
{
    const bsp_wifi_ap_t *ap = &aps[idx];

    lv_obj_t *row = theme_action_card(list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, ui_px(44));
    lv_obj_add_event_cb(row, on_ap_clicked, LV_EVENT_CLICKED, (void *)ap);

    lv_obj_t *ssid = theme_label(row, ap->ssid, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(ssid, LV_ALIGN_LEFT_MID, ui_px(2), 0);
    lv_obj_set_width(ssid, ui_px(190));
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_SCROLL_CIRCULAR);

    /* 右侧：加密/开放 + 信号强度四档图标 */
    lv_obj_t *sec = theme_label(row, ap->secure ? "加密" : "开放", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(sec, LV_ALIGN_RIGHT_MID, -ui_px(34), 0);

    const lv_image_dsc_t *ic = ap->rssi > -55 ? &img_wifi_4 :
                               ap->rssi > -62 ? &img_wifi_3 :
                               ap->rssi > -72 ? &img_wifi_2 : &img_wifi_1;
    lv_obj_t *sig = theme_img(row, ic, THEME_COL_TEXT);
    lv_obj_align(sig, LV_ALIGN_RIGHT_MID, -ui_px(6), 0);
}

static void build_list_from(int n)
{
    lv_obj_clean(list);

    lv_obj_t *btn = theme_button(list, LV_SYMBOL_REFRESH, "重新扫描", 0);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, ui_px(40));
    lv_obj_add_event_cb(btn, refresh_row_clicked, LV_EVENT_CLICKED, NULL);

    if (n <= 0) {
        show_hint(n == 0 ? "未发现网络" : "扫描失败，点列表上方重试");
        return;
    }
    lv_obj_add_flag(lbl_hint, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < n; i++) add_ap_row(i);
}

static void tick(void)
{
    if (connecting) {
        /* 超时兜底：正常失败会有 DISCONNECTED→FAILED，防御状态机卡死 */
        if (++connect_ticks > 30) {
            ui_toast("连接超时", THEME_COL_ERROR);
            conn_overlay_close();
            return;
        }
        switch (bsp_wifi_status()) {
        case BSP_WIFI_CONNECTED: {
            char msg[64];
            snprintf(msg, sizeof(msg), TR("已连接 %s"), sel_ssid);
            ui_toast(msg, THEME_COL_OK);
            /* 凭据落盘 network.conf，下次开机自动回连（wc 已清零，截断也安全） */
            wifi_conf_t wc = {0};
            snprintf(wc.ssid, sizeof(wc.ssid), "%s", sel_ssid);
            snprintf(wc.pass, sizeof(wc.pass), "%s", pwd_buf);
            wc.valid = true;
            settings_save_wifi(&wc);
            conn_overlay_close();
            break;
        }
        case BSP_WIFI_FAILED:
            ui_toast("连接失败，请检查密码", THEME_COL_ERROR);
            conn_overlay_close();
            break;
        default:
            break;   /* 还在转圈 */
        }
        return;
    }
    if (scanning) {
        int n = bsp_wifi_scan_poll(aps, 16);
        if (n == BSP_WIFI_SCAN_RUNNING) return;
        scanning = 0;
        build_list_from(n);
    }
}

static lv_obj_t *create(void)
{
    bsp_wifi_init();

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, ui_content_w(), ui_scr_h() - THEME_TITLEBAR_H - ui_px(10));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(4));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, ui_px(6), 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    lbl_hint = theme_label(scr, "扫描中…", THEME_FONT_M, THEME_COL_TEXT_DIM);
    lv_obj_set_width(lbl_hint, ui_content_w());
    lv_obj_set_style_text_align(lbl_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_hint, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(90));

    return scr;
}

panel_def_t panel_wifi_def = {
    .name = "wifi", .title = "无线网络", .title_s = "网络",
    .create = create,
    .on_show = start_scan,
    .on_tick = tick,
    .hide_temps = 1,
};
