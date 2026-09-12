/*
 * 文件详情（打印文件的二级菜单）：显示文件信息，提供 打印 / 删除。
 * 删除为破坏性操作，需二次确认（第一次点变成"确认删除？"，再点才执行）。
 */
#include "../theme.h"
#include "../lang.h"
#include "../ui_anim.h"
#include "../panel_mgr.h"
#include "printer.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static char     sel_name[96];
static uint32_t sel_size;
static double   sel_modified;

static lv_obj_t *lbl_name;
static lv_obj_t *lbl_info;
static lv_obj_t *lbl_del_text;
static bool confirm_del;

/* panel_files 选中行时调用，随后 panel_mgr_open("file_detail") */
void panel_file_detail_set(const char *name, uint32_t size, double modified)
{
    strncpy(sel_name, name, sizeof(sel_name) - 1);
    sel_name[sizeof(sel_name) - 1] = 0;
    sel_size = size;
    sel_modified = modified;
}

static void on_print(lv_event_t *e)
{
    (void)e;
    if (printer_state() == PRINTER_STATE_PRINTING) {
        ui_toast("正在打印中，无法开始新任务", THEME_COL_ERROR);
        return;
    }
    printer_print_start(sel_name);
    ui_toast("开始打印", THEME_COL_OK);
    panel_mgr_open("job_status");
}

static void on_delete(lv_event_t *e)
{
    (void)e;
    if (!confirm_del) {
        confirm_del = true;
        lv_label_set_text(lbl_del_text, TR("确认删除?"));
        return;
    }
    printer_file_delete(sel_name);
    ui_toast("已删除", THEME_COL_OK);
    panel_mgr_back();   /* 回到列表，其 on_show 会重新拉取 */
}

static void update_ui(void)
{
    confirm_del = false;
    lv_label_set_text(lbl_name, sel_name);

    char sz[20];
    if (sel_size >= 1024 * 1024) {
        theme_fmt_float(sz, sizeof(sz), sel_size / 1048576.0f, 1);
        strncat(sz, "MB", sizeof(sz) - strlen(sz) - 1);
    } else {
        snprintf(sz, sizeof(sz), "%uKB", (unsigned)(sel_size / 1024 + 1));
    }
    char info[64];
    time_t t = (time_t)sel_modified;
    struct tm *tm = localtime(&t);
    if (tm)
        snprintf(info, sizeof(info), "%s  %02d-%02d %02d:%02d",
                 sz, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
    else
        snprintf(info, sizeof(info), "%s", sz);
    lv_label_set_text(lbl_info, info);

    lv_label_set_text(lbl_del_text, TR("删除"));
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    /* 文件信息卡：撑满标题栏与底部按钮之间的空间 */
    int gap = ui_gap(8);
    int card_h = ui_scr_h() - (THEME_TITLEBAR_H + ui_px(4)) - (ui_px(44) + ui_px(12) + gap);
    lv_obj_t *card = theme_card(scr);
    lv_obj_set_size(card, ui_content_w(), card_h);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(4));

    lbl_name = theme_label(card, "", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_set_width(lbl_name, ui_content_w() - 2 * THEME_PAD);
    lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_name, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_info = theme_label(card, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(lbl_info, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* 打印（主操作，accent） */
    int bw = (ui_content_w() - gap) / 2;
    lv_obj_t *btn_print = theme_button(scr, LV_SYMBOL_PLAY, "打印", 1);
    lv_obj_set_size(btn_print, bw, ui_px(44));
    lv_obj_align(btn_print, LV_ALIGN_BOTTOM_LEFT, ui_px(8), -ui_px(12));
    lv_obj_add_event_cb(btn_print, on_print, LV_EVENT_CLICKED, NULL);

    /* 删除（危险操作，红字；确认态文字由 update_ui/on_delete 维护） */
    lv_obj_t *btn_del = theme_button(scr, NULL, NULL, 0);
    lv_obj_set_size(btn_del, bw, ui_px(44));
    lv_obj_align(btn_del, LV_ALIGN_BOTTOM_RIGHT, -ui_px(8), -ui_px(12));
    lv_obj_add_event_cb(btn_del, on_delete, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(btn_del, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_del, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_del, ui_px(4), 0);
    lv_obj_t *ic = theme_label(btn_del, LV_SYMBOL_TRASH, THEME_FONT_ICON, THEME_COL_ERROR);
    lbl_del_text = theme_label(btn_del, "删除", THEME_FONT_S, THEME_COL_ERROR);

    return scr;
}

panel_def_t panel_file_detail_def = {
    .name = "file_detail", .title = "文件详情", .title_s = "详情",
    .create = create,
    .on_show = update_ui,
    .on_tick = NULL,
    .hide_temps = 1,
};
