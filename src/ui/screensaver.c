#include "screensaver.h"
#include "lvgl.h"
#include "bsp.h"
#include "bsp_screen_power.h"
#include "assets/saver_frames.h"
#include "app_settings.h"

/* 延时从设置读（1..60秒），缺省5秒 */
#define SAVER_CHG_MV    4210             /* 充电电压 > 4.21V 触发 */
#define SAVER_EXIT_MV    4100             /* 跌到 <= 4.1V 退出 */
#define SAVER_FRAME_MS  600              /* 帧间隔 */

static lv_obj_t *scr_layer;      /* 全屏遮罩 */
static lv_obj_t *img;
static lv_timer_t *frame_timer;
static int cur_frame;
static int active;
static lv_img_dsc_t dsc[3];

static void show(void)
{
    if (active) return;
    active = 1;
    cur_frame = 0;
    lv_img_set_src(img, &dsc[0]);
    lv_obj_remove_flag(scr_layer, LV_OBJ_FLAG_HIDDEN);
    lv_timer_resume(frame_timer);
}

static void hide(void)
{
    if (!active) return;
    active = 0;
    lv_obj_add_flag(scr_layer, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(frame_timer);
}

static void on_frame(lv_timer_t *t)
{
    (void)t;
    cur_frame = (cur_frame + 1) % 3;
    lv_img_set_src(img, &dsc[cur_frame]);
}

void screensaver_init(void)
{
    /* 三张 RGB565 帧包装成 lv_img_dsc_t */
    for (int i = 0; i < 3; i++) {
        dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc[i].header.cf = LV_COLOR_FORMAT_NATIVE;
        dsc[i].header.flags = 0;
        dsc[i].header.w = 320;
        dsc[i].header.h = 240;
        dsc[i].header.stride = 320 * 2;
        dsc[i].data_size = 320 * 240 * 2;
        dsc[i].data = (const uint8_t *)saver_frames[i];
    }

    scr_layer = lv_obj_create(lv_layer_top());
    lv_obj_set_size(scr_layer, 320, 240);
    lv_obj_align(scr_layer, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(scr_layer, lv_color_black(), 0);
    lv_obj_set_style_border_width(scr_layer, 0, 0);
    lv_obj_set_style_pad_all(scr_layer, 0, 0);
    lv_obj_remove_flag(scr_layer, LV_OBJ_FLAG_SCROLLABLE);

    img = lv_img_create(scr_layer);
    lv_obj_center(img);

    frame_timer = lv_timer_create(on_frame, SAVER_FRAME_MS, NULL);
    lv_timer_pause(frame_timer);

    lv_obj_add_flag(scr_layer, LV_OBJ_FLAG_HIDDEN);
}

int screensaver_is_active(void) { return active; }

void screensaver_poll(void)
{
    uint64_t idle = bsp_screen_idle_ms();
    int mv = bsp_battery_mv();

    if (active) {
        /* 任意输入唤醒即退出屏保 */
        if (idle < 200) { hide(); return; }
        /* 电压回落到阈值以下也退出 */
        if (mv <= SAVER_EXIT_MV) hide();
        return;
    }
    /* 屏幕已熄则不播（息屏优先级更高） */
    if (bsp_screen_is_off()) return;
    if (mv > SAVER_CHG_MV && idle >= (uint64_t)settings_load_saver_delay() * 1000U)
        show();
}
