/* Bambu Cloud account sign-in and bound-printer picker. */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_anim.h"
#include "../ui_nav.h"
#include "../assets/icons.h"
#include "app_settings.h"
#include "bambu_cloud.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static lv_obj_t *lbl_status;
static lv_obj_t *lbl_detail;
static lv_obj_t *form_region;
static lv_obj_t *lbl_region;
static lv_obj_t *form_account_type;
static lv_obj_t *lbl_account_type;
static lv_obj_t *form_account;
static lv_obj_t *lbl_account_key;
static lv_obj_t *lbl_account;
static lv_obj_t *form_password;
static lv_obj_t *lbl_password;
static lv_obj_t *btn_code;
static lv_obj_t *lbl_code_icon;
static lv_obj_t *lbl_code_button;
static lv_obj_t *btn_password;
static lv_obj_t *code_row;
static lv_obj_t *lbl_code;
static lv_obj_t *btn_submit_code;
static lv_obj_t *btn_restart;
static lv_obj_t *device_list;
static lv_obj_t *btn_refresh;
static lv_obj_t *btn_logout;

static char account[BAMBU_CLOUD_ACCOUNT_MAX];
static char password[192];
static char code[32];
static int account_phone;
static bambu_cloud_snapshot_t shown;
static int shown_valid;

static void copy_text(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

/* Cloud workers report stable Simplified-Chinese message keys so every port
 * stays independent of the UI language.  Translate exact keys here and handle
 * the one formatted success message separately. */
static const char *localized_cloud_message(const bambu_cloud_snapshot_t *now,
                                           char *buf, size_t cap)
{
    const char *message = now->message;
    static const char found_prefix[] = "登录成功，找到 ";
    if (strncmp(message, found_prefix, sizeof(found_prefix) - 1) == 0) {
        snprintf(buf, cap, TR("登录成功，找到 %d 台打印机"), now->device_count);
        return buf;
    }

    /* Preserve an HTTP/Windows diagnostic suffix while translating its
     * stable leading message, e.g. "...（HTTP 401）". */
    const char *suffix = strstr(message, "（");
    if (suffix && suffix > message) {
        char key[160];
        size_t len = (size_t)(suffix - message);
        if (len >= sizeof(key)) len = sizeof(key) - 1;
        memcpy(key, message, len);
        key[len] = 0;
        const char *translated = TR(key);
        if (strcmp(translated, key) != 0) {
            snprintf(buf, cap, "%s %s", translated, suffix);
            return buf;
        }
    }
    return TR(message);
}

/* ---------- shared text editor ---------- */
static lv_obj_t *txt_overlay;
static lv_group_t *txt_nav_group;
static lv_obj_t *ta;
static char *edit_target;
static size_t edit_cap;
static lv_obj_t *edit_value;
static int edit_masked;

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void update_field(lv_obj_t *label, const char *value, int masked)
{
    if (!value[0]) {
        lv_label_set_text(label, TR("未设置"));
        return;
    }
    if (!masked) {
        lv_label_set_text(label, value);
        return;
    }
    char stars[18];
    size_t n = strlen(value);
    if (n > sizeof(stars) - 1) n = sizeof(stars) - 1;
    memset(stars, '*', n);
    stars[n] = 0;
    lv_label_set_text(label, stars);
}

static void close_text_dialog(void)
{
    if (!txt_overlay) return;
    ui_desktop_input_end();
    ui_nav_detach_scope(txt_overlay);
    lv_obj_delete(txt_overlay);
    txt_overlay = NULL;
    ui_nav_modal_end(txt_nav_group);
    txt_nav_group = NULL;
}

static void on_text_ready(lv_event_t *e)
{
    LV_UNUSED(e);
    copy_text(edit_target, edit_cap, lv_textarea_get_text(ta));
    update_field(edit_value, edit_target, edit_masked);
    close_text_dialog();
}

static void on_text_cancel(lv_event_t *e)
{
    LV_UNUSED(e);
    close_text_dialog();
}

static void open_text_dialog(const char *title, char *target, size_t cap,
                             lv_obj_t *value_label, int masked, int numeric)
{
    edit_target = target;
    edit_cap = cap;
    edit_value = value_label;
    edit_masked = masked;
    txt_nav_group = ui_nav_modal_begin();
    if (!txt_nav_group) return;

    txt_overlay = lv_obj_create(lv_layer_top());
    ui_nav_attach_scope(txt_overlay, txt_nav_group);
    lv_obj_remove_style_all(txt_overlay);
    lv_obj_set_size(txt_overlay, ui_scr_w(), ui_scr_h());
    lv_obj_set_style_bg_color(txt_overlay, theme_col(THEME_COL_BG), 0);
    lv_obj_set_style_bg_opa(txt_overlay, LV_OPA_COVER, 0);

    lv_obj_t *heading = theme_label(txt_overlay, title, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, ui_px(7));
    ta = lv_textarea_create(txt_overlay);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, (uint32_t)(cap - 1));
    lv_textarea_set_password_mode(ta, masked != 0);
    if (numeric) lv_textarea_set_accepted_chars(ta, "0123456789");
    lv_textarea_set_text(ta, target);
    lv_obj_set_style_text_font(ta, THEME_FONT_S, 0);
    lv_obj_set_width(ta, ui_px(300));
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, ui_px(32));
    lv_obj_add_event_cb(ta, on_text_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ta, on_text_cancel, LV_EVENT_CANCEL, NULL);

    lv_obj_t *kb = lv_keyboard_create(txt_overlay);
    lv_obj_set_size(kb, ui_scr_w(), ui_px(150));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, ui_font_icon(), LV_PART_ITEMS);
    if (numeric) lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, on_text_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_text_cancel, LV_EVENT_CANCEL, NULL);
    lv_group_remove_obj(ta);
    theme_focusable(kb);
    lv_group_focus_obj(kb);
    lv_group_set_editing(txt_nav_group, true);
    ui_desktop_textarea_begin(ta);
}

static lv_obj_t *make_field(lv_obj_t *parent, const char *key,
                            lv_obj_t **key_label, lv_obj_t **value,
                            int y, lv_event_cb_t cb)
{
    lv_obj_t *row = theme_action_card(parent);
    lv_obj_set_size(row, ui_content_w(), ui_px(32));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, ui_px(y));
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *k = theme_label(row, key, THEME_FONT_S, THEME_COL_TEXT);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
    if (key_label) *key_label = k;
    lv_obj_t *arrow = theme_label(row, LV_SYMBOL_RIGHT, THEME_FONT_ICON, THEME_COL_ACCENT);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
    *value = theme_label(row, TR("未设置"), THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_set_width(*value, ui_px(178));
    lv_label_set_long_mode(*value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(*value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align_to(*value, arrow, LV_ALIGN_OUT_LEFT_MID, -ui_px(5), 0);
    return row;
}

static void on_account(lv_event_t *e)
{
    LV_UNUSED(e);
    open_text_dialog(account_phone ? TR("手机号") : "Bambu Email",
                     account, sizeof(account), lbl_account, 0, account_phone);
}

static void on_password(lv_event_t *e)
{
    LV_UNUSED(e);
    open_text_dialog(TR("密码"), password, sizeof(password), lbl_password, 1, 0);
}

static void on_code(lv_event_t *e)
{
    LV_UNUSED(e);
    open_text_dialog(TR("验证码"), code, sizeof(code), lbl_code, 0, 1);
}

static void on_account_type(lv_event_t *e)
{
    LV_UNUSED(e);
    bambu_cloud_snapshot_t now;
    bambu_cloud_snapshot(&now);
    if (now.region != BAMBU_CLOUD_REGION_CHINA) return;
    account_phone = !account_phone;
    account[0] = 0;
    update_field(lbl_account, account, 0);
    shown_valid = 0;
}

static void on_region(lv_event_t *e)
{
    LV_UNUSED(e);
    bambu_cloud_snapshot_t now;
    bambu_cloud_snapshot(&now);
    bambu_cloud_region_t next = now.region == BAMBU_CLOUD_REGION_CHINA
                              ? BAMBU_CLOUD_REGION_GLOBAL
                              : BAMBU_CLOUD_REGION_CHINA;
    if (bambu_cloud_set_region(next)) {
        account_phone = next == BAMBU_CLOUD_REGION_CHINA;
        account[0] = 0;
        update_field(lbl_account, account, 0);
        shown_valid = 0;
    }
}

static int valid_account(void)
{
    if (!account[0]) return 0;
    if (!account_phone) return strchr(account, '@') != NULL;
    size_t n = strlen(account);
    if (n < 6 || n > 20) return 0;
    for (size_t i = 0; i < n; i++)
        if (account[i] < '0' || account[i] > '9') return 0;
    return 1;
}

static void on_request_code(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!valid_account()) {
        ui_toast(account_phone ? TR("请先输入手机号") : TR("请先输入邮箱"),
                 THEME_COL_WARN);
        return;
    }
    bool accepted = account_phone ? bambu_cloud_request_sms_code(account)
                                  : bambu_cloud_request_email_code(account);
    if (!accepted)
        ui_toast(TR("登录任务正在运行"), THEME_COL_WARN);
}

static void on_password_login(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!valid_account()) {
        ui_toast(account_phone ? TR("请先输入手机号") : TR("请先输入邮箱"),
                 THEME_COL_WARN);
        return;
    }
    if (!password[0]) {
        ui_toast(TR("请先输入密码"), THEME_COL_WARN);
        return;
    }
    if (!bambu_cloud_login_password(account, password))
        ui_toast(TR("登录任务正在运行"), THEME_COL_WARN);
    else {
        memset(password, 0, sizeof(password));
        update_field(lbl_password, password, 1);
    }
}

static void on_submit_code(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!code[0]) {
        ui_toast(TR("请先输入验证码"), THEME_COL_WARN);
        return;
    }
    if (!bambu_cloud_submit_code(code))
        ui_toast(TR("验证码提交失败"), THEME_COL_WARN);
    else {
        memset(code, 0, sizeof(code));
        update_field(lbl_code, code, 0);
    }
}

static void on_restart(lv_event_t *e)
{
    LV_UNUSED(e);
    bambu_cloud_logout();
    password[0] = 0;
    code[0] = 0;
    shown_valid = 0;
}

static void on_refresh(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!bambu_cloud_refresh_devices())
        ui_toast(TR("刷新任务正在运行"), THEME_COL_WARN);
}

static void on_logout(lv_event_t *e)
{
    LV_UNUSED(e);
    bambu_cloud_logout();
    account[0] = password[0] = code[0] = 0;
    shown_valid = 0;
}

static void on_device(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    bambu_cloud_snapshot_t now;
    bambu_cloud_snapshot(&now);
    if (index < 0 || index >= now.device_count) return;
    bambu_device_conf_t selected = {0};
    copy_text(selected.serial, sizeof(selected.serial), now.devices[index].serial);
    copy_text(selected.name, sizeof(selected.name), now.devices[index].name);
    copy_text(selected.model, sizeof(selected.model), now.devices[index].model);
    selected.valid = true;
    if (!settings_save_bambu_device(&selected)) {
        ui_toast(TR("保存失败"), THEME_COL_ERROR);
        return;
    }
    ui_toast(TR("已选择打印机"), THEME_COL_OK);
    shown_valid = 0;
}

static void rebuild_devices(const bambu_cloud_snapshot_t *now)
{
    lv_obj_clean(device_list);
    bambu_device_conf_t selected;
    settings_load_bambu_device(&selected);
    for (int i = 0; i < now->device_count; i++) {
        const bambu_cloud_device_t *dev = &now->devices[i];
        bool active = selected.valid && strcmp(selected.serial, dev->serial) == 0;
        if (active && (strcmp(selected.name, dev->name) != 0 ||
                       strcmp(selected.model, dev->model) != 0)) {
            copy_text(selected.name, sizeof(selected.name), dev->name);
            copy_text(selected.model, sizeof(selected.model), dev->model);
            settings_save_bambu_device(&selected);
        }
        lv_obj_t *card = theme_action_card(device_list);
        lv_obj_set_size(card, lv_pct(100), ui_px(42));
        lv_obj_set_style_pad_all(card, ui_px(5), 0);
        if (active) {
            lv_obj_set_style_bg_color(card, theme_col(THEME_COL_OK), 0);
            theme_focus_bg(card, THEME_COL_OK, LV_OPA_COVER);
        }
        lv_obj_add_event_cb(card, on_device, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = theme_label(card, dev->name, THEME_FONT_S,
                                     active ? THEME_COL_BG : THEME_COL_TEXT);
        lv_obj_set_width(name, ui_px(210));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, -ui_px(2));
        char detail[112];
        snprintf(detail, sizeof(detail), "%s%s%s", dev->model,
                 dev->model[0] ? " · " : "", dev->online ? TR("在线") : TR("离线"));
        lv_obj_t *sub = theme_label(card, detail, THEME_FONT_S,
                                    active ? THEME_COL_BG : THEME_COL_TEXT_DIM);
        lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 0, ui_px(2));
        if (active) {
            lv_obj_t *mark = theme_label(card, LV_SYMBOL_OK, THEME_FONT_ICON, THEME_COL_BG);
            lv_obj_align(mark, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }
}

static int snapshot_changed(const bambu_cloud_snapshot_t *now)
{
    return !shown_valid || now->state != shown.state || now->region != shown.region ||
           now->device_count != shown.device_count ||
           strcmp(now->account, shown.account) != 0 ||
           strcmp(now->message, shown.message) != 0 ||
           memcmp(now->devices, shown.devices, sizeof(now->devices)) != 0;
}

static void refresh(void)
{
    bambu_cloud_snapshot_t now;
    bambu_cloud_snapshot(&now);
    if (!snapshot_changed(&now)) return;
    bool form = now.state == BAMBU_CLOUD_SIGNED_OUT ||
                now.state == BAMBU_CLOUD_FAILED ||
                now.state == BAMBU_CLOUD_UNSUPPORTED;
    bool challenge = now.state == BAMBU_CLOUD_NEED_CODE ||
                     now.state == BAMBU_CLOUD_NEED_TFA;
    bool signed_in = now.state == BAMBU_CLOUD_SIGNED_IN;
    bool busy = now.state == BAMBU_CLOUD_BUSY;

    if (now.region == BAMBU_CLOUD_REGION_GLOBAL) account_phone = 0;
    update_field(lbl_account, account, 0);
    update_field(lbl_password, password, 1);
    update_field(lbl_code, code, 0);

    const char *status = TR("拓竹云登录");
    if (busy) status = TR("正在登录");
    else if (challenge) status = now.state == BAMBU_CLOUD_NEED_TFA
                                   ? TR("双重验证") : TR("验证码登录");
    else if (signed_in) status = TR("已登录");
    else if (now.state == BAMBU_CLOUD_FAILED) status = TR("登录失败");
    else if (now.state == BAMBU_CLOUD_UNSUPPORTED) status = TR("桌面版功能");
    lv_label_set_text(lbl_status, status);
    char localized_detail[256];
    lv_label_set_text(lbl_detail,
                      localized_cloud_message(&now, localized_detail,
                                              sizeof(localized_detail)));
    lv_label_set_text(lbl_region, now.region == BAMBU_CLOUD_REGION_CHINA
                                  ? TR("中国区") : TR("全球区"));
    lv_label_set_text(lbl_account_type, account_phone ? TR("手机号") : "Email");
    lv_label_set_text(lbl_account_key, account_phone ? TR("手机号") : "Email");
    lv_label_set_text(lbl_code_icon, account_phone ? LV_SYMBOL_CALL : LV_SYMBOL_ENVELOPE);
    lv_label_set_text(lbl_code_button, account_phone ? TR("短信验证码") : TR("邮箱验证码"));
    if (now.region == BAMBU_CLOUD_REGION_CHINA)
        lv_obj_remove_state(form_account_type, LV_STATE_DISABLED);
    else
        lv_obj_add_state(form_account_type, LV_STATE_DISABLED);

    set_hidden(form_region, !form);
    set_hidden(form_account_type, !form);
    set_hidden(form_account, !form);
    set_hidden(form_password, !form);
    set_hidden(btn_code, !form);
    set_hidden(btn_password, !form);
    set_hidden(code_row, !challenge);
    set_hidden(btn_submit_code, !challenge);
    set_hidden(btn_restart, !challenge);
    set_hidden(device_list, !signed_in);
    set_hidden(btn_refresh, !signed_in);
    set_hidden(btn_logout, !signed_in);

    if (signed_in) rebuild_devices(&now);
    if (busy) {
        set_hidden(form_region, true);
        set_hidden(form_account_type, true);
        set_hidden(form_account, true);
        set_hidden(form_password, true);
        set_hidden(btn_code, true);
        set_hidden(btn_password, true);
    }
    shown = now;
    shown_valid = 1;
}

static lv_obj_t *create(void)
{
    bambu_cloud_init();
    bambu_cloud_snapshot_t initial;
    bambu_cloud_snapshot(&initial);
    if (initial.state == BAMBU_CLOUD_SIGNED_IN && initial.account[0])
        copy_text(account, sizeof(account), initial.account);
    account_phone = initial.region == BAMBU_CLOUD_REGION_CHINA &&
                    strchr(account, '@') == NULL;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    lv_obj_t *logo = theme_img(scr, ui_icon(&img_bambu_logo_56, &img_bambu_logo_112), THEME_COL_TEXT);
    lv_obj_set_size(logo, ui_px(42), ui_px(42));
    lv_image_set_inner_align(logo, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(logo, LV_ALIGN_TOP_LEFT, ui_px(10), THEME_TITLEBAR_H + ui_px(5));
    lbl_status = theme_label(scr, "", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, ui_px(62), THEME_TITLEBAR_H + ui_px(6));
    lbl_detail = theme_label(scr, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_set_width(lbl_detail, ui_px(245));
    lv_label_set_long_mode(lbl_detail, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_detail, LV_ALIGN_TOP_LEFT, ui_px(62), THEME_TITLEBAR_H + ui_px(28));

    int gap = ui_px(6), half = (ui_content_w() - gap) / 2;
    form_region = theme_button(scr, NULL, NULL, 0);
    lv_obj_set_size(form_region, half, ui_px(28));
    lv_obj_align(form_region, LV_ALIGN_TOP_LEFT, ui_px(8), THEME_TITLEBAR_H + ui_px(50));
    lbl_region = theme_label(form_region, "", THEME_FONT_S, THEME_COL_TEXT);
    lv_obj_center(lbl_region);
    lv_obj_add_event_cb(form_region, on_region, LV_EVENT_CLICKED, NULL);
    form_account_type = theme_button(scr, NULL, NULL, 0);
    lv_obj_set_size(form_account_type, half, ui_px(28));
    lv_obj_align(form_account_type, LV_ALIGN_TOP_RIGHT, -ui_px(8),
                 THEME_TITLEBAR_H + ui_px(50));
    lbl_account_type = theme_label(form_account_type, "", THEME_FONT_S, THEME_COL_TEXT);
    lv_obj_center(lbl_account_type);
    lv_obj_add_event_cb(form_account_type, on_account_type, LV_EVENT_CLICKED, NULL);
    form_account = make_field(scr, "Email", &lbl_account_key, &lbl_account,
                              110, on_account);
    form_password = make_field(scr, TR("密码"), NULL, &lbl_password,
                               145, on_password);

    btn_code = theme_button(scr, NULL, NULL, 0);
    lv_obj_set_size(btn_code, half, ui_px(34));
    lv_obj_align(btn_code, LV_ALIGN_BOTTOM_LEFT, ui_px(8), -ui_px(6));
    lv_obj_add_event_cb(btn_code, on_request_code, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(btn_code, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_code, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_code, ui_px(4), 0);
    lbl_code_icon = theme_label(btn_code, LV_SYMBOL_ENVELOPE, THEME_FONT_ICON,
                                THEME_COL_TEXT);
    lbl_code_button = theme_label(btn_code, TR("邮箱验证码"), THEME_FONT_S,
                                  THEME_COL_TEXT);
    btn_password = theme_button(scr, NULL, NULL, 1);
    lv_obj_set_size(btn_password, half, ui_px(34));
    lv_obj_align(btn_password, LV_ALIGN_BOTTOM_RIGHT, -ui_px(8), -ui_px(6));
    lv_obj_add_event_cb(btn_password, on_password_login, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(btn_password, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_password, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_password, ui_px(4), 0);
    theme_img(btn_password, ui_icon(&img_link, &img_link_32), THEME_COL_TEXT);
    theme_label(btn_password, TR("密码登录"), THEME_FONT_S, THEME_COL_TEXT);

    code_row = make_field(scr, TR("验证码"), NULL, &lbl_code, 115, on_code);
    btn_submit_code = theme_button(scr, LV_SYMBOL_OK, TR("确认登录"), 1);
    lv_obj_set_size(btn_submit_code, ui_content_w(), ui_px(36));
    lv_obj_align(btn_submit_code, LV_ALIGN_TOP_MID, 0, ui_px(154));
    lv_obj_add_event_cb(btn_submit_code, on_submit_code, LV_EVENT_CLICKED, NULL);
    btn_restart = theme_button(scr, LV_SYMBOL_REFRESH, TR("重新登录"), 0);
    lv_obj_set_size(btn_restart, ui_content_w(), ui_px(32));
    lv_obj_align(btn_restart, LV_ALIGN_BOTTOM_MID, 0, -ui_px(6));
    lv_obj_add_event_cb(btn_restart, on_restart, LV_EVENT_CLICKED, NULL);

    device_list = lv_obj_create(scr);
    lv_obj_remove_style_all(device_list);
    lv_obj_set_size(device_list, ui_content_w(), ui_px(110));
    lv_obj_align(device_list, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(50));
    lv_obj_set_flex_flow(device_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(device_list, ui_px(4), 0);
    lv_obj_set_scroll_dir(device_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(device_list, LV_SCROLLBAR_MODE_AUTO);
    btn_refresh = theme_button(scr, LV_SYMBOL_REFRESH, TR("刷新"), 0);
    lv_obj_set_size(btn_refresh, half, ui_px(32));
    lv_obj_align(btn_refresh, LV_ALIGN_BOTTOM_LEFT, ui_px(8), -ui_px(6));
    lv_obj_add_event_cb(btn_refresh, on_refresh, LV_EVENT_CLICKED, NULL);
    btn_logout = theme_button(scr, LV_SYMBOL_CLOSE, TR("退出登录"), 0);
    lv_obj_set_size(btn_logout, half, ui_px(32));
    lv_obj_align(btn_logout, LV_ALIGN_BOTTOM_RIGHT, -ui_px(8), -ui_px(6));
    lv_obj_add_event_cb(btn_logout, on_logout, LV_EVENT_CLICKED, NULL);

    shown_valid = 0;
    refresh();
    return scr;
}

static void on_show(void)
{
    shown_valid = 0;
    refresh();
    bambu_cloud_snapshot_t now;
    bambu_cloud_snapshot(&now);
    if (now.state == BAMBU_CLOUD_SIGNED_IN && now.device_count == 0)
        bambu_cloud_refresh_devices();
}

panel_def_t panel_bambu_setup_def = {
    .name = "bambu_setup", .title = "拓竹连接",
    .create = create, .on_show = on_show, .on_tick = refresh, .hide_temps = 1,
};
