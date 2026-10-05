/*
 * 检查更新（Linux 上位机 BSP_HAS_LINUX_HOST + Android）：状态卡 + 进度条 +
 * 底部动作区。worker 在后台跑（supd_*），本面板只在节拍里读快照；
 * 离开面板不取消——下载落在本地，回来可继续看进度。
 * 下载可暂停/取消；校验通过后要用户点"安装"并经二次确认——
 * Linux 原地替换二进制重启，Android 经 JNI 跳系统安装器。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "../ui_anim.h"
#include "../widgets/confirm.h"
#include "bsp_caps.h"

#if BSP_HAS_LINUX_HOST || defined(__ANDROID__)

#include "self_update.h"
#include "version.h"
#include "bsp.h"

#include <stdio.h>

static lv_obj_t *lbl_status;
static lv_obj_t *lbl_detail;
static lv_obj_t *bar;
static lv_obj_t *btn_main, *lbl_main;
static lv_obj_t *btn_cancel, *lbl_cancel;
static supd_state_t shown_state = SUPD_IDLE;
static int shown_progress = -1;

static void on_main(lv_event_t *e);
static void on_cancel(lv_event_t *e);
static void do_install(void *ud);

static void on_main(lv_event_t *e)
{
    (void)e;
    supd_status_t s;
    supd_poll(&s);
    switch (s.state) {
    case SUPD_IDLE:
    case SUPD_UPTODATE:
    case SUPD_ERROR:
        supd_check_start();
        break;
    case SUPD_DOWNLOADING:
        supd_pause();
        break;
    case SUPD_PAUSED:
        supd_resume();
        break;
    case SUPD_READY: {
        char msg[96];
#if defined(__ANDROID__)
        snprintf(msg, sizeof(msg), "校验通过，安装 %s？", s.latest);
#else
        snprintf(msg, sizeof(msg), "校验通过，安装 %s 并重启？", s.latest);
#endif
        confirm_open(msg, TR("安装"), do_install, NULL);
        break;
    }
    default: break;   /* CHECKING/VERIFYING：按钮禁用态 */
    }
}

static void on_cancel(lv_event_t *e)
{
    (void)e;
    supd_cancel();
}

static void do_install(void *ud)
{
    (void)ud;
    if (supd_apply()) {
#if defined(__ANDROID__)
        ui_toast("已调起系统安装器…", THEME_COL_ACCENT);
        /* 不重启：Android 系统安装器接管，装完新版自动替换 */
#else
        ui_toast("更新完成，正在重启", THEME_COL_ACCENT);
        bsp_restart();   /* exit → systemd Restart=always 拉起新版本 */
#endif
    } else {
        ui_toast("更新失败", 0xC0392B);
    }
}

static void refresh(void)
{
    supd_progress_tick();
    supd_status_t s;
    supd_poll(&s);

    /* 进度条：仅下载相关状态可见，值变化才更新 */
    bool bar_on = (s.state == SUPD_DOWNLOADING || s.state == SUPD_PAUSED ||
                   s.state == SUPD_VERIFYING || s.state == SUPD_READY);
    if (bar_on && lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN))
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
    if (!bar_on && !lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN))
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    if (bar_on && s.progress != shown_progress) {
        shown_progress = s.progress;
        lv_bar_set_value(bar, s.progress, LV_ANIM_OFF);
    }

    if (s.state == SUPD_DOWNLOADING || s.state == SUPD_PAUSED) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s %d%%",
                 s.state == SUPD_DOWNLOADING ? "下载中" : "已暂停",
                 s.progress / 10);
        lv_label_set_text(lbl_status, buf);
    }
    if (s.state == shown_state) return;
    shown_state = s.state;

    char detail[96];
    detail[0] = 0;
    bool main_en = true, cancel_on = false;
    const char *main_t = TR("检查更新");
    switch (s.state) {
    case SUPD_IDLE:
        lv_label_set_text(lbl_status, TR("检查更新"));
        snprintf(detail, sizeof(detail), "当前版本 v%s", KR_VERSION);
        break;
    case SUPD_CHECKING:
        lv_label_set_text(lbl_status, "正在检查更新...");
        main_en = false;
        break;
    case SUPD_UPTODATE:
        lv_label_set_text(lbl_status, TR("已是最新"));
        snprintf(detail, sizeof(detail), "当前 v%s · 远端 %s", KR_VERSION, s.latest);
        break;
    case SUPD_DOWNLOADING:
        main_t = TR("暂停");
        cancel_on = true;
        snprintf(detail, sizeof(detail), "新版本 %s", s.latest);
        break;
    case SUPD_PAUSED:
        main_t = TR("继续");
        cancel_on = true;
        snprintf(detail, sizeof(detail), "新版本 %s", s.latest);
        break;
    case SUPD_VERIFYING:
        lv_label_set_text(lbl_status, "校验中...");
        main_en = false;
        cancel_on = true;
        break;
    case SUPD_READY:
        lv_label_set_text(lbl_status, "下载完成，校验通过");
        snprintf(detail, sizeof(detail), "当前 v%s → 新版本 %s", KR_VERSION, s.latest);
        main_t = TR("安装");
        cancel_on = true;
        break;
    case SUPD_ERROR:
        lv_label_set_text(lbl_status, s.error);
        snprintf(detail, sizeof(detail), "当前版本 v%s", KR_VERSION);
        main_t = TR("重试");
        break;
    }
    lv_label_set_text(lbl_detail, detail);
    lv_label_set_text(lbl_main, main_t);
    if (main_en) lv_obj_remove_state(btn_main, LV_STATE_DISABLED);
    else lv_obj_add_state(btn_main, LV_STATE_DISABLED);

    /* 取消按钮按需出现；主按钮独占或半宽 */
    int w = cancel_on ? (ui_content_w() - ui_gap(6)) / 2 : ui_content_w();
    lv_obj_set_width(btn_main, w);
    if (cancel_on && lv_obj_has_flag(btn_cancel, LV_OBJ_FLAG_HIDDEN))
        lv_obj_remove_flag(btn_cancel, LV_OBJ_FLAG_HIDDEN);
    if (!cancel_on && !lv_obj_has_flag(btn_cancel, LV_OBJ_FLAG_HIDDEN))
        lv_obj_add_flag(btn_cancel, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *create(void)
{
    shown_state = SUPD_IDLE;
    shown_progress = -1;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    lv_obj_t *card = theme_card(scr);
    lv_obj_set_size(card, ui_content_w(), ui_px(76));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(6));
    lbl_status = theme_label(card, TR("检查更新"), THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, ui_px(4));
    lbl_detail = theme_label(card, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_set_width(lbl_detail, ui_content_w() - 2 * THEME_PAD - ui_px(8));
    lv_obj_align(lbl_detail, LV_ALIGN_TOP_MID, 0, ui_px(28));
    bar = lv_bar_create(card);
    lv_obj_set_size(bar, ui_content_w() - 2 * THEME_PAD - ui_px(16), ui_px(10));
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -ui_px(6));
    lv_bar_set_range(bar, 0, 1000);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);   /* 发现新版本开始下载才出现 */

    /* 来源信息卡：检查网址 + 本架构下载包名（静态信息，长文本裁剪不滚动） */
    lv_obj_t *info = theme_card(scr);
    lv_obj_set_size(info, ui_content_w(), ui_px(52));
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(88));
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);
    int iw = ui_content_w() - 2 * THEME_PAD - ui_px(8);
    lv_obj_t *l1 = theme_label(info, supd_release_page(), THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_set_width(l1, iw);
    lv_label_set_long_mode(l1, LV_LABEL_LONG_CLIP);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, ui_px(2));
    lv_obj_t *l2 = theme_label(info, supd_asset_name(), THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_set_width(l2, iw);
    lv_label_set_long_mode(l2, LV_LABEL_LONG_CLIP);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, ui_px(24));

    /* 底部动作区：主按钮 + 按需出现的取消按钮 */
    int bw = (ui_content_w() - ui_gap(6)) / 2;
    btn_main = theme_button(scr, NULL, TR("检查更新"), 1);
    lv_obj_set_size(btn_main, ui_content_w(), ui_px(38));
    lv_obj_align(btn_main, LV_ALIGN_BOTTOM_LEFT, ui_px(8), -ui_px(6));
    lbl_main = lv_obj_get_child(btn_main, -1);
    lv_obj_add_event_cb(btn_main, on_main, LV_EVENT_CLICKED, NULL);

    btn_cancel = theme_button(scr, NULL, TR("取消"), 0);
    lv_obj_set_size(btn_cancel, bw, ui_px(38));
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_RIGHT, -ui_px(8), -ui_px(6));
    lbl_cancel = lv_obj_get_child(btn_cancel, -1);
    lv_obj_add_event_cb(btn_cancel, on_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(btn_cancel, LV_OBJ_FLAG_HIDDEN);

    /* 主按钮半宽时让位给左侧：底部两键并排 */
    lv_obj_align(btn_main, LV_ALIGN_BOTTOM_LEFT, ui_px(8), -ui_px(6));

    ui_nav_group_set_list(lv_group_get_default(), true);
    return scr;
}

panel_def_t panel_update_def = {
    .name = "update", .title = "检查更新", .title_s = "更新",
    .create = create,
    .on_show = refresh,
    .on_tick = refresh,
    .hide_temps = 1,
};

#endif /* BSP_HAS_LINUX_HOST || __ANDROID__ */
