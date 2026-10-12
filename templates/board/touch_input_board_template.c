/*
 * Concrete capacitive-touch example: CST816S over I2C -> LVGL pointer.
 *
 * Keep the LVGL-facing shape when changing controller. The controller-specific
 * pieces are the include, IO macro, create call, and possibly the read policy.
 * CST816S should be read only after its interrupt says data is available.
 */
#include "sdkconfig.h"
#if CONFIG_BOARD_TEMPLATE

#include "touch_input_board_template.h"

#include "bsp.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define RELEASE_GRACE_US (80 * 1000)

static esp_lcd_touch_handle_t touch_handle;
static SemaphoreHandle_t touch_irq;
static uint16_t last_x;
static uint16_t last_y;
static int64_t last_point_us;
static bool pressing;
static bool wake_swallow;

static void touch_interrupt_cb(esp_lcd_touch_handle_t touch)
{
    (void)touch;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(touch_irq, &wake);
    if (wake == pdTRUE) portYIELD_FROM_ISR();
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int64_t now = esp_timer_get_time();

    /* CST816S answers I2C for a short time after an event. Do not poll it
       blindly on every LVGL read. */
    if (xSemaphoreTake(touch_irq, 0) == pdTRUE) {
        esp_lcd_touch_point_data_t point[1] = {0};
        uint8_t count = 0;

        if (esp_lcd_touch_read_data(touch_handle) == ESP_OK &&
            esp_lcd_touch_get_data(touch_handle, point, &count, 1) == ESP_OK) {
            if (count > 0) {
                if (bsp_screen_activity()) wake_swallow = true;
                last_x = point[0].x;
                last_y = point[0].y;
                last_point_us = now;
                pressing = true;
            } else {
                pressing = false;
            }
        }
    }

    /* Some modules do not report an explicit release. Keep the last point for
       a short grace period so a drag does not flicker between press/release. */
    if (pressing && now - last_point_us <= RELEASE_GRACE_US) {
        if (wake_swallow) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        data->point.x = last_x;
        data->point.y = last_y;
        data->state = LV_INDEV_STATE_PRESSED;
        return;
    }

    pressing = false;
    wake_swallow = false;
    data->state = LV_INDEV_STATE_RELEASED;
}

esp_err_t board_template_touch_input_create(
    lv_display_t *display,
    i2c_master_bus_handle_t i2c_bus,
    const board_template_touch_input_config_t *config)
{
    if (!display || !i2c_bus || !config ||
        config->interrupt_gpio == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_ARG;
    }

    touch_irq = xSemaphoreCreateBinary();
    if (!touch_irq) return ESP_ERR_NO_MEM;

    esp_lcd_panel_io_handle_t touch_io;
    esp_lcd_panel_io_i2c_config_t io_config =
        ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    esp_err_t err = esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &touch_io);
    if (err != ESP_OK) return err;

    esp_lcd_touch_config_t touch_config = {
        .x_max = config->h_res,
        .y_max = config->v_res,
        .rst_gpio_num = config->reset_gpio,
        .int_gpio_num = config->interrupt_gpio,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = {
            .swap_xy = config->swap_xy,
            .mirror_x = config->mirror_x,
            .mirror_y = config->mirror_y,
        },
        .interrupt_callback = touch_interrupt_cb,
    };
    err = esp_lcd_touch_new_i2c_cst816s(touch_io, &touch_config,
                                         &touch_handle);
    if (err != ESP_OK) return err;

    lv_indev_t *input = lv_indev_create();
    if (!input) return ESP_ERR_NO_MEM;
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input, display);
    lv_indev_set_read_cb(input, touch_read_cb);
    return ESP_OK;
}

#endif /* CONFIG_BOARD_TEMPLATE */
