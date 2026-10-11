#pragma once

/* Minimal TFT_eSPI setup for the onboard Wio Terminal ILI9341. The upstream
 * Seeed setup also links several unused bitmap fonts; omitting them saves
 * precious SAMD51 internal flash because LVGL renders all text itself. */
#define USER_SETUP_ID 500
#define ILI9341_DRIVER
#define TFT_SPI_PORT LCD_SPI
#define TFT_CS LCD_SS_PIN
#define TFT_DC LCD_DC
#define TFT_RST LCD_RESET
#define TFT_BL LCD_BACKLIGHT
#define TFT_BACKLIGHT_ON HIGH
#define SPI_FREQUENCY 50000000
#define SPI_READ_FREQUENCY 20000000
