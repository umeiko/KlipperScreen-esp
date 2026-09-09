/*
 * BSP: ESP32-S3 N16R8 + ST7789 240x320 SPI 屏 + GT911 电容触摸（I2C）
 * 逻辑分辨率 320x240 横屏。
 *
 * 引脚定义需按实际接线修改（下方 #define PIN_*）。
 * 触摸 GT911：使用 esp_lcd_touch_gt911 组件，坐标由驱动按 x_max/y_max 直接输出，
 *            方向通过 tp_cfg.flags.swap_xy / mirror_x / mirror_y 调整。
 */
#include "sdkconfig.h"
#if CONFIG_BOARD_ESP32S3_ST7789

#include "bsp.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ---------- 引脚定义（按实际接线修改！） ---------- */
/* LCD SPI */
#define PIN_LCD_SCLK   21
#define PIN_LCD_MOSI   47
#define PIN_LCD_MISO   (-1)   /* ST7789 只写不读，MISO 可不接 */
#define PIN_LCD_CS     41
#define PIN_LCD_DC     40
#define PIN_LCD_RST    45     /* 无独立复位脚时填 -1，走软件复位 */
#define PIN_LCD_BL     42     /* 高电平点亮 */

/* 触摸 GT911 I2C */
#define PIN_TP_SDA     8
#define PIN_TP_SCL     9
#define PIN_TP_RST     (-1)   /* 无复位脚填 -1 */

/* 旋转编码器 EC11（可选）：A/B 相位脚 + 按键，均内部上拉，公共端接 GND */
#define PIN_ENC_A       13
#define PIN_ENC_B       14
#define PIN_ENC_KEY     46

/* 一键息屏/唤醒独立按键（低有效，内部上拉，公共端接 GND） */
/* 息屏/唤醒按键（EC11 minimal 合并）：BOOT(GPIO0) + GPIO39 双键等效，任一按下触发 */
#define PIN_BTN_BOOT     0
#define PIN_PWR_KEY     39

#define LCD_H_RES      320    /* 240x320 面板右转 90° → 横屏 320x240 */
#define LCD_V_RES      240
#define LCD_SPI_HZ     (40 * 1000 * 1000)
#define DRAW_BUF_LINES 40

static const char *TAG = "bsp";

static SemaphoreHandle_t lvgl_mux;
static adc_oneshot_unit_handle_t bat_adc;   /* 电池 ADC（IO17 = ADC2_CH6） */
static adc_cali_handle_t bat_cali;   /* eFuse 校准（可为 NULL） */
static int bat_last_mv = -1;           /* 上次有效电压缓存（WiFi 抢 ADC2 时兜底） */
static esp_lcd_panel_handle_t panel_handle;
static esp_lcd_touch_handle_t touch_handle;
static lv_indev_t *enc_indev;              /* 编码器 LVGL 输入设备 */
static pcnt_unit_handle_t enc_pcnt;        /* PCNT 计数单元 */
static int enc_last_cnt;                   /* 上次读取的计数（差值送给 LVGL） */
static int enc_acc;                       /* 累积计数：攒满 1 格（4 计数）才输出 1 步，避免一格跳多个焦点 */
static int enc_key_raw = 1;                /* 按键原始电平（1=释放，0=按下） */
static int enc_key_state = 1;              /* 去抖后的稳定状态 */
static int64_t enc_key_debounce_ms;        /* 电平变化时间戳（去抖） */

/* IO39 息屏键状态机：0=释放 1=按下去抖中 2=已触发待释放 */
static int pwr_key_state = 0;
static int64_t pwr_key_debounce_ms;

void bsp_lvgl_lock(void)   { xSemaphoreTakeRecursive(lvgl_mux, portMAX_DELAY); }
void bsp_lvgl_unlock(void) { xSemaphoreGiveRecursive(lvgl_mux); }

/* ---------- 开机动画推屏 ---------- */
/* esp_lcd_panel_draw_bitmap 是 DMA 异步传输：必须等 on_color_trans_done 再释放/复用
   像素缓冲，否则 DMA 读到被覆写的内存，画面出现 Y 向条状撕裂 */
static SemaphoreHandle_t lcd_trans_done;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    LV_UNUSED(io); LV_UNUSED(edata); LV_UNUSED(user_ctx);
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(lcd_trans_done, &hp);
    return hp == pdTRUE;
}

void bsp_lcd_push(int x, int y, int w, int h, const uint16_t *px)
{
    /* ST7789 走 SPI 要求先发像素高字节：拷一份交换字节再推 */
    size_t n = (size_t)w * h;
    uint16_t *tmp = malloc(n * 2);
    if (!tmp) return;
    for (size_t i = 0; i < n; i++) tmp[i] = (uint16_t)((px[i] >> 8) | (px[i] << 8));
    xSemaphoreTake(lcd_trans_done, 0);   /* 排掉 LVGL flush 可能留下的存量信号 */
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, tmp);
    xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(500));
    free(tmp);
}

void bsp_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

/* ---------- 背光亮度 ---------- */
static uint8_t bl_duty = 255;
static int     bl_pct = 100;

void bsp_set_brightness(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct > 0 && pct < 5) pct = 5;
    bl_pct = pct;
    bl_duty = (uint8_t)(pct * 255 / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, bl_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ---------- 自动息屏 ---------- */
static uint32_t so_after_s;
static bool     screen_off;
static int64_t  last_act_us;

void bsp_set_screen_timeout(uint32_t sec)
{
    so_after_s = sec;
    last_act_us = esp_timer_get_time();
    if (screen_off) { screen_off = false; bsp_set_brightness(bl_pct); }
}

static void screen_activity(void)
{
    last_act_us = esp_timer_get_time();
    if (screen_off) { screen_off = false; bsp_set_brightness(bl_pct); }
}

static void screen_off_check(void)
{
    if (screen_off || !so_after_s) return;
    if (esp_timer_get_time() - last_act_us > (int64_t)so_after_s * 1000000) {
        screen_off = true;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
}

/* 息屏/唤醒（EC11 minimal 合并）：BOOT(GPIO0) + GPIO39 双键，低电平有效，
   任一稳定按下 50ms 触发一次；释放后再按可再触发 */
static void pwr_key_check(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    int cur = gpio_get_level(PIN_BTN_BOOT) & gpio_get_level(PIN_PWR_KEY);
    /* 两键任一为 0（按下）→ cur=0；全释放 → cur=1 */
    switch (pwr_key_state) {
    case 0:
        if (cur == 0) { pwr_key_state = 1; pwr_key_debounce_ms = now; }
        break;
    case 1:
        if (cur != 0) { pwr_key_state = 0; break; }   /* 抖动弹回 */
        if (now - pwr_key_debounce_ms >= 50) {
            if (screen_off) {
                screen_activity();   /* 唤醒 + 重置活动计时（否则自动息屏超时立即再灭） */
            } else {
                screen_off = true;   /* 息屏：背光灭 */
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            }
            pwr_key_state = 2;   /* 已触发，等释放 */
        }
        break;
    case 2:
        if (cur != 0) pwr_key_state = 0;
        break;
    }
}

void bsp_fade_out(uint32_t ms)
{
    static bool fade_installed;
    if (!fade_installed) { ledc_fade_func_install(0); fade_installed = true; }
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0, ms);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);
    bl_duty = 0;

    /* 渐暗后把 GRAM 整屏推黑：否则面板寄存器残留旧帧，下次上电瞬间会闪一下旧画面 */
    static uint16_t black[LCD_H_RES * 40];
    for (int y = 0; y < LCD_V_RES; y += 40) {
        xSemaphoreTake(lcd_trans_done, 0);
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y + 40, black);
        xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(500));
    }
}

/* ---------- LVGL 对接 ---------- */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* ST7789 走 SPI 要求先发像素高字节，LVGL 内存是小端 RGB565 → 就地交换字节 */
    uint16_t *p = (uint16_t *)px_map;
    int32_t n = (int32_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    for (int32_t i = 0; i < n; i++) p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
    lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    int cnt = 0;
    pcnt_unit_get_count(enc_pcnt, &cnt);
    int diff = (int)((int16_t)(cnt - enc_last_cnt));   /* 旋转步数（正=顺时针） */
    /* 20 脉冲 EC11：1 格 = 1 正交周期 = 4 个 PCNT 计数；30ms 采样会把一格拆成多次读取，
       直接取符号会一格跳多个焦点，这里累积到完整 4 计数才输出 1 步 */
    enc_acc += diff;
    int step = 0;
    while (enc_acc >= 4) { step++; enc_acc -= 4; }
    while (enc_acc <= -4) { step--; enc_acc += 4; }
    data->enc_diff = step;
    enc_last_cnt = cnt;

    /* 按键（低有效）：电平变化后稳定 30ms 才切换状态，去抖 */
    int64_t now = esp_timer_get_time() / 1000;
    int cur = gpio_get_level(PIN_ENC_KEY);   /* 0=按下 */
    if (cur != enc_key_raw) {
        enc_key_raw = cur;
        enc_key_debounce_ms = now;
    }
    if (now - enc_key_debounce_ms >= 30) {
        enc_key_state = cur;
        data->state = cur ? LV_INDEV_STATE_RELEASED : LV_INDEV_STATE_PRESSED;
    } else {
        data->state = enc_key_state ? LV_INDEV_STATE_RELEASED : LV_INDEV_STATE_PRESSED;
    }
    /* 只有实际旋转（step!=0）或按住按键才算活动：否则轮询本身会立即唤醒手动息屏 */
    if (step != 0 || (enc_key_state == 0 && cur == 0))
        screen_activity();
}
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
        if (!touch_handle) { data->state = LV_INDEV_STATE_RELEASED; return; }
    static bool wake_swallow;               /* 息屏唤醒的那次按下：吞掉防误触 */
    esp_lcd_touch_point_data_t pt[1] = {0};
    uint8_t count = 0;
    esp_lcd_touch_read_data(touch_handle);
    if (esp_lcd_touch_get_data(touch_handle, pt, &count, 1) == ESP_OK && count > 0) {
        if (screen_off) wake_swallow = true;
        screen_activity();
        if (wake_swallow) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        data->state = LV_INDEV_STATE_PRESSED;
        /* 驱动按 x_max/y_max 直接输出屏幕坐标；方向由 tp_cfg.flags 控制 */
        data->point.x = LV_CLAMP(0, pt[0].x, LCD_H_RES - 1);
        data->point.y = LV_CLAMP(0, pt[0].y, LCD_V_RES - 1);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        wake_swallow = false;
    }
}

static void lvgl_task(void *arg)
{
    for (;;) {
        bsp_lvgl_lock();
        lv_timer_handler();
        bsp_lvgl_unlock();
        screen_off_check();
        pwr_key_check();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

lv_display_t *bsp_get_display(void) { return lv_display_get_default(); }

void bsp_init(void)
{
    lvgl_mux = xSemaphoreCreateRecursiveMutex();

    /* NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }

    /* LittleFS */
    esp_vfs_littlefs_conf_t fs_conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&fs_conf));

    /* 背光 LEDC */
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
        .duty = 255, .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));

    /* SPI 总线（LCD） */
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* LCD panel IO + ST7789 */
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io_handle));

    lcd_trans_done = xSemaphoreCreateBinary();
    esp_lcd_panel_io_callbacks_t io_cbs = { .on_color_trans_done = on_color_trans_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &io_cbs, NULL));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,   /* 多数 ST7789 模组是 BGR；颜色红蓝互换改 RGB */
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    /* 240x240 面板；若内容上下偏移 40 行改 set_gap(0, 40)，颠倒改 mirror */
ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
    /* 240x320 面板右转 90°（顺时针）：行列交换 + 行反向；反了则换 mirror(false,false) 或 (true,false) */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    /* 左转 90°（逆时针）：行列交换 + 列反向 */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    /* 触摸 GT911（I2C0）：可选。初始化失败仅警告、禁用触摸，不阻塞显示（无触摸屏时正常跑） */
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_TP_SDA,
        .scl_io_num = PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    touch_handle = NULL;
    i2c_master_bus_handle_t i2c_bus = NULL;
    if (i2c_new_master_bus(&i2c_cfg, &i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed, touch disabled");
    } else {
        esp_lcd_panel_io_handle_t tp_io = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        if (esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io) != ESP_OK) {
            ESP_LOGW(TAG, "GT911 I2C IO init failed, touch disabled");
        } else {
            esp_lcd_touch_config_t tp_cfg = {
                .x_max = LCD_H_RES,
                .y_max = LCD_V_RES,
                .rst_gpio_num = PIN_TP_RST,
                .int_gpio_num = GPIO_NUM_NC,
                .levels = { .reset = 0, .interrupt = 0 },
                /* 触摸方向与屏幕不一致时改这里的三个 flag 组合 */
                .flags = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
            };
            if (esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &touch_handle) != ESP_OK) {
                ESP_LOGW(TAG, "GT911 not found, touch disabled");
            } else {
                ESP_LOGI(TAG, "GT911 touch ready");
            }
        }
    }

    /* 旋转编码器 EC11：PCNT 正交解码（A/B），按键 GPIO 输入 */
    gpio_config_t enc_io_cfg = {
        .pin_bit_mask = (1ULL << PIN_ENC_A) | (1ULL << PIN_ENC_B) | (1ULL << PIN_ENC_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&enc_io_cfg));

    pcnt_unit_config_t enc_pcnt_cfg = { .low_limit = -32768, .high_limit = 32767 };
    ESP_ERROR_CHECK(pcnt_new_unit(&enc_pcnt_cfg, &enc_pcnt));
    pcnt_glitch_filter_config_t enc_gf = { .max_glitch_ns = 1000 };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(enc_pcnt, &enc_gf));

    pcnt_chan_config_t enc_chan_cfg = { .edge_gpio_num = PIN_ENC_A, .level_gpio_num = PIN_ENC_B };
    pcnt_channel_handle_t enc_ch_a = NULL, enc_ch_b = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(enc_pcnt, &enc_chan_cfg, &enc_ch_a));
    enc_chan_cfg.edge_gpio_num = PIN_ENC_B;
    enc_chan_cfg.level_gpio_num = PIN_ENC_A;
    ESP_ERROR_CHECK(pcnt_new_channel(enc_pcnt, &enc_chan_cfg, &enc_ch_b));
    /* pcnt_new_channel 会把 GPIO 重配为浮空输入覆盖上拉，这里重新使能 A/B 上拉 */
    ESP_ERROR_CHECK(gpio_config(&enc_io_cfg));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(enc_ch_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(enc_ch_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(enc_ch_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(enc_ch_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));   /* 官方例程：KEEP(低)/INVERSE(高) */
    ESP_ERROR_CHECK(pcnt_unit_enable(enc_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(enc_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_start(enc_pcnt));
    /* 一键息屏/唤醒按键 IO39 */
    gpio_config_t pwr_io_cfg = {
        .pin_bit_mask = (1ULL << PIN_BTN_BOOT) | (1ULL << PIN_PWR_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pwr_io_cfg));
    ESP_LOGI(TAG, "Encoder ready (A=GPIO%d B=GPIO%d KEY=GPIO%d PWRKEY=GPIO%d)",
             PIN_ENC_A, PIN_ENC_B, PIN_ENC_KEY, PIN_PWR_KEY);

    /* LVGL */
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    size_t buf_sz = LCD_H_RES * DRAW_BUF_LINES * 2;
    /* N16R8 有 PSRAM，但 SPI 屏的 DMA 缓冲必须在内部 RAM（GDMA 不能直接访问 PSRAM），
       用 MALLOC_CAP_DMA 分配。如果后续要更大的 PARTIAL 缓冲，可改用 MALLOC_CAP_SPIRAM
       分配渲染缓冲 + 一小块内部 RAM 做 DMA 中转，但 40 行缓冲内部 RAM 已足够 */
    void *buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
    void *buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(buf1 && buf2 ? ESP_OK : ESP_ERR_NO_MEM);
    lv_display_set_buffers(disp, buf1, buf2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    enc_indev = lv_indev_create();
    lv_indev_set_type(enc_indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(enc_indev, encoder_read_cb);


    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);

    /* 电池 ADC：IO17 = ADC2_CH6，100k:100k 分压；11dB 衰减满量程约 3100mV */
    adc_oneshot_unit_init_cfg_t bat_cfg = { .unit_id = ADC_UNIT_2 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&bat_cfg, &bat_adc));
    adc_oneshot_chan_cfg_t bat_chan = { .atten = ADC_ATTEN_DB_11, .bitwidth = ADC_BITWIDTH_12 };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(bat_adc, ADC_CHANNEL_6, &bat_chan));
    /* eFuse 曲线校准（失败则回退 3100mV 线性） */
    adc_cali_scheme_ver_t cali_mask = 0;
    if (adc_cali_check_scheme(&cali_mask) == ESP_OK && (cali_mask & ADC_CALI_SCHEME_VER_CURVE_FITTING)) {
        adc_cali_curve_fitting_config_t ccfg = {
            .unit_id = ADC_UNIT_2, .chan = ADC_CHANNEL_6,
            .atten = ADC_ATTEN_DB_11, .bitwidth = ADC_BITWIDTH_12,
        };
        if (adc_cali_create_scheme_curve_fitting(&ccfg, &bat_cali) != ESP_OK) bat_cali = NULL;
    } else {
        bat_cali = NULL;
    }

    ESP_LOGI(TAG, "BSP ready (ESP32-S3 ST7789+GT911, %dx%d)", LCD_H_RES, LCD_V_RES);
}

void bsp_restart(void)

{
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}
lv_indev_t *bsp_encoder_indev(void) { return enc_indev; }

/* ---------- 显示设置：反色 / 180° 旋转（ST7789 硬件支持，供"显示设置"页调用） ---------- */
bool bsp_disp_can_invert(void) { return true; }
void bsp_disp_set_invert(bool en) { esp_lcd_panel_invert_color(panel_handle, en); }

bool bsp_disp_can_rotate180(void) { return true; }
void bsp_disp_set_rotate180(bool en)
{
    /* 基线方向：swap_xy=true + mirror(true,false)（左转 90° 横屏）；
       再转 180° = 两轴镜像方向互换；恢复则回到基线 */
    if (en) esp_lcd_panel_mirror(panel_handle, false, true);
    else    esp_lcd_panel_mirror(panel_handle, true, false);
}


/* ---------- 电池检测 ---------- */
int bsp_battery_mv(void)
{
    int raw = 0;
    /* ADC2 与 WiFi 共用硬件互斥：WiFi 活跃时读取可能超时 → 用上次缓存（避免跳 0） */
    if (adc_oneshot_read(bat_adc, ADC_CHANNEL_6, &raw) != ESP_OK) return bat_last_mv;
    /* 100k 高阻抗源采样不足：连续读几次取最大，接近真值 */
    for (int i = 0; i < 7; i++) {
        int r = 0;
        if (adc_oneshot_read(bat_adc, ADC_CHANNEL_6, &r) == ESP_OK && r > raw) raw = r;
    }
    int mv;
    if (bat_cali && adc_cali_raw_to_voltage(bat_cali, raw, &mv) == ESP_OK) {
        mv = mv * 2;   /* 100k:100k 分压 → 电池电压 = ADC 电压 x2 */
    } else {
        mv = (int)((int64_t)raw * 3100 * 2 / 4095);   /* 回退：11dB 满量程约 3100mV，x2 分压 */
    }
    bat_last_mv = mv;
    return mv;
}

int bsp_battery_percent(void)
{
    int mv = bsp_battery_mv();
    if (mv < 0) return -1;
    int pct = (mv - 3300) * 100 / 900;   /* 3.3V=0% 4.2V=100% 线性近似 */
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

#endif /* CONFIG_BOARD_ESP32S3_ST7789 */
