#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

/* Wrap a standard 8-bit-command SPI LCD IO BEFORE creating its panel.
 * Tracks MADCTL (0x36), overriding only RGB/BGR bit 3. Driver-owned rotation
 * and mirror state stays intact. One display per BSP; callbacks still receive
 * the original transport IO, with their original context and ISR path.
 * Init and subsequent settings changes must hold the BSP LVGL lock. */
esp_err_t bsp_lcd_color_io_wrap(esp_lcd_panel_io_handle_t *io, bool default_bgr);
