/*
 * OTA 升级进度面板（ST7789 等屏幕顶层覆盖层）：
 * 开机自动升级时显示「OTA 升级中」+ 版本变化 + 下载进度条。
 *
 * 与 ota_auto 通过全局进度结构（ota_progress_get）松耦合：
 * ota_auto 在下载/写入时更新状态，本模块在 UI 任务（持 LVGL 锁）内
 * 周期轮询并渲染，避免跨线程直接操作 LVGL。
 */

#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "esp_timer.h"
#include "theme.h"
#include "ui_layout.h"
#include "ota_auto.h"
#include "ota_progress_ui.h"

#define TAG_UPDATE_MS 100   /* 进度文本刷新节流 */

static lv_obj_t *s_overlay = NULL;   /* 全屏遮罩 + 卡片根对象 */
static lv_obj_t *s_bar = NULL;       /* 进度条 */
static lv_obj_t *s_lbl_status = NULL;/* 状态/百分比 */
static int64_t   s_last_update = 0;

static void ota_panel_create(void)
{
    lv_obj_t *top = lv_layer_top();
    s_overlay = lv_obj_create(top);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, ui_scr_w(), ui_scr_h());
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_overlay, LV_OPA_70, 0);   /* 半透明遮罩，露出底层 UI 轮廓 */

    /* 居中卡片 */
    int cw = ui_px(272), ch = ui_px(150);
    lv_obj_t *card = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, cw, ch);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x2A2A30), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, ui_px(10), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x55555E), 0);
    lv_obj_set_style_pad_all(card, ui_px(14), 0);

    /* 标题 */
    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_style_text_font(title, THEME_FONT_M, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title, "OTA 升级中");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    /* 版本变化 */
    lv_obj_t *ver = lv_label_create(card);
    lv_obj_set_style_text_font(ver, THEME_FONT_S, 0);
    lv_obj_set_style_text_color(ver, lv_color_hex(0xC8C8D0), 0);
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, ui_px(34));

    /* 进度条 */
    lv_obj_t *bar = lv_bar_create(card);
    lv_obj_set_size(bar, cw - ui_px(44), ui_px(12));
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, ui_px(64));
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x4A4A52), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, ui_px(6), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x2F9BFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, ui_px(6), LV_PART_INDICATOR);
    s_bar = bar;

    /* 状态文字（百分比 / 阶段提示） */
    lv_obj_t *status = lv_label_create(card);
    lv_obj_set_style_text_font(status, THEME_FONT_S, 0);
    lv_obj_set_style_text_color(status, lv_color_hex(0xDCDCE4), 0);
    lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -ui_px(8));
    s_lbl_status = status;

    /* 设置版本文本 */
    const ota_progress_t *p = ota_progress_get();
    lv_label_set_text_fmt(ver, "%s  →  %s", p->cur_ver[0] ? p->cur_ver : "?",
                          p->new_ver[0] ? p->new_ver : "?");
    lv_label_set_text(status, "准备下载…");
}

static void ota_panel_update(void)
{
    const ota_progress_t *p = ota_progress_get();
    int percent = 0;
    if (p->total > 0) {
        percent = (int)((int64_t)p->downloaded * 100 / p->total);
        if (percent > 100) percent = 100;
    }
    lv_bar_set_value(s_bar, percent, LV_ANIM_OFF);

    char st[48];
    switch (p->phase) {
    case 0:
        lv_label_set_text(s_lbl_status, "检测到新版本，准备下载…");
        break;
    case 1:
        if (p->total > 0)
            snprintf(st, sizeof(st), "正在下载 %d%%", percent);
        else
            snprintf(st, sizeof(st), "正在下载… %d KB", p->downloaded / 1024);
        lv_label_set_text(s_lbl_status, st);
        break;
    case 2:
        lv_label_set_text(s_lbl_status, "固件写入完成");
        lv_bar_set_value(s_bar, 100, LV_ANIM_OFF);
        break;
    case 3:
        lv_label_set_text(s_lbl_status, "升级完成，即将重启…");
        lv_bar_set_value(s_bar, 100, LV_ANIM_OFF);
        break;
    default:
        break;
    }
}

void ota_progress_ui_poll(void)
{
    const ota_progress_t *p = ota_progress_get();
    if (p->active) {
        if (!s_overlay) {
            ota_panel_create();
            s_last_update = esp_timer_get_time() / 1000;
        } else {
            int64_t now = esp_timer_get_time() / 1000;
            if (now - s_last_update >= TAG_UPDATE_MS) {
                ota_panel_update();
                s_last_update = now;
            }
        }
    } else if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
        s_bar = NULL;
        s_lbl_status = NULL;
    }
}

static void ota_timer_cb(lv_timer_t *t)
{
    (void)t;
    ota_progress_ui_poll();
}

void ota_progress_ui_init(void)
{
    lv_timer_create(ota_timer_cb, 100, NULL);
}
