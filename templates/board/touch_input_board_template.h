#pragma once

/*
 * Board-local touch adapter example.
 *
 * This header deliberately hides the concrete touch controller from the BSP.
 * The matching .c file is a complete CST816S example. Rename every
 * "board_template" token when copying it for a real board.
 */
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "lvgl.h"

typedef struct {
    uint16_t h_res;
    uint16_t v_res;
    gpio_num_t reset_gpio;
    gpio_num_t interrupt_gpio;
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
} board_template_touch_input_config_t;

/* Creates the CST816S driver and registers one LVGL pointer input. */
esp_err_t board_template_touch_input_create(
    lv_display_t *display,
    i2c_master_bus_handle_t i2c_bus,
    const board_template_touch_input_config_t *config);
