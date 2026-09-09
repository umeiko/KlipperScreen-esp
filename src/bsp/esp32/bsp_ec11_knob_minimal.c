/*
 * Official "EC11 Knob Minimal System" breadboard reference:
 *   ESP32-S3-DevKitC-1 N16R8 + 240x320 ST7789 SPI display + EC11 module.
 *
 * The pinout follows the hardware validated in contributor PR #6. This BSP
 * owns only the display. The EC11 is created by shared bsp_input_init() from
 * this board's sdkconfig, so the navigation policy stays board-independent.
 * There is deliberately no touch controller. Screen-off/wake buttons: the
 * DevKitC BOOT key (GPIO0) plus a dedicated button on GPIO39 (to GND,
 * internal pull-up, active-low).
 */
#include "sdkconfig.h"
#if CONFIG_BOARD_EC11_KNOB_MINIMAL

#include "bsp.h"
#include "bsp_screen_power.h"
#include "bsp_sleep_button.h"

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

/* PR #6 reference wiring: ST7789 SPI module. */
#define PIN_LCD_SCLK       21
#define PIN_LCD_MOSI       47
#define PIN_LCD_CS         41
#define PIN_LCD_DC         40
#define PIN_LCD_RST        45
#define PIN_LCD_BL         42

/* 息屏/唤醒按钮：DevKitC 板载 BOOT 键 + 外挂独立按钮（GPIO39 ── 按键 ── GND，
   内部上拉、低电平有效）。两个按钮等效，任意一个都可息屏/唤醒。 */
#define PIN_BTN_BOOT        0
#define PIN_BTN_SLEEP      39

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
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, enabled));
}

bool bsp_disp_can_rotate180(void)
{
    return true;
}

void bsp_disp_set_rotate180(bool enabled)
{
    /* Baseline is swap_xy + mirror(true, false), matching PR #6. */
    if (enabled)
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, true));
    else
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
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

    static uint16_t black[LCD_H_RES * DRAW_BUF_LINES];
    for (int y = 0; y < LCD_V_RES; y += DRAW_BUF_LINES)
        draw_wait(0, y, LCD_H_RES, y + DRAW_BUF_LINES, black);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    for (;;) {
        bsp_lvgl_lock();
        lv_timer_handler();
        bsp_screen_power_poll();
        bsp_sleep_button_poll();
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

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config,
                                              &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
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

    /* No pointer input: this official wiring is controlled entirely by EC11. */

    /* 息屏/唤醒按钮（BOOT + GPIO39 外挂按钮，低电平有效） */
    const bsp_sleep_button_cfg_t sleep_btns[] = {
        { PIN_BTN_BOOT, true },
        { PIN_BTN_SLEEP, true },
    };
    ESP_ERROR_CHECK(bsp_sleep_button_init(sleep_btns,
                                          sizeof(sleep_btns) / sizeof(sleep_btns[0])));

    BaseType_t task_ok = xTaskCreatePinnedToCore(
        lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "BSP ready (EC11 Knob Minimal System, %dx%d)",
             LCD_H_RES, LCD_V_RES);
}

void bsp_restart(void)
{
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

#endif /* CONFIG_BOARD_EC11_KNOB_MINIMAL */
