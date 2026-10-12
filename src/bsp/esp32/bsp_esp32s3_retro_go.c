/*
 * esp32s3-retro-go: Chaeng "retro-go" ESP32-S3 掌机主板（T320-S3）。
 *   硬件来源：https://github.com/Chaeng3/retro-go_chaeng
 *   开源硬件页：https://oshwhub.com/chaeng/project_jofcnupz
 *
 * 3.2" T320B7-C12-16 IPS（ST7789，原生 240x320，横屏 320x240），SPI2 @ 40MHz，
 * RGB 颜色序（上游 MADCTL BGR=0），必须 INVON。无触摸；导航全部走板载 GPIO
 * 按键（方向键 + A/B/START/SELECT），经通用 bsp_gpio_buttons 后端进语义按钮层。
 * KEY_MENU(18)/KEY_OPTION(8)/KEY_BOOT(0) 本固件保留未映射——GPIO0 不注册为
 * 息屏按钮，避免输入职责混淆。SD 卡 / 音频 / 电量 ADC 本固件未使用。
 */
#include "sdkconfig.h"
#if CONFIG_BOARD_ESP32S3_RETRO_GO

#include "bsp.h"
#include "bsp_lcd_color_io.h"
#include "bsp_gpio_buttons.h"
#include "bsp_screen_power.h"

#include <string.h>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdlib.h>

/* 上游 retro-go t320-s3 目标实测引脚（components/retro-go/targets/t320-s3/config.h） */
#define PIN_LCD_SCLK       48
#define PIN_LCD_MOSI       12
#define PIN_LCD_CS         14
#define PIN_LCD_DC         47
#define PIN_LCD_RST        3
#define PIN_LCD_BL         39

/* 导航按键：全部内部上拉、低电平有效（上游 RG_GAMEPAD_GPIO_MAP）。
   确定 = A(15) / START(17) / SELECT(16) 三键并联，返回 = B(5)。 */
#define PIN_KEY_UP          7
#define PIN_KEY_DOWN       20
#define PIN_KEY_LEFT       19
#define PIN_KEY_RIGHT       6
#define PIN_KEY_OK_A       15
#define PIN_KEY_OK_START   17
#define PIN_KEY_OK_SELECT  16
#define PIN_KEY_BACK_B      5

#define LCD_H_RES         320
#define LCD_V_RES         240
#define LCD_SPI_HZ         (40 * 1000 * 1000)
#define DRAW_BUF_LINES     40

static const char *TAG = "bsp";
static esp_lcd_panel_handle_t panel_handle;
static SemaphoreHandle_t lvgl_mux;
static SemaphoreHandle_t lcd_done;

static bool on_color_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *event,
                          void *user_ctx)
{
    (void)io;
    (void)event;
    (void)user_ctx;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(lcd_done, &wake);
    return wake == pdTRUE;
}

static void draw_wait(int x1, int y1, int x2, int y2, const void *pixels)
{
    xSemaphoreTake(lcd_done, 0);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, x1, y1,
                                               x2, y2, pixels));
    ESP_ERROR_CHECK(xSemaphoreTake(lcd_done, portMAX_DELAY) == pdTRUE
                        ? ESP_OK : ESP_FAIL);
}

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush_cb(lv_display_t *display, const lv_area_t *area,
                     uint8_t *pixels)
{
    uint16_t *p = (uint16_t *)pixels;
    int count = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    for (int i = 0; i < count; i++)
        p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));

    draw_wait(area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels);
    lv_display_flush_ready(display);
}

void bsp_lvgl_lock(void)
{
    xSemaphoreTakeRecursive(lvgl_mux, portMAX_DELAY);
}

void bsp_lvgl_unlock(void)
{
    xSemaphoreGiveRecursive(lvgl_mux);
}

lv_display_t *bsp_get_display(void)
{
    return lv_display_get_default();
}

void bsp_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void backlight_apply(int pct)
{
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                                  pct * 255 / 100));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}

static uint64_t screen_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

void bsp_lcd_push(int x, int y, int w, int h, const uint16_t *pixels)
{
    size_t count = (size_t)w * h;
    uint16_t *copy = heap_caps_malloc(count * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!copy) return;
    for (size_t i = 0; i < count; i++)
        copy[i] = (uint16_t)((pixels[i] >> 8) | (pixels[i] << 8));
    draw_wait(x, y, x + w, y + h, copy);
    free(copy);
}

bool bsp_disp_can_invert(void)
{
    return true;
}

void bsp_disp_set_invert(bool enabled)
{
    /* 本板 IPS 面板必须 INVON 才是正常颜色，"反色"开关在此基础上再翻一次 */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, !enabled));
}

static bool disp_rot180, disp_mirrorx;

bool bsp_disp_can_rotate180(void)
{
    return true;
}

bool bsp_disp_can_mirror_x(void)
{
    return true;
}

/* 基准横屏 = swap_xy + mirror(false, true)（上游 MADCTL 0xA0：MV|MY）。
   swap_xy 下屏幕水平轴对应面板 Y 轴：mx = 基线 ^ rot180；my = 基线 ^ rot180 ^ mirror_x */
static void panel_mirror_apply(void)
{
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle,
                                         disp_rot180,
                                         disp_rot180 == disp_mirrorx));
}

void bsp_disp_set_rotate180(bool enabled)
{
    disp_rot180 = enabled;
    panel_mirror_apply();
}

void bsp_disp_set_mirror_x(bool enabled)
{
    disp_mirrorx = enabled;
    panel_mirror_apply();
}

void bsp_fade_out(uint32_t ms)
{
    static bool fade_installed;
    if (!fade_installed) {
        ESP_ERROR_CHECK(ledc_fade_func_install(0));
        fade_installed = true;
    }
    ESP_ERROR_CHECK(ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE,
                                            LEDC_CHANNEL_0, 0, ms));
    ESP_ERROR_CHECK(ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                                    LEDC_FADE_WAIT_DONE));

    uint16_t black[LCD_H_RES];
    memset(black, 0, sizeof(black));   /* 栈上现场填一行全 0，逐行推，不留常驻缓冲 */
    for (int y = 0; y < LCD_V_RES; y++)
        draw_wait(0, y, LCD_H_RES, y + 1, black);
}

static void nav_buttons_init(void)
{
    static const bsp_gpio_button_cfg_t up[] = {
        { PIN_KEY_UP, BSP_GPIO_PULL_UP, true },
    };
    static const bsp_gpio_button_cfg_t down[] = {
        { PIN_KEY_DOWN, BSP_GPIO_PULL_UP, true },
    };
    static const bsp_gpio_button_cfg_t left[] = {
        { PIN_KEY_LEFT, BSP_GPIO_PULL_UP, true },
    };
    static const bsp_gpio_button_cfg_t right[] = {
        { PIN_KEY_RIGHT, BSP_GPIO_PULL_UP, true },
    };
    static const bsp_gpio_button_cfg_t ok[] = {
        { PIN_KEY_OK_A, BSP_GPIO_PULL_UP, true },
        { PIN_KEY_OK_START, BSP_GPIO_PULL_UP, true },
        { PIN_KEY_OK_SELECT, BSP_GPIO_PULL_UP, true },
    };
    static const bsp_gpio_button_cfg_t back[] = {
        { PIN_KEY_BACK_B, BSP_GPIO_PULL_UP, true },
    };
    ESP_ERROR_CHECK(bsp_gpio_buttons_bind(BSP_NAV_KEY_UP, up, 1));
    ESP_ERROR_CHECK(bsp_gpio_buttons_bind(BSP_NAV_KEY_DOWN, down, 1));
    ESP_ERROR_CHECK(bsp_gpio_buttons_bind(BSP_NAV_KEY_LEFT, left, 1));
    ESP_ERROR_CHECK(bsp_gpio_buttons_bind(BSP_NAV_KEY_RIGHT, right, 1));
    ESP_ERROR_CHECK(bsp_gpio_buttons_bind(BSP_NAV_KEY_OK, ok, 3));
    ESP_ERROR_CHECK(bsp_gpio_buttons_bind(BSP_NAV_KEY_BACK, back, 1));
}

static void lvgl_task(void *arg)
{
    (void)arg;
    for (;;) {
        bsp_lvgl_lock();
        lv_timer_handler();
        bsp_screen_power_poll();
        bsp_gpio_buttons_poll();
        bsp_lvgl_unlock();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void bsp_init(void)
{
    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    lcd_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(lvgl_mux && lcd_done ? ESP_OK : ESP_ERR_NO_MEM);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    esp_vfs_littlefs_conf_t fs = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&fs));

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t channel = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 255,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    bsp_screen_power_init(backlight_apply, screen_now_ms);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io));
    esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = on_color_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(
        io, &callbacks, NULL));

    ESP_ERROR_CHECK(bsp_lcd_color_io_wrap(&io, false));
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,   /* 上游 MADCTL BGR=0 */
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config,
                                              &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, true));
    /* IPS 面板必须 INVON，INVOFF 全屏反色（bsp_disp_set_invert 语义相应取反） */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);
    size_t buffer_bytes = LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t);
    void *buffer_a = heap_caps_malloc(buffer_bytes, MALLOC_CAP_DMA);
    void *buffer_b = heap_caps_malloc(buffer_bytes, MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(buffer_a && buffer_b ? ESP_OK : ESP_ERR_NO_MEM);
    lv_display_set_buffers(display, buffer_a, buffer_b, buffer_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_cb);

    /* No pointer input: navigation is entirely on the GPIO buttons. */
    nav_buttons_init();

    BaseType_t task_ok = xTaskCreatePinnedToCore(
        lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "BSP ready (esp32s3-retro-go, %dx%d)",
             LCD_H_RES, LCD_V_RES);
}

void bsp_restart(void)
{
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

#endif /* CONFIG_BOARD_ESP32S3_RETRO_GO */
