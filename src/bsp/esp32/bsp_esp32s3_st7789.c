/*
 * BSP: ESP32-S3 N16R8 + ST7789 240x320 SPI 屏（无触摸）+ EC11 编码器 + 4 快捷键 + 电池检测
 * 逻辑分辨率 320x240 横屏。
 *
 * 引脚分配：
 *   ST7789 SPI:  clk=12  sda(MOSI)=11  cs=13  dc=14  rst=10  bckl=9
 *   EC11:        A=47  B=21  key=48
 *   息屏按键:    GPIO4（低有效，内部上拉）
 *   快捷键:      GPIO5/6/7/15 → 打印机槽位 0/1/2/3（按下选中，旋钮确认切换）
 *   电池检测:    GPIO8 = ADC1_CH7，100k:100k 分压（电池电压 = ADC 电压 x2）
 */
#include "sdkconfig.h"
#if CONFIG_BOARD_ESP32S3_ST7789

#include "bsp.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_littlefs.h"
#include "bsp_screen_power.h"
#include "bsp_sleep_button.h"
#include "bsp_wifi.h"
#include "esp_log.h"
extern void ui_toast(const char *text, uint32_t accent_hex);   /* src/ui/ui_anim.h */
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ---------- 引脚定义 ---------- */
/* LCD SPI (ST7789) */
#define PIN_LCD_SCLK   12
#define PIN_LCD_MOSI   11
#define PIN_LCD_MISO   (-1)   /* ST7789 只写不读 */
#define PIN_LCD_CS     13
#define PIN_LCD_DC     14
#define PIN_LCD_RST    10
#define PIN_LCD_BL     9      /* 高电平点亮 */

/* 旋转编码器 EC11：A/B 相位 + 按键，内部上拉，公共端接 GND */
#define PIN_ENC_A       47
#define PIN_ENC_B       21
#define PIN_ENC_KEY     48

/* 一键息屏/唤醒按键（低有效，内部上拉，公共端接 GND） */
#define PIN_SLEEP_BTN   4

/* 快捷键：GPIO5/6/7/15 → 打印机槽位 0/1/2/3 */
#define HOTKEY_COUNT    4
static const int PIN_HOTKEY[HOTKEY_COUNT] = { 5, 6, 7, 15 };

/* 电池 ADC：GPIO8 = ADC1_CH7，100k:100k 分压 */
#define BAT_ADC_UNIT    ADC_UNIT_1
#define BAT_ADC_CH      ADC_CHANNEL_7
#define BAT_ADC_ATTEN   ADC_ATTEN_DB_11

#define LCD_H_RES      320    /* 240x320 面板右转 90° → 横屏 320x240 */
#define LCD_V_RES      240
#define LCD_SPI_HZ     (40 * 1000 * 1000)
#define DRAW_BUF_LINES 40

/* 快捷键确认窗口（ms）：按下快捷键后 N 毫秒内按编码器旋钮确认 */
#define HOTKEY_CONFIRM_MS  3000

static const char *TAG = "bsp";

static SemaphoreHandle_t lvgl_mux;
static adc_oneshot_unit_handle_t bat_adc;
static adc_cali_handle_t bat_cali;
static int bat_last_mv = -1;
static esp_lcd_panel_handle_t panel_handle;
static lv_indev_t *enc_indev;
static pcnt_unit_handle_t enc_pcnt;
static int enc_last_cnt;
static int enc_acc;
static int enc_key_raw = 1;
static int enc_key_state = 1;
static int64_t enc_key_debounce_ms;

/* 快捷键切换打印机：待确认槽位（-1 = 无待确认） */
static int pending_slot = -1;
static int64_t pending_until_ms = 0;
static void (*hotkey_printer_fn)(int slot) = NULL;
static void (*hotkey_select_fn)(int slot) = NULL;

void bsp_lvgl_lock(void)   { xSemaphoreTakeRecursive(lvgl_mux, portMAX_DELAY); }
void bsp_lvgl_unlock(void) { xSemaphoreGiveRecursive(lvgl_mux); }

/* ---------- 开机动画推屏 ---------- */
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
    size_t n = (size_t)w * h;
    uint16_t *tmp = malloc(n * 2);
    if (!tmp) return;
    for (size_t i = 0; i < n; i++) tmp[i] = (uint16_t)((px[i] >> 8) | (px[i] << 8));
    xSemaphoreTake(lcd_trans_done, 0);
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, tmp);
    xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(500));
    free(tmp);
}

void bsp_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

/* ---------- 背光 + 屏幕电源 ---------- */
static void backlight_apply(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)pct * 255 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static uint64_t screen_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

void bsp_fade_out(uint32_t ms)
{
    static bool fade_installed;
    if (!fade_installed) { ledc_fade_func_install(0); fade_installed = true; }
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0, ms);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);

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
    uint16_t *p = (uint16_t *)px_map;
    int32_t n = (int32_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    for (int32_t i = 0; i < n; i++) p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
    lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    int cnt = 0;
    pcnt_unit_get_count(enc_pcnt, &cnt);
    int diff = (int)((int16_t)(cnt - enc_last_cnt));
    enc_acc += diff;
    int step = 0;
    while (enc_acc >= 4) { step++; enc_acc -= 4; }
    while (enc_acc <= -4) { step--; enc_acc += 4; }
    data->enc_diff = step;
    enc_last_cnt = cnt;

    /* 按键（低有效）：去抖 */
    int64_t now = now_ms();
    int cur = gpio_get_level(PIN_ENC_KEY);
    if (cur != enc_key_raw) {
        enc_key_raw = cur;
        enc_key_debounce_ms = now;
    }
    bool pressed_stable;
    if (now - enc_key_debounce_ms >= 30) {
        enc_key_state = cur;
        pressed_stable = (cur == 0);
    } else {
        pressed_stable = (enc_key_state == 0);
    }
    data->state = pressed_stable ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;

    /* 快捷键确认：待确认期间按编码器旋钮 → 执行切换 */
    if (pending_slot >= 0 && pressed_stable && now < pending_until_ms) {
        ESP_LOGI(TAG, "Hotkey confirm: switch to printer slot %d", pending_slot);
        if (hotkey_printer_fn) hotkey_printer_fn(pending_slot);
        pending_slot = -1;
    }
    /* 旋转编码器取消待确认 */
    if (step != 0 && pending_slot >= 0) pending_slot = -1;

    /* 活动唤醒 */
    if (step != 0 || pressed_stable)
        bsp_screen_activity();
}

/* 无触摸板：pointer 输入设备始终返回释放 */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    data->state = LV_INDEV_STATE_RELEASED;
}

/* ---------- 快捷键轮询 ---------- */
static int hotkey_prev_level[HOTKEY_COUNT];

static void hotkey_poll(void)
{
    int64_t now = now_ms();

    /* 超时取消 */
    if (pending_slot >= 0 && now >= pending_until_ms) {
        ESP_LOGI(TAG, "Hotkey confirm timeout (slot %d)", pending_slot);
        pending_slot = -1;
    }

    for (int i = 0; i < HOTKEY_COUNT; i++) {
        int lvl = gpio_get_level(PIN_HOTKEY[i]);
        /* 按下沿（高→低） */
        if (hotkey_prev_level[i] == 1 && lvl == 0) {
            ESP_LOGI(TAG, "Hotkey %d (GPIO%d): select printer slot %d",
                     i + 1, PIN_HOTKEY[i], i);
            if (hotkey_select_fn) hotkey_select_fn(i);
            bsp_screen_activity();
        }
        hotkey_prev_level[i] = lvl;
    }
}

/* ---------- 长按息屏键 5s 开启 SoftAP（手机后台配置页） ---------- */
#define SLEEP_KEY_LONG_MS   5000
static int64_t sleep_key_down_since = -1;
static bool    sleep_key_long_fired = false;

static void sleep_key_long_poll(void)
{
    int64_t now = now_ms();
    int lvl = gpio_get_level(PIN_SLEEP_BTN);
    if (lvl == 0) {   /* 按下（低有效） */
        if (sleep_key_down_since < 0) {
            sleep_key_down_since = now;
            sleep_key_long_fired = false;
        } else if (!sleep_key_long_fired && now - sleep_key_down_since >= SLEEP_KEY_LONG_MS) {
            sleep_key_long_fired = true;
            ESP_LOGI(TAG, "Sleep key long-press 5s: enable SoftAP KlipperScreen-AP");
            bsp_wifi_start_ap("KlipperScreen-AP", "12345678");
            extern void web_config_server_start(void);
            web_config_server_start();   /* 热点开启后仅此时提供后台配置页 */
            bsp_screen_activity();   /* 唤醒屏幕给反馈 */
            ui_toast("热点已开启 KlipperScreen-AP", 0x00A651);
        }
    } else {
        sleep_key_down_since = -1;
        sleep_key_long_fired = false;
    }
}

static void lvgl_task(void *arg)
{
    for (;;) {
        bsp_lvgl_lock();
        lv_timer_handler();
        bsp_screen_power_poll();
        bsp_sleep_button_poll();
        sleep_key_long_poll();
        hotkey_poll();
        extern void screensaver_poll(void);
        screensaver_poll();
        bsp_lvgl_unlock();
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
    bsp_screen_power_init(backlight_apply, screen_now_ms);

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
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, /* 本模组为 RGB 序；BGR 会红蓝互换（急停红显示成蓝） */
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

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
    ESP_ERROR_CHECK(gpio_config(&enc_io_cfg));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(enc_ch_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(enc_ch_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(enc_ch_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(enc_ch_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_unit_enable(enc_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(enc_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_start(enc_pcnt));
    ESP_LOGI(TAG, "Encoder ready (A=GPIO%d B=GPIO%d KEY=GPIO%d)",
             PIN_ENC_A, PIN_ENC_B, PIN_ENC_KEY);

    /* 息屏/唤醒按钮（GPIO4，低电平有效） */
    const bsp_sleep_button_cfg_t sleep_btns[] = {
        { PIN_SLEEP_BTN, true },
    };
    ESP_ERROR_CHECK(bsp_sleep_button_init(sleep_btns, 1));

    /* 快捷键 GPIO5/6/7/15 → 槽位 0/1/2/3，内部上拉，低有效 */
    uint64_t hk_mask = 0;
    for (int i = 0; i < HOTKEY_COUNT; i++) {
        hk_mask |= (1ULL << PIN_HOTKEY[i]);
        hotkey_prev_level[i] = 1;   /* 上电默认释放 */
    }
    gpio_config_t hk_io = {
        .pin_bit_mask = hk_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&hk_io));

    /* LVGL */
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    size_t buf_sz = LCD_H_RES * DRAW_BUF_LINES * 2;
    void *buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
    void *buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(buf1 && buf2 ? ESP_OK : ESP_ERR_NO_MEM);
    lv_display_set_buffers(disp, buf1, buf2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    /* 无触摸板：pointer indev 空操作（保留以兼容 UI 焦点逻辑） */
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    enc_indev = lv_indev_create();
    lv_indev_set_type(enc_indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(enc_indev, encoder_read_cb);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);

    /* 电池 ADC：GPIO8 = ADC1_CH7，100k:100k 分压 */
    adc_oneshot_unit_init_cfg_t bat_cfg = { .unit_id = BAT_ADC_UNIT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&bat_cfg, &bat_adc));
    adc_oneshot_chan_cfg_t bat_chan = { .atten = BAT_ADC_ATTEN, .bitwidth = ADC_BITWIDTH_12 };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(bat_adc, BAT_ADC_CH, &bat_chan));
    adc_cali_scheme_ver_t cali_mask = 0;
    if (adc_cali_check_scheme(&cali_mask) == ESP_OK && (cali_mask & ADC_CALI_SCHEME_VER_CURVE_FITTING)) {
        adc_cali_curve_fitting_config_t ccfg = {
            .unit_id = BAT_ADC_UNIT, .chan = BAT_ADC_CH,
            .atten = BAT_ADC_ATTEN, .bitwidth = ADC_BITWIDTH_12,
        };
        if (adc_cali_create_scheme_curve_fitting(&ccfg, &bat_cali) != ESP_OK) bat_cali = NULL;
    } else {
        bat_cali = NULL;
    }

    ESP_LOGI(TAG, "BSP ready (ESP32-S3 ST7789 no-touch, %dx%d, battery on GPIO8)",
             LCD_H_RES, LCD_V_RES);
}

void bsp_restart(void)
{
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

lv_indev_t *bsp_encoder_indev(void) { return enc_indev; }

/* ---------- 显示设置：反色 / 180° 旋转 ---------- */
bool bsp_disp_can_invert(void) { return true; }
void bsp_disp_set_invert(bool en) { esp_lcd_panel_invert_color(panel_handle, en); }

bool bsp_disp_can_rotate180(void) { return true; }
void bsp_disp_set_rotate180(bool en)
{
    if (en) esp_lcd_panel_mirror(panel_handle, false, true);
    else    esp_lcd_panel_mirror(panel_handle, true, false);
}

/* ---------- 电池检测 ---------- */
int bsp_battery_mv(void)
{
    int raw = 0;
    if (adc_oneshot_read(bat_adc, BAT_ADC_CH, &raw) != ESP_OK) return bat_last_mv;
    for (int i = 0; i < 7; i++) {
        int r = 0;
        if (adc_oneshot_read(bat_adc, BAT_ADC_CH, &r) == ESP_OK && r > raw) raw = r;
    }
    int mv;
    if (bat_cali && adc_cali_raw_to_voltage(bat_cali, raw, &mv) == ESP_OK) {
        mv = mv * 2;   /* 100k:100k 分压 → 电池电压 = ADC 电压 x2 */
    } else {
        mv = (int)((int64_t)raw * 3100 * 2 / 4095);
    }
    bat_last_mv = mv;
    return mv;
}

int bsp_battery_percent(void)
{
    int mv = bsp_battery_mv();
    if (mv < 0) return -1;
    int pct = (mv - 3300) * 100 / 900;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

/* ---------- 快捷键打印机切换回调注册 ---------- */
void bsp_hotkey_set_printer_handler(void (*handler)(int slot))
{
    hotkey_printer_fn = handler;
}
void bsp_hotkey_set_select_handler(void (*handler)(int slot))
{
    hotkey_select_fn = handler;
}

#endif /* CONFIG_BOARD_ESP32S3_ST7789 */
