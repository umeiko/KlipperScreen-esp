/*
 * New-board BSP template: minimal SPI command-panel / ST7789 example.
 *
 * Copy this file; do not add it to the build unchanged. Search for TODO(board).
 * Optional rotary input is initialized by bsp_input_esp32.c from Kconfig and
 * intentionally does not appear here.
 *
 * IMPORTANT: the public BSP lifecycle is reusable, but the display transport
 * below is not universal. I80, RGB/DOTCLK, QSPI and MIPI panels need different
 * bus, framebuffer and flush code. Classify and smoke-test the display first;
 * see Step 3 of docs/porting.md or docs/porting.zh.md.
 */
#include "sdkconfig.h"
#if CONFIG_BOARD_TEMPLATE

#error "TODO(board): rename CONFIG_BOARD_TEMPLATE, fill the pin table, then remove this line"

#include "bsp.h"
#include "bsp_screen_power.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_littlefs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdlib.h>

/* TODO(board): values below are examples, not a known board pinout. */
#define PIN_LCD_SCLK       12
#define PIN_LCD_MOSI       11
#define PIN_LCD_CS         10
#define PIN_LCD_DC          9
#define PIN_LCD_RST         8
#define PIN_LCD_BL          7
#define LCD_H_RES         320
#define LCD_V_RES         240
#define LCD_SPI_HZ         (40 * 1000 * 1000)
#define DRAW_BUF_LINES     32

/* Set to 1 only when this product has touch.  The supplied adapter is a
 * concrete CST816S example; replace its controller-specific code for another
 * touch chip.  A no-touch/rotary-only product leaves this at 0. */
#define BOARD_HAS_TOUCH 0
#define PIN_TOUCH_SDA      GPIO_NUM_NC
#define PIN_TOUCH_SCL      GPIO_NUM_NC
#define PIN_TOUCH_RST      GPIO_NUM_NC
#define PIN_TOUCH_INT      GPIO_NUM_NC

#if BOARD_HAS_TOUCH
#include "touch_input_board_template.h"
#endif

static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t lvgl_mux;
static SemaphoreHandle_t lcd_done;

static bool on_color_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *event,
                          void *user_ctx)
{
    (void)io; (void)event; (void)user_ctx;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(lcd_done, &wake);
    return wake == pdTRUE;
}

static void draw_wait(int x1, int y1, int x2, int y2, const void *pixels)
{
    xSemaphoreTake(lcd_done, 0);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, pixels));
    xSemaphoreTake(lcd_done, portMAX_DELAY);
}

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    uint16_t *p = (uint16_t *)pixels;
    int count = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    for (int i = 0; i < count; i++) p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));
    draw_wait(area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels);
    lv_display_flush_ready(display);
}

void bsp_lvgl_lock(void)   { xSemaphoreTakeRecursive(lvgl_mux, portMAX_DELAY); }
void bsp_lvgl_unlock(void) { xSemaphoreGiveRecursive(lvgl_mux); }
lv_display_t *bsp_get_display(void) { return lv_display_get_default(); }
void bsp_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

/* This is the only board-specific part of brightness control.  The shared
 * state machine stores the user's 0..100 value and supplies 0 while off. */
static void backlight_apply(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    /* TODO(board): invert this duty when the backlight is active-low. */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 255 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static uint64_t screen_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

void bsp_lcd_push(int x, int y, int w, int h, const uint16_t *pixels)
{
    size_t bytes = (size_t)w * h * sizeof(uint16_t);
    uint16_t *copy = heap_caps_malloc(bytes, MALLOC_CAP_DMA);
    if (!copy) return;
    for (int i = 0; i < w * h; i++)
        copy[i] = (uint16_t)((pixels[i] >> 8) | (pixels[i] << 8));
    draw_wait(x, y, x + w, y + h, copy);
    free(copy);
}

void bsp_restart(void)
{
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void bsp_fade_out(uint32_t ms)
{
    backlight_apply(0);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* TODO(board): expose these only after validating the panel commands/orientation. */
bool bsp_disp_can_invert(void) { return false; }
bool bsp_disp_can_rotate180(void) { return false; }
void bsp_disp_set_invert(bool enabled) { (void)enabled; }
void bsp_disp_set_rotate180(bool enabled) { (void)enabled; }
void bsp_time_sync_from_host(const char *host, uint16_t port) { (void)host; (void)port; }

static void lvgl_task(void *arg)
{
    (void)arg;
    for (;;) {
        bsp_lvgl_lock();
        lv_timer_handler();
        bsp_screen_power_poll();
        bsp_lvgl_unlock();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void bsp_init(void)
{
    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    lcd_done = xSemaphoreCreateBinary();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    esp_vfs_littlefs_conf_t fs = {
        .base_path = "/littlefs", .partition_label = "storage",
        .format_if_mount_failed = true, .dont_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&fs));

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t channel = {
        .gpio_num = PIN_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 255,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    bsp_screen_power_init(backlight_apply, screen_now_ms);

    /* DISPLAY TRANSPORT: SPI command-panel example only. Replace this entire
     * block for I80, RGB/DOTCLK, QSPI or MIPI; changing only the panel factory
     * is not enough for a different physical interface. */
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK, .mosi_io_num = PIN_LCD_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC, .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_SPI_HZ, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 10,
    };
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                              &io_config, &io));
    esp_lcd_panel_io_callbacks_t callbacks = { .on_color_trans_done = on_color_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &callbacks, NULL));
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    /* TODO(board): set swap/mirror/gap/invert for the physical panel. */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);
    size_t buffer_bytes = LCD_H_RES * DRAW_BUF_LINES * 2;
    void *buffer_a = heap_caps_malloc(buffer_bytes, MALLOC_CAP_DMA);
    void *buffer_b = heap_caps_malloc(buffer_bytes, MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(buffer_a && buffer_b ? ESP_OK : ESP_ERR_NO_MEM);
    lv_display_set_buffers(display, buffer_a, buffer_b, buffer_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_cb);

    /* Optional concrete input example: CST816S capacitive touch over I2C.
       Reuse an existing I2C bus handle here if this board already created one. */
#if BOARD_HAS_TOUCH
    i2c_master_bus_config_t touch_i2c_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    i2c_master_bus_handle_t touch_i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&touch_i2c_config, &touch_i2c_bus));

    board_template_touch_input_config_t touch_config = {
        .h_res = LCD_H_RES,
        .v_res = LCD_V_RES,
        .reset_gpio = PIN_TOUCH_RST,
        .interrupt_gpio = PIN_TOUCH_INT,
        .swap_xy = false,
        .mirror_x = false,
        .mirror_y = false,
    };
    ESP_ERROR_CHECK(board_template_touch_input_create(
        display, touch_i2c_bus, &touch_config));
#endif

    /* Resistive touch needs a board-specific calibration path instead. A
       rotary-only board creates no pointer here. Shared bsp_input_init()
       creates the optional rotary encoder from Kconfig after bsp_init(). */

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);
}

#endif /* CONFIG_BOARD_TEMPLATE */
