/* Fixed eight-row page: neither cached records nor LVGL objects grow with the
 * printer's file count. ESP32 loads via a bounded HTTP stream off the UI task. */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "printer.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

void panel_file_detail_set(const char *name, uint32_t size, double modified);
static lv_obj_t *list, *status_label, *prev_btn, *next_btn, *refresh_btn;
static lv_obj_t *rows[PRINTER_FILES_PAGE_SIZE], *names[PRINTER_FILES_PAGE_SIZE], *sizes[PRINTER_FILES_PAGE_SIZE];
static char size_text[PRINTER_FILES_PAGE_SIZE][20], status_text[64];
static printer_file_page_t current;
static unsigned offset;
static bool loading;

static void enabled(lv_obj_t *obj, bool en)
{
    if (en) lv_obj_remove_state(obj, LV_STATE_DISABLED);
    else lv_obj_add_state(obj, LV_STATE_DISABLED);
}
static void request_page(unsigned next_offset);

static void show_page(const printer_file_page_t *page, void *ud)
{
    (void)ud;
    if (!list) return;
    loading = false;
    enabled(refresh_btn, true);
    enabled(prev_btn, offset > 0);
    enabled(next_btn, page && offset + page->count < page->total);
    if (!page) {
        lv_label_set_text_static(status_label, TR("获取失败，请检查连接"));
        return;
    }
    current = *page;
    if (!current.total) { offset = 0; enabled(prev_btn, false); }
    if (current.total && !current.count && offset >= current.total) {
        request_page(((current.total - 1) / PRINTER_FILES_PAGE_SIZE) * PRINTER_FILES_PAGE_SIZE);
        return;
    }
    for (int i = 0; i < PRINTER_FILES_PAGE_SIZE; i++) {
        if (i >= current.count) { lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN); continue; }
        printer_file_t *f = &current.files[i];
        lv_label_set_text_static(names[i], f->name);
        if (f->size >= 1024 * 1024) {
            theme_fmt_float(size_text[i], sizeof(size_text[i]), f->size / 1048576.0f, 1);
            size_t n = strlen(size_text[i]);
            snprintf(size_text[i] + n, sizeof(size_text[i]) - n, "MB");
        } else snprintf(size_text[i], sizeof(size_text[i]), "%uKB", (unsigned)(f->size / 1024 + 1));
        lv_label_set_text_static(sizes[i], size_text[i]);
        enabled(rows[i], true);
        lv_obj_remove_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!current.total) lv_label_set_text_static(status_label, TR("暂无 GCode 文件"));
    else {
        snprintf(status_text, sizeof(status_text), "%u / %u%s", offset / PRINTER_FILES_PAGE_SIZE + 1,
            (current.total + PRINTER_FILES_PAGE_SIZE - 1) / PRINTER_FILES_PAGE_SIZE,
            current.skipped ? " *" : "");
        lv_label_set_text_static(status_label, status_text);
    }
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
}

static void request_page(unsigned next_offset)
{
    if (loading || !list) return;
    offset = next_offset;
    loading = true;
    enabled(prev_btn, false); enabled(next_btn, false); enabled(refresh_btn, false);
    for (int i = 0; i < PRINTER_FILES_PAGE_SIZE; i++) {
        enabled(rows[i], false);
        lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text_static(status_label, TR("加载中…"));
    if (!printer_files_refresh(offset, show_page, NULL)) show_page(NULL, NULL);
}

static void previous(lv_event_t *e) { (void)e; request_page(offset >= PRINTER_FILES_PAGE_SIZE ? offset - PRINTER_FILES_PAGE_SIZE : 0); }
static void next(lv_event_t *e) { (void)e; request_page(offset + PRINTER_FILES_PAGE_SIZE); }
static void refresh(lv_event_t *e) { (void)e; request_page(offset); }

static void on_row(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (loading || i < 0 || i >= current.count) return;
    printer_file_t *f = &current.files[i];
    panel_file_detail_set(f->name, f->size, f->modified);
    panel_mgr_open("file_detail");
}

static void on_leave(lv_event_t *e)
{
    if (!list || lv_obj_get_screen(list) != lv_event_get_target_obj(e)) return;
    printer_files_cancel();
    loading = false;
    if (lv_event_get_code(e) == LV_EVENT_DELETE) list = NULL;
}

static void on_show(void) { loading = false; request_page(offset); }

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    int top = THEME_TITLEBAR_H + ui_px(4);
    int footer = ui_px(32);
    status_label = theme_label(scr, "", THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_set_width(status_label, ui_content_w());
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, top);
    list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, ui_content_w(), ui_scr_h() - top - ui_px(24) - footer);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, top + ui_px(20));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, THEME_GAP, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    for (int i = 0; i < PRINTER_FILES_PAGE_SIZE; i++) {
        rows[i] = theme_action_card(list);
        lv_obj_set_size(rows[i], ui_content_w(), ui_px(40));
        lv_obj_add_event_cb(rows[i], on_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        names[i] = theme_label(rows[i], "", THEME_FONT_S, THEME_COL_TEXT);
        lv_obj_set_width(names[i], ui_content_w() - 2 * THEME_PAD - ui_px(65));
        /* DOT mutates its text buffer; never let it modify the stored path.
         * CLIP uses no marquee timers or writable copies; details show full names. */
        lv_label_set_long_mode(names[i], LV_LABEL_LONG_CLIP);
        lv_obj_align(names[i], LV_ALIGN_LEFT_MID, 0, 0);
        sizes[i] = theme_label(rows[i], "", THEME_FONT_S, THEME_COL_TEXT_DIM);
        lv_obj_align(sizes[i], LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    prev_btn = theme_button(scr, LV_SYMBOL_LEFT, NULL, 0);
    next_btn = theme_button(scr, LV_SYMBOL_RIGHT, NULL, 0);
    refresh_btn = theme_button(scr, LV_SYMBOL_REFRESH, NULL, 0);
    lv_obj_set_size(prev_btn, ui_px(64), footer - ui_px(4));
    lv_obj_set_size(next_btn, ui_px(64), footer - ui_px(4));
    lv_obj_set_size(refresh_btn, ui_px(64), footer - ui_px(4));
    lv_obj_align(prev_btn, LV_ALIGN_BOTTOM_LEFT, ui_px(8), -ui_px(2));
    lv_obj_align(next_btn, LV_ALIGN_BOTTOM_RIGHT, -ui_px(8), -ui_px(2));
    lv_obj_align(refresh_btn, LV_ALIGN_BOTTOM_MID, 0, -ui_px(2));
    lv_obj_add_event_cb(prev_btn, previous, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(next_btn, next, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(refresh_btn, refresh, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr, on_leave, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    lv_obj_add_event_cb(scr, on_leave, LV_EVENT_DELETE, NULL);
    ui_nav_group_set_list(lv_group_get_default(), true);
    return scr;
}

panel_def_t panel_files_def = {
    .name = "files", .title = "打印文件", .title_s = "文件",
    .create = create, .on_show = on_show, .on_tick = printer_files_poll, .hide_temps = 1,
};
