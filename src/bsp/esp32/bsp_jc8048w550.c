/*
 * BSP: JC8048W550（ESP32-S3-WROOM-1，5" 800x480 ST7262 RGB 并口 + GT911 电容触摸）
 * 引脚与 RGB 时序取自厂商 Arduino 例程（.reff/jc8048w550c/1-Demo）。
 * 与 CYD 2432S028R 的主要差异：
 *   - 屏幕是 RGB 并口（无 GRAM），整帧缓冲在 Octal PSRAM，EDMA 直读
 *   - RGB565 小端原生，不需要 SPI 屏的字节交换
 *   - 电容触摸 GT911（I2C），坐标由驱动按 x_max/y_max + 镜像直接输出，无需两点校准
 */
#include "sdkconfig.h"
#if CONFIG_BOARD_JC8048W550

#include "bsp.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "rgb44.h"

/* ---------- 引脚定义（JC8048W550 官方例程） ---------- */
/* RGB LCD：16bit 565，数据序 B0..B4,G0..G5,R0..R4 */
#define PIN_LCD_DE     40
#define PIN_LCD_VSYNC  41
#define PIN_LCD_HSYNC  39
#define PIN_LCD_PCLK   42
#define PIN_LCD_BL     2     /* 高电平点亮 */
#define LCD_DATA_PINS  { 8, 3, 46, 9, 1,      /* B0..B4 */ \
                         5, 6, 7, 15, 16, 4,  /* G0..G5 */ \
                         45, 48, 47, 21, 14 } /* R0..R4 */

/* 触摸 GT911：I2C0，无 INT（轮询），复位脚 38 */
#define PIN_TP_SDA     19
#define PIN_TP_SCL     20
#define PIN_TP_RST     38

#define LCD_H_RES      800
#define LCD_V_RES      480
#define LCD_PCLK_HZ    (16 * 1000 * 1000)

static const char *TAG = "bsp";

static SemaphoreHandle_t lvgl_mux;
static rgb44_handle_t panel_handle;
static esp_lcd_touch_handle_t touch_handle;
static uint16_t *fb0, *fb1; /* 双帧缓冲（PSRAM 各 768KB）。rgb44 4.4 传输模型 +
                               LVGL DIRECT 直渲 + vsync 换页，排障全程见
                               docs/jc8048w550-rgb-display-guide.md */

/* 开机动画推屏：两块 fb 都写（此时 LVGL 未启动，物理扫描固定在 fb0，
   写双份保证换页后画面不丢）。S3 的 GDMA 读 PSRAM 不过 cache，但动画
   每帧大面积 memcpy 的驱逐压力会被动回写脏行，与 GFX 同款，无需 msync */
static void fb_push(int x, int y, int w, int h, const uint16_t *px)
{
    for (int r = 0; r < h; r++) {
        memcpy(&fb0[(y + r) * LCD_H_RES + x], &px[r * w], w * 2);
        memcpy(&fb1[(y + r) * LCD_H_RES + x], &px[r * w], w * 2);
    }
}

void bsp_lvgl_lock(void)   { xSemaphoreTakeRecursive(lvgl_mux, portMAX_DELAY); }
void bsp_lvgl_unlock(void) { xSemaphoreGiveRecursive(lvgl_mux); }

/* ---------- 开机动画推屏（boot_anim 经 bsp.h 调用，LVGL 锁由调用方持有） ---------- */
void bsp_lcd_push(int x, int y, int w, int h, const uint16_t *px)
{
    fb_push(x, y, w, h, px);
}

void bsp_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* 反色 / 180° 旋转：RGB 并口屏（ST7262）无命令接口，硬件不支持；
   DIRECT 双缓冲下软件翻转/反色的 CPU 拷贝会重新挤爆 MSPI 总线（抽动教训），不做 */
bool bsp_disp_can_invert(void)    { return false; }
bool bsp_disp_can_rotate180(void) { return false; }
void bsp_disp_set_invert(bool en)    { LV_UNUSED(en); }
void bsp_disp_set_rotate180(bool en) { LV_UNUSED(en); }

/* 背光亮度：滑杆 0-100，经 bsp_set_brightness 分段映射到占空比 */
static uint8_t bl_duty = 255;
static int     bl_pct = 100;

/* 本板背光硬件曲线特殊：占空比 80% 以下几乎不可见，可见亮度全挤在 80~100%。
   反向补偿：滑杆 5% → 占空比 80%，滑杆 100% → 100%，分段线性；
   滑杆 0 仍为全灭（息屏逻辑另行处理，不经此函数） */
void bsp_set_brightness(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    bl_pct = pct;
    int duty_pct;
    if (pct <= 5) duty_pct = pct * 16;                 /* 0..5   → 0..80 */
    else          duty_pct = 80 + (pct - 5) * 20 / 95; /* 5..100 → 80..100 */
    bl_duty = (uint8_t)(duty_pct * 255 / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, bl_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ---------- 自动息屏：超时灭背光，触摸唤醒 ---------- */
static uint32_t so_after_s;                     /* 0 = 永不 */
static bool     screen_off;
static int64_t  last_act_us;

void bsp_set_screen_timeout(uint32_t sec)
{
    so_after_s = sec;
    last_act_us = esp_timer_get_time();
    if (screen_off) {                           /* 改设置时若正息屏，先唤醒 */
        screen_off = false;
        bsp_set_brightness(bl_pct);
    }
}

static void screen_activity(void)               /* 触摸回调里打点 + 唤醒 */
{
    last_act_us = esp_timer_get_time();
    if (screen_off) {
        screen_off = false;
        bsp_set_brightness(bl_pct);
    }
}

static void screen_off_check(void)              /* lvgl 任务里周期检查 */
{
    if (screen_off || !so_after_s) return;
    if (esp_timer_get_time() - last_act_us > (int64_t)so_after_s * 1000000) {
        screen_off = true;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
}

/* LEDC 硬件渐变到灭（阻塞至完成）。语言切换重启前调用，避免生硬跳变 */
void bsp_fade_out(uint32_t ms)
{
    static bool fade_installed;
    if (!fade_installed) {
        ledc_fade_func_install(0);
        fade_installed = true;
    }
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0, ms);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);
    bl_duty = 0;

    /* 渐暗后整屏推黑：否则帧缓冲残留旧画面，下次上电瞬间会闪一下 */
    memset(fb0, 0, LCD_H_RES * LCD_V_RES * 2);
    memset(fb1, 0, LCD_H_RES * LCD_V_RES * 2);
}

/* ---------- LVGL 对接 ----------
   DIRECT 双缓冲直渲，flush_cb 三条铁律（详见 docs/jc8048w550-rgb-display-guide.md）：
   1. 只在最后一次 flush（多脏区一段刷新会调多次 flush_cb）请求换页；
   2. 换页前 esp_cache_msync 回写脏行——S3 的 GDMA 读 PSRAM 不过 cache，
      不回写小面积更新会一直显示旧数据；
   3. 阻塞等换页在 vsync 真正生效再 flush_ready，把 LVGL 缓冲轮转锁死在
      物理换页上，LVGL 永远只渲染离屏 fb。 */
/* 换页等待耗时统计（排障用）：flush_cb 里阻塞等 vsync 的时间，需从
   lv_timer_handler 总耗时里扣除才能得到纯渲染耗时 */
static uint32_t swap_n, swap_total_us, swap_max_us;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    if (lv_display_flush_is_last(disp)) {
        esp_cache_msync(px_map, LCD_H_RES * LCD_V_RES * 2,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        if (rgb44_show_fb(panel_handle, px_map)) {
            int64_t t0 = esp_timer_get_time();
            rgb44_wait_swap(panel_handle, 100);
            uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
            swap_n++;
            swap_total_us += dt;
            if (dt > swap_max_us) swap_max_us = dt;
        }
    }
    lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static bool wake_swallow;               /* 息屏唤醒的那次按下：吞掉防误触 */
    esp_lcd_touch_point_data_t pt[1] = {0};
    uint8_t count = 0;
    esp_lcd_touch_read_data(touch_handle);
    if (esp_lcd_touch_get_data(touch_handle, pt, &count, 1) == ESP_OK && count > 0) {
        if (screen_off) wake_swallow = true;    /* 息屏时的按下只为唤醒 */
        screen_activity();          /* 息屏唤醒 + 重置超时计时 */
        if (wake_swallow) {                     /* 唤醒点击不触发任何元素 */
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        data->state = LV_INDEV_STATE_PRESSED;
        /* 驱动按 x_max/y_max 直接输出屏幕坐标（实测无需镜像） */
        data->point.x = LV_CLAMP(0, pt[0].x, LCD_H_RES - 1);
        data->point.y = LV_CLAMP(0, pt[0].y, LCD_V_RES - 1);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        wake_swallow = false;                   /* 抬手，恢复交互 */
    }
}

/* 渲染耗时统计（排障用）：handler=lv_timer_handler 总耗时（含等 vsync），
   work=纯渲染+msync 耗时（扣除 flush_cb 里的换页等待）。
   work 超过帧周期 25.6ms 才会真正掉帧 judder；handler 超期可能只是正常等 vsync。 */
static uint32_t render_calls, render_total_us, render_max_us;
static uint32_t work_total_us, work_max_us;

static void lvgl_task(void *arg)
{
    uint32_t prev_swap_total = 0;
    for (;;) {
        bsp_lvgl_lock();
        int64_t t0 = esp_timer_get_time();
        lv_timer_handler();
        uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
        bsp_lvgl_unlock();
        uint32_t sw = swap_total_us - prev_swap_total;
        prev_swap_total = swap_total_us;
        if (sw > dt) sw = dt;
        uint32_t work = dt - sw;
        render_calls++;
        render_total_us += dt;
        if (dt > render_max_us) render_max_us = dt;
        work_total_us += work;
        if (work > work_max_us) work_max_us = work;
        screen_off_check();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

lv_display_t *bsp_get_display(void) { return lv_display_get_default(); }

void bsp_init(void)
{
    lvgl_mux = xSemaphoreCreateRecursiveMutex();

    /* NVS（WiFi 模块会用，重复 init 安全） */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* LittleFS：挂载 storage 分区到 /littlefs（配置文件），首次启动自动格式化 */
    esp_vfs_littlefs_conf_t fs_conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&fs_conf));

    /* 背光：LEDC PWM（GPIO2，高电平点亮），亮度由 bsp_set_brightness 调节 */
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    ledc_channel_config_t bl_ch = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 255,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));

    /* RGB LCD（ST7262 800x480）：自研 rgb44 驱动（IDF 4.4 传输模型：
       auto_next_frame=false + 一次性 DMA 链 + vsync 全量重启，欠载帧下帧
       必然自愈），双 fb PSRAM + vsync 换页。时序参数同厂商例程 */
    rgb44_config_t rgb_cfg = {
        .timing = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags = { .pclk_active_neg = 1 },
        },
        .data_gpio_nums = LCD_DATA_PINS,
        .hsync_gpio_num = PIN_LCD_HSYNC,
        .vsync_gpio_num = PIN_LCD_VSYNC,
        .pclk_gpio_num = PIN_LCD_PCLK,
        .de_gpio_num = PIN_LCD_DE,
        .disp_gpio_num = GPIO_NUM_NC,
        .fb_in_psram = true,
        .double_fb = true,
    };
    ESP_ERROR_CHECK(rgb44_new(&rgb_cfg, &panel_handle));
    fb0 = rgb44_fb(panel_handle, 0);
    fb1 = rgb44_fb(panel_handle, 1);
    memset(fb0, 0, LCD_H_RES * LCD_V_RES * 2);
    memset(fb1, 0, LCD_H_RES * LCD_V_RES * 2);

    /* 触摸 GT911（I2C0）：原生坐标与显示方向一致，无需镜像 */
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_TP_SDA,
        .scl_io_num = PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &touch_handle));

    /* LVGL */
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    /* DIRECT 双缓冲：LVGL 直渲两块 PSRAM 全帧 fb，flush 只换页不拷贝。
       （试过 FULL 模式：省了 refr_sync_areas 同步拷贝 88→69ms/帧，
       但静止时 1Hz 时钟也触发整帧渲染+回写爆发，每秒可见自抽，回退） */
    lv_display_set_buffers(disp, fb0, fb1, LCD_H_RES * LCD_V_RES * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "BSP ready (JC8048W550, %dx%d, rgb44 DIRECT double-fb)", LCD_H_RES, LCD_V_RES);
}

/* CLI 'lcdstat'：打印并清零 vsync/dma_late/渲染耗时统计（排障用，dma_late=欠载帧数） */
void bsp_lcd_stats_print(void)
{
    uint32_t vs = 0, mn = 0, mx = 0, late = 0;
    rgb44_stats(panel_handle, &vs, &mn, &mx, &late);
    printf("lcd: vsync=%lu period=%lu..%lu us, dma_late=%lu\n"
           "  handler: n=%lu avg=%lu max=%lu us | work: avg=%lu max=%lu us | swap: n=%lu avg=%lu max=%lu us (counters reset)\n",
           (unsigned long)vs, (unsigned long)mn, (unsigned long)mx, (unsigned long)late,
           (unsigned long)render_calls,
           (unsigned long)(render_calls ? render_total_us / render_calls : 0),
           (unsigned long)render_max_us,
           (unsigned long)(render_calls ? work_total_us / render_calls : 0),
           (unsigned long)work_max_us,
           (unsigned long)swap_n,
           (unsigned long)(swap_n ? swap_total_us / swap_n : 0),
           (unsigned long)swap_max_us);
    render_calls = 0; render_total_us = 0; render_max_us = 0;
    work_total_us = 0; work_max_us = 0;
    swap_n = 0; swap_total_us = 0; swap_max_us = 0;
}

void bsp_restart(void)
{
    /* 先把「重启中」toast 画出来再重启 */
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

#endif /* CONFIG_BOARD_JC8048W550 */
