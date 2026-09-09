#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include <stdbool.h>

typedef struct {
    int gpio_a;
    int gpio_b;
    int gpio_button;
    int counts_per_detent;
    int button_debounce_ms;
    uint32_t glitch_filter_ns;
    bool phase_pullups;
    bool reverse;
    bool button_active_low;
} bsp_rotary_encoder_config_t;

/*
 * Create a PCNT-backed LVGL encoder input device. The returned input device is
 * intentionally not board-global: the UI navigation layer discovers all
 * encoder/keypad devices and binds them to the active focus scope.
 */
esp_err_t bsp_rotary_encoder_create(const bsp_rotary_encoder_config_t *config,
                                    lv_display_t *display,
                                    lv_indev_t **out_indev);
