#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "bsp_caps.h"

#if BSP_HAS_CAMERA

#include "camera.h"
#include "camera_stream.h"
#include "moonraker_client.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t *screen;
static lv_obj_t *image;
static lv_obj_t *message;
static lv_obj_t *live;
static lv_obj_t *selector;
static lv_timer_t *timer;
static camera_stream_t *stream;
static camera_frame_t *current_frame;
static camera_list_t cameras;
static uint32_t generation;
static uint32_t retry_at;
static bool list_pending;
static bool list_loaded;
static int selected_camera;

static void set_message(const char *text)
{
    if (!message) return;
    const char *translated = TR(text);
    if (strcmp(lv_label_get_text(message), translated) != 0)
        lv_label_set_text(message, translated);
    lv_obj_remove_flag(message, LV_OBJ_FLAG_HIDDEN);
}

static void clear_frame(void)
{
    if (image) {
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(image, NULL);
    }
    if (current_frame) {
        lv_image_cache_drop(&current_frame->dsc);
        camera_frame_free(current_frame);
        current_frame = NULL;
    }
    if (live) lv_obj_add_flag(live, LV_OBJ_FLAG_HIDDEN);
}

static void stop_stream(void)
{
    if (stream) {
        camera_stream_stop(stream);
        stream = NULL;
    }
    clear_frame();
}

static void start_selected(void)
{
    stop_stream();
    if (selected_camera < 0 || selected_camera >= cameras.count) return;
    set_message("正在连接视频流…");
    stream = camera_stream_start(&cameras.items[selected_camera],
                                 lv_obj_get_width(lv_obj_get_parent(image)),
                                 lv_obj_get_height(lv_obj_get_parent(image)));
    if (!stream) set_message("无法启动视频流");
}

static void populate_selector(void)
{
    char options[CAMERA_MAX_CAMERAS * CAMERA_NAME_CAP] = {0};
    for (int i = 0; i < cameras.count; i++) {
        if (i) strncat(options, "\n", sizeof(options) - strlen(options) - 1);
        strncat(options, cameras.items[i].name, sizeof(options) - strlen(options) - 1);
    }
    lv_dropdown_set_options(selector, cameras.count ? options : TR("未发现摄像头"));
    lv_dropdown_set_selected(selector, 0);
    if (cameras.count) lv_obj_remove_state(selector, LV_STATE_DISABLED);
    else lv_obj_add_state(selector, LV_STATE_DISABLED);
}

static void on_camera_list(char *json, void *ud)
{
    uint32_t token = (uint32_t)(uintptr_t)ud;
    if (token != generation || !screen) {
        free(json);
        return;
    }
    list_pending = false;
    camera_list_t parsed;
    if (json && camera_parse_list_json(json, &parsed)) {
        cameras = parsed;
        list_loaded = true;
        selected_camera = 0;
        populate_selector();
        if (cameras.count) start_selected();
        else set_message("未发现已启用的摄像头");
    } else {
        set_message("摄像头列表获取失败，正在重试…");
        retry_at = lv_tick_get() + 3000;
    }
    free(json);
}

static void request_list(void)
{
    if (list_pending || !screen) return;
    stop_stream();
    list_pending = true;
    set_message("正在获取摄像头…");
    if (!moonraker_rpc("server.webcams.list", NULL, on_camera_list,
                       (void *)(uintptr_t)generation)) {
        list_pending = false;
        set_message("Moonraker 未连接，正在重试…");
        retry_at = lv_tick_get() + 3000;
    }
}

static void on_refresh(lv_event_t *e)
{
    LV_UNUSED(e);
    list_loaded = false;
    request_list();
}

static void on_select(lv_event_t *e)
{
    if (!list_loaded) return;
    selected_camera = (int)lv_dropdown_get_selected(lv_event_get_target(e));
    start_selected();
}

static void show_frame(camera_frame_t *next)
{
    camera_frame_t *old = current_frame;
    current_frame = next;
    lv_image_set_src(image, &next->dsc);
    lv_obj_center(image);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(message, LV_OBJ_FLAG_HIDDEN);
    if (old) {
        lv_image_cache_drop(&old->dsc);
        camera_frame_free(old);
    }
}

static void poll_stream(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!screen) return;
    if (!list_loaded && !list_pending && (int32_t)(lv_tick_get() - retry_at) >= 0)
        request_list();
    if (!stream) return;

    camera_frame_t *next = NULL;
    camera_stream_status_t status;
    camera_stream_poll(stream, &next, &status);
    if (next) show_frame(next);
    if (status.state == CAMERA_STREAM_PLAYING && current_frame) {
        char fps[24];
        if (status.fps_x10)
            snprintf(fps, sizeof(fps), "LIVE  %u.%u FPS", status.fps_x10 / 10, status.fps_x10 % 10);
        else
            snprintf(fps, sizeof(fps), "LIVE");
        if (strcmp(lv_label_get_text(live), fps) != 0) lv_label_set_text(live, fps);
        lv_obj_remove_flag(live, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(live, LV_OBJ_FLAG_HIDDEN);
        if (status.state == CAMERA_STREAM_RECONNECTING)
            set_message("视频流中断，正在重连…");
        else if (status.state == CAMERA_STREAM_FAILED)
            set_message("无法播放该视频流（需 HTTP MJPEG/JPEG）");
        else
            set_message("正在连接视频流…");
    }
}

static void on_screen_delete(lv_event_t *e)
{
    LV_UNUSED(e);
    generation++;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    image = NULL;
    message = NULL;
    live = NULL;
    selector = NULL;
    stop_stream();
    screen = NULL;
    list_pending = false;
    list_loaded = false;
}

static lv_obj_t *create(void)
{
    generation++;
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, theme_col(THEME_COL_BG), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    stream = NULL;
    current_frame = NULL;
    list_pending = false;
    list_loaded = false;
    selected_camera = 0;

    int view_y = THEME_TITLEBAR_H + ui_px(4);
    int controls_h = ui_px(34);
    int view_h = ui_scr_h() - view_y - controls_h - ui_px(4);
    lv_obj_t *view = lv_obj_create(screen);
    lv_obj_set_size(view, ui_content_w(), view_h);
    lv_obj_align(view, LV_ALIGN_TOP_MID, 0, view_y);
    lv_obj_set_style_bg_color(view, lv_color_black(), 0);
    lv_obj_set_style_border_width(view, 0, 0);
    lv_obj_set_style_radius(view, THEME_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(view, 0, 0);
    lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLLABLE);

    image = lv_image_create(view);
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);

    message = theme_label(view, "", THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_set_width(message, ui_content_w() - ui_px(16));
    lv_obj_set_style_text_color(message, lv_color_white(), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_center(message);

    live = theme_label(view, "", THEME_FONT_S, THEME_COL_TEXT);
    lv_obj_set_style_text_color(live, lv_color_white(), 0);
    lv_obj_set_style_bg_color(live, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(live, LV_OPA_60, 0);
    lv_obj_set_style_radius(live, ui_px(3), 0);
    lv_obj_set_style_pad_all(live, ui_px(3), 0);
    lv_obj_align(live, LV_ALIGN_BOTTOM_LEFT, ui_px(4), -ui_px(4));
    lv_obj_add_flag(live, LV_OBJ_FLAG_HIDDEN);

    int selector_w = ui_content_w() - ui_px(80);
    selector = lv_dropdown_create(screen);
    lv_obj_set_size(selector, selector_w, ui_px(30));
    lv_obj_align(selector, LV_ALIGN_BOTTOM_LEFT, ui_px(8), -ui_px(4));
    lv_obj_set_style_text_font(selector, THEME_FONT_M, 0);
    lv_dropdown_set_options(selector, TR("正在加载…"));
    lv_obj_add_state(selector, LV_STATE_DISABLED);
    theme_focusable(selector);
    theme_dropdown_finish(selector);
    lv_obj_add_event_cb(selector, on_select, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *refresh = theme_button(screen, LV_SYMBOL_REFRESH, "刷新", 0);
    lv_obj_set_size(refresh, ui_px(72), ui_px(30));
    lv_obj_align(refresh, LV_ALIGN_BOTTOM_RIGHT, -ui_px(8), -ui_px(4));
    lv_obj_add_event_cb(refresh, on_refresh, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(screen, on_screen_delete, LV_EVENT_DELETE, NULL);
    timer = lv_timer_create(poll_stream, 66, NULL);
    ui_nav_group_set_spatial(lv_group_get_default(), true);
    request_list();
    return screen;
}

panel_def_t panel_camera_def = {
    .name = "camera", .title = "视频监视", .title_s = "视频",
    .create = create,
    .hide_temps = 1,
};

#endif
