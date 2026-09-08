/*
 * 文件列表：server.files.list 实时拉取（desktop 走 mock）。
 * 点文件名进二级菜单（file_detail：打印/删除），不直接打印。
 */
#include "../theme.h"
#include "../ui_anim.h"
#include "../panel_mgr.h"
#include "printer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* panel_file_detail.c 提供：选中文件后打开二级菜单 */
void panel_file_detail_set(const char *name, uint32_t size, double modified);

static lv_obj_t *list;
static printer_file_t *files;   /* 缓存的列表（数据层回调移交所有权） */
static int file_count = -1;     /* -1 = 尚未拉取；0 = 空；>0 = 条数 */

static void show_status(const char *text)
{
    lv_obj_clean(list);
    lv_obj_t *l = theme_label(list, text, THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_set_width(l, ui_content_w());
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    panel_mgr_nav_refresh();
}

static void on_row(lv_event_t *e);

static void rebuild_rows(void)
{
    if (!list) return;
    if (file_count <= 0) {
        show_status(file_count == 0 ? "暂无 GCode 文件" : "加载中…");
        return;
    }
    lv_obj_clean(list);
    int row_w = ui_content_w();   /* 行撑满列表宽（list 无内边距），大屏不右侧留白 */
    for (int i = 0; i < file_count; i++) {
        lv_obj_t *row = theme_card(list);
        lv_obj_set_size(row, row_w, ui_px(40));
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *ic = theme_label(row, LV_SYMBOL_FILE, THEME_FONT_ICON, THEME_COL_ACCENT);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *name = theme_label(row, files[i].name, THEME_FONT_S, THEME_COL_TEXT);
        lv_obj_set_width(name, row_w - 2 * THEME_PAD - ui_px(24) - ui_px(70));
        lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, ui_px(24), 0);

        char sz[20];
        if (files[i].size >= 1024 * 1024) {
            theme_fmt_float(sz, sizeof(sz), files[i].size / 1048576.0f, 1);
            strncat(sz, "MB", sizeof(sz) - strlen(sz) - 1);
        } else {
            snprintf(sz, sizeof(sz), "%uKB", (unsigned)(files[i].size / 1024 + 1));
        }
        lv_obj_t *size = theme_label(row, sz, THEME_FONT_S, THEME_COL_TEXT_DIM);
        lv_obj_align(size, LV_ALIGN_RIGHT_MID, -ui_px(4), 0);
    }
    panel_mgr_nav_refresh();   /* 行集合变化：重建焦点组，清掉被 clean 销毁的旧行 */
}

static void on_files(printer_file_t *f, int count, void *ud)
{
    (void)ud;
    free(files);
    if (count < 0) {   /* 获取失败（RPC 错误/解析失败），区别于空列表 */
        files = NULL;
        file_count = 0;
        show_status("获取失败，请检查连接");
        return;
    }
    files = f;
    file_count = count;
    rebuild_rows();
}

static void on_row(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= file_count) return;
    panel_file_detail_set(files[idx].name, files[idx].size, files[idx].modified);
    panel_mgr_open("file_detail");
}

static void on_show(void)
{
    if (!printer_files_refresh(on_files, NULL)) {
        /* 未发出：离线（或上一请求在途，保留现有内容等它回来） */
        if (printer_state() == PRINTER_STATE_DISCONNECTED && file_count <= 0) {
            file_count = 0;
            show_status("未连接 Moonraker");
        }
    } else if (file_count < 0) {
        show_status("加载中…");
    }
    /* 有缓存时先展示旧数据，新数据到达后 on_files 重建 */
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, ui_content_w(), ui_scr_h() - THEME_TITLEBAR_H - ui_px(12));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(4));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, THEME_GAP, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    return scr;
}

panel_def_t panel_files_def = {
    .name = "files", .title = "打印文件",
    .create = create,
    .on_show = on_show,
    .on_tick = NULL,
};
