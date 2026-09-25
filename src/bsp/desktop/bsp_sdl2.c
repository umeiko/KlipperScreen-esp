/*
 * BSP: desktop（SDL2，Windows/Linux 同构）
 * 默认逻辑分辨率与 CYD 2432S028R 一致（320x240 横屏），窗口 2x 缩放，鼠标模拟触摸。
 * 环境变量 KLIPPER_RES=WxH 可模拟其它板型分辨率（如 KLIPPER_RES=800x480 模拟 JC8048W550）。
 */
#include "bsp.h"
#include "bsp_caps.h"
#include "bsp_screen_power.h"
#include "ui_buttons.h"
#include "ui_nav.h"
#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int scr_w = 320, scr_h = 240;
static SDL_mutex *lvgl_mutex;

#if defined(KLIPPER_DESKTOP_SIMULATOR) && defined(KR_DISPLAY_SETTINGS_PREVIEW)
static bool preview_bgr;

static bool preview_color_order_apply(bool bgr)
{
    preview_bgr = bgr;
    return true;
}

/* SDL DIRECT/RGB565 同步复制到纹理：flush 前模拟 R/B 互换，flush 后还原。
 * 不改主题常量，不污染 LVGL 双缓冲的逻辑像素；此路径只用于桌面预览。
 * 实体面板必须接自己的色序控制，不能把该逐像素路径带到 ESP32。 */
static void preview_color_flush(lv_event_t *e)
{
    if (!preview_bgr) return;
    lv_display_t *disp = lv_event_get_current_target(e);
    lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
    for (uint32_t y = 0; y < buf->header.h; y++) {
        uint16_t *pixels = (uint16_t *)(buf->data + y * buf->header.stride);
        for (uint32_t x = 0; x < buf->header.w; x++) {
            uint16_t p = pixels[x];
            pixels[x] = (uint16_t)((p & 0x07E0) | ((p & 0xF800) >> 11) | ((p & 0x001F) << 11));
        }
    }
}
#endif

#if BSP_HAS_ENCODER_SETTINGS
static int encoder_counts = 4, encoder_remainder;
static int encoder_hw_counts = 2;  /* 预览默认复现“两格走一下” */
#endif

int bsp_encoder_default_counts_per_detent(void)
{
#if BSP_HAS_ENCODER_SETTINGS
    return 4;
#else
    return 0;
#endif
}

int bsp_encoder_get_counts_per_detent(void)
{
#if BSP_HAS_ENCODER_SETTINGS
    return encoder_counts;
#else
    return 0;
#endif
}

bool bsp_encoder_set_counts_per_detent(int counts)
{
#if BSP_HAS_ENCODER_SETTINGS
    if (counts < 0 || counts > 8) return false;
    encoder_counts = counts ? counts : bsp_encoder_default_counts_per_detent();
    encoder_remainder = 0;
    return true;
#else
    LV_UNUSED(counts);
    return false;
#endif
}

static uint64_t screen_now_ms(void)
{
    return SDL_GetTicks64();
}

static void backlight_apply(int pct)
{
    /* Desktop has no physical backlight; keep the state observable in logs. */
    printf("backlight: %d%%\n", pct);
}

static int SDLCALL screen_input_filter(void *userdata, SDL_Event *event)
{
    (void)userdata;
    bool activity = event->type == SDL_MOUSEWHEEL ||
                    event->type == SDL_KEYDOWN ||
                    event->type == SDL_TEXTINPUT ||
                    event->type == SDL_FINGERDOWN ||
                    (event->type == SDL_MOUSEBUTTONDOWN &&
                     (event->button.button == SDL_BUTTON_LEFT ||
                      event->button.button == SDL_BUTTON_MIDDLE));

    /* Dropping the first event mirrors the hardware adapters: waking the
       screen must not also click a control or move encoder focus. */
    if (activity && bsp_screen_activity()) return 0;
#if BSP_HAS_ENCODER_SETTINGS
    if (event->type == SDL_MOUSEWHEEL) {
        /* 在 SDL 正常 encoder 驱动之前换算；中键/键盘/触摸不受影响。
         * setter 与事件泵均运行在主 LVGL 线程，不另建输入后台线程。 */
        encoder_remainder += event->wheel.y * encoder_hw_counts;
        event->wheel.y = encoder_remainder / encoder_counts;
        encoder_remainder -= event->wheel.y * encoder_counts;
    }
#endif
    return 1;
}

void bsp_init(void)
{
    const char *res = getenv("KLIPPER_RES");
    if (res) {
        int w = 0, h = 0;
        if (sscanf(res, "%dx%d", &w, &h) == 2 && w >= 128 && h >= 96) {
            scr_w = w; scr_h = h;
        }
    }

    /* 独占显示服务（Linux systemd 单元设置 KLIPPER_FULLSCREEN=1）：
       以屏幕原生分辨率建窗并隐藏光标——weston kiosk-shell 会自动全屏化
       xdg-toplevel；裸 X11（xinit 无 WM）下原生分辨率窗口即铺满全屏。
       SDL_Init 幂等，提前调只为读显示模式。 */
    const bool fullscreen = !res && getenv("KLIPPER_FULLSCREEN") &&
                            getenv("KLIPPER_FULLSCREEN")[0] == '1';
    if (fullscreen) {
        SDL_Init(SDL_INIT_VIDEO);
        SDL_DisplayMode mode;
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0 &&
            mode.w >= 128 && mode.h >= 96) {
            scr_w = mode.w;
            scr_h = mode.h;
        }
        SDL_ShowCursor(SDL_DISABLE);
    }

    lv_init();

    lv_display_t *disp = lv_sdl_window_create(scr_w, scr_h);
#if defined(KLIPPER_DESKTOP_SIMULATOR) && defined(KR_DISPLAY_SETTINGS_PREVIEW)
    /* 仅在已验证的 SDL 模式开放能力，避免误用于异步/局部缓冲驱动。 */
    if (lv_display_get_color_format(disp) == LV_COLOR_FORMAT_RGB565 &&
        LV_SDL_RENDER_MODE == LV_DISPLAY_RENDER_MODE_DIRECT && LV_USE_DRAW_SDL == 0) {
        bsp_disp_color_order_register(false, preview_color_order_apply); /* SDL 原生 RGB */
        lv_display_add_event_cb(disp, preview_color_flush, LV_EVENT_FLUSH_START, NULL);
        lv_display_add_event_cb(disp, preview_color_flush, LV_EVENT_FLUSH_FINISH, NULL);
    }
#endif
    /* 小屏放大看：160x128 → 3x，320x240 → 2x，800x480 → 1x；全屏服务不缩放 */
    if (!fullscreen)
        lv_sdl_window_set_zoom(disp, scr_w <= 200 ? 3 : (scr_w <= 320 ? 2 : 1));
#ifdef KLIPPER_DESKTOP_SIMULATOR
#if defined(KR_DISPLAY_SETTINGS_PREVIEW)
    lv_sdl_window_set_title(disp, "Display Settings Preview - RGB/BGR + Encoder");
#elif BSP_HAS_ENCODER_SETTINGS
    lv_sdl_window_set_title(disp, "Encoder Settings Preview - wheel / middle click");
#else
    lv_sdl_window_set_title(disp, "Klipper Remote Simulator");
#endif
#else
    lv_sdl_window_set_title(disp, "Klipper Remote");
#endif
    lv_sdl_mouse_create();
    lvgl_mutex = SDL_CreateMutex();
    if (!lvgl_mutex) {
        fprintf(stderr, "SDL_CreateMutex failed: %s\n", SDL_GetError());
        exit(1);
    }
    bsp_screen_power_init(backlight_apply, screen_now_ms);
    SDL_SetEventFilter(screen_input_filter, NULL);
}

/* 物理键盘 → 语义实体按钮（上/下/左/右/确定/返回）。
   文本输入会话期间键盘归会话所有（ui_nav 的 desktop_keyboard_watch），这里让路。
   事件在 SDL_PollEvent（LVGL 定时器内、已持锁）里派发，可直接喂语义层。 */
static int SDLCALL buttons_sdl_watch(void *userdata, SDL_Event *event)
{
    (void)userdata;
    if (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP) return 0;
    if (ui_desktop_input_active()) {
        /* 文本输入会话期间：方向键归导航走位；回车归导航（按下虚拟键盘高亮键 /
           数字键盘焦点键 = 输入字符），F1 才提交表单；字符/退格/ESC 归会话。 */
        switch (event->key.keysym.sym) {
        case SDLK_UP: case SDLK_DOWN: case SDLK_LEFT: case SDLK_RIGHT:
        case SDLK_RETURN: case SDLK_KP_ENTER:
            break;
        default: return 0;
        }
    }
    if (event->key.repeat) return 0;   /* 长按重复由 LVGL keypad 处理，忽略 SDL 自重复 */
    if (event->type == SDL_KEYDOWN &&
        (event->key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI))) return 0;

    ui_button_id_t id;
    switch (event->key.keysym.sym) {
    case SDLK_UP:        id = UI_BTN_UP;    break;
    case SDLK_DOWN:      id = UI_BTN_DOWN;  break;
    case SDLK_LEFT:      id = UI_BTN_LEFT;  break;
    case SDLK_RIGHT:     id = UI_BTN_RIGHT; break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  id = UI_BTN_OK;    break;
    case SDLK_ESCAPE:
    case SDLK_BACKSPACE: id = UI_BTN_BACK;  break;
    default: return 0;
    }
    ui_buttons_send(id, event->type == SDL_KEYDOWN);
    return 0;
}

void bsp_input_init(void)
{
#if BSP_HAS_ENCODER_SETTINGS
    const char *counts = getenv("KLIPPER_ENCODER_HW_COUNTS");
    if (counts && strlen(counts) == 1 && counts[0] >= '1' && counts[0] <= '8')
        encoder_hw_counts = counts[0] - '0';
#endif
    /* 滚轮正/反转 = encoder diff；中键按下 = encoder push。 */
    lv_sdl_mousewheel_create();
    SDL_AddEventWatch(buttons_sdl_watch, NULL);
}

/* ---------- 开机动画推屏（boot_anim 调用；首次调用时建全屏 canvas） ---------- */
static lv_obj_t  *boot_scr;
static lv_obj_t  *boot_canvas;
static uint16_t  *boot_buf;      /* scr_w * scr_h */

static void boot_cleanup_cb(lv_timer_t *t)
{
    /* 主界面加载后清理开场资源（此时主屏幕已激活，删旧屏幕安全） */
    if (boot_scr) lv_obj_delete(boot_scr);
    free(boot_buf);
    boot_scr = NULL;
    boot_buf = NULL;
    lv_timer_delete(t);
}

void bsp_lcd_push(int x, int y, int w, int h, const uint16_t *px)
{
    if (!boot_buf) {
        boot_buf = malloc((size_t)scr_w * scr_h * 2);
        if (!boot_buf) return;
        memset(boot_buf, 0, (size_t)scr_w * scr_h * 2);
        boot_scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(boot_scr, lv_color_black(), 0);
        lv_screen_load(boot_scr);
        boot_canvas = lv_canvas_create(boot_scr);
        lv_canvas_set_buffer(boot_canvas, boot_buf, scr_w, scr_h, LV_COLOR_FORMAT_RGB565);
        lv_timer_create(boot_cleanup_cb, 100, NULL);   /* 动画播完进主循环后自动清理 */
    }
    for (int r = 0; r < h; r++)
        memcpy(boot_buf + (size_t)(y + r) * scr_w + x, px + (size_t)r * w, (size_t)w * 2);
    lv_obj_invalidate(boot_canvas);
    lv_refr_now(NULL);
}

void bsp_delay_ms(uint32_t ms)
{
    lv_delay_ms(ms);
}

lv_display_t *bsp_get_display(void)
{
    return lv_display_get_default();
}

/* 网络线程只在持锁时向 LVGL 投递异步更新。主循环也持同一把锁。 */
void bsp_lvgl_lock(void)   { if (lvgl_mutex) SDL_LockMutex(lvgl_mutex); }
void bsp_lvgl_unlock(void) { if (lvgl_mutex) SDL_UnlockMutex(lvgl_mutex); }

void bsp_restart(void)
{
    /* 桌面端无「重启」概念：退出进程，重新启动即按新配置加载 */
    printf("bsp_restart: exit for restart\n");
    exit(0);
}

const char *bsp_board_name(void) { return "desktop"; }

/* 桌面端调试前端：反色/旋转/镜像不提供（UI 会按 can_* 隐藏开关） */
bool bsp_disp_can_invert(void)    { return false; }
bool bsp_disp_can_rotate180(void) { return false; }
bool bsp_disp_can_mirror_x(void)  { return false; }
void bsp_disp_set_invert(bool en)    { LV_UNUSED(en); }
void bsp_disp_set_rotate180(bool en) { LV_UNUSED(en); }
void bsp_disp_set_mirror_x(bool en)  { LV_UNUSED(en); }

void bsp_fade_out(uint32_t ms)
{
    /* 桌面端无背光，模拟耗时即可 */
    printf("bsp_fade_out: %ums\n", (unsigned)ms);
    lv_delay_ms(ms);
}

void bsp_time_sync_from_host(const char *host, uint16_t port)
{
    /* 桌面端直接用本机时间，无需兜底 */
    (void)host; (void)port;
}

bool bsp_time_sync_from_http_date(const char *http_date)
{
    /* 桌面端直接用本机时间；视为已处理，调用方无需平台分支。 */
    (void)http_date;
    return true;
}
