/*
 * 设置 → OTA 升级：检查新版本 → 显示新版本号 → 确认更新 → 进度条。
 * 进度条由 ota_progress_ui 顶层遮罩自动显示（升级激活时）。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../widgets/confirm.h"
#include "ota_auto.h"
#include "esp_app_desc.h"

static lv_obj_t *s_lbl_state;    /* 状态行值标签 */
static lv_obj_t *s_row_update;   /* 更新行（有新版本才显示） */

static void refresh(void)
{
    const ota_check_state_t *cs = ota_check_state_get();
    const ota_progress_t *p = ota_progress_get();
    char buf[48];

    if (p->active) {
        lv_label_set_text(s_lbl_state, "升级中…");
        lv_obj_add_flag(s_row_update, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (cs->checking) {
        lv_label_set_text(s_lbl_state, "检查中…");
        lv_obj_add_flag(s_row_update, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (cs->result == 1) {
        snprintf(buf, sizeof(buf), "新版本 %s", cs->remote);
        lv_label_set_text(s_lbl_state, buf);
        lv_obj_clear_flag(s_row_update, LV_OBJ_FLAG_HIDDEN);
    } else if (cs->result == 0) {
        lv_label_set_text(s_lbl_state, "已是最新版本");
        lv_obj_add_flag(s_row_update, LV_OBJ_FLAG_HIDDEN);
    } else if (cs->result == -1) {
        lv_label_set_text(s_lbl_state, "检查失败");
        lv_obj_add_flag(s_row_update, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_lbl_state, "点击下方检查");
        lv_obj_add_flag(s_row_update, LV_OBJ_FLAG_HIDDEN);
    }
}

static void do_update(void *ud)
{
    (void)ud;
    if (ota_start_update() != 0) {
        lv_label_set_text(s_lbl_state, "启动失败");
        lv_obj_clear_flag(s_row_update, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_check(lv_event_t *e)
{
    LV_UNUSED(e);
    ota_check_start();
    refresh();
}

static void on_update(lv_event_t *e)
{
    LV_UNUSED(e);
    const ota_check_state_t *cs = ota_check_state_get();
    char msg[96];
    snprintf(msg, sizeof(msg), "更新到 %s？\n将下载固件并重启", cs->remote);
    confirm_open(msg, "更新", do_update, NULL);
}

static void on_tick(void)
{
    refresh();
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);

    int y = THEME_TITLEBAR_H + ui_px(4);
    const int step = ui_px(39);

    /* 当前版本（只读行） */
    const esp_app_desc_t *app = esp_app_get_description();
    const char *cur = (app && app->version[0]) ? app->version : "?";
    lv_obj_t *row_cur = theme_row(scr, "当前版本", cur, y);
    (void)row_cur;
    y += step;

    /* 检查新版本（链接行） */
    theme_row_link(scr, "检查新版本", "", y, on_check);
    y += step;

    /* 状态行（只读，动态文本；持有值标签以便刷新） */
    lv_obj_t *row_state = theme_card(scr);
    lv_obj_set_size(row_state, ui_content_w(), ui_px(38));
    lv_obj_align(row_state, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_t *k_state = theme_label(row_state, "状态", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(k_state, LV_ALIGN_LEFT_MID, ui_px(2), 0);
    s_lbl_state = theme_label(row_state, "点击下方检查", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(s_lbl_state, LV_ALIGN_RIGHT_MID, -ui_px(4), 0);
    lv_obj_add_flag(row_state, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row_state, LV_OBJ_FLAG_SCROLLABLE);
    theme_focusable(row_state);
    y += step;

    /* 更新（链接行，有新版本才显示；导航自动跳过隐藏项） */
    s_row_update = theme_row_link(scr, "更新", "", y, on_update);
    lv_obj_add_flag(s_row_update, LV_OBJ_FLAG_HIDDEN);
    y += step;

    return scr;
}

panel_def_t panel_ota_def = {
    .name = "ota", .title = "OTA 升级",
    .create = create,
    .on_show = NULL,
    .on_tick = on_tick,
};
