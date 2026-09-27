/* Use ESP-IDF's public IO interface, never a panel driver's private layout.
 * A one-off MADCTL write is insufficient: mirror/swap_xy later sends the
 * driver's cached color bit again. Intercept every MADCTL instead. */
#include "bsp_lcd_color_io.h"
#include "bsp.h"
#include "esp_lcd_panel_io_interface.h"
#include <stdint.h>
#include <stdlib.h>

#define LCD_MADCTL 0x36
#define LCD_SWRESET 0x01
#define LCD_BGR 0x08

typedef struct {
    esp_lcd_panel_io_t base;
    esp_lcd_panel_io_handle_t transport;
    uint8_t madctl;
    bool valid;
    bool bgr;
} color_io_t;

static color_io_t *display_io;

static uint8_t with_color(uint8_t value, bool bgr)
{
    return (value & ~LCD_BGR) | (bgr ? LCD_BGR : 0);
}

static bool apply_color(bool bgr)
{
    color_io_t *io = display_io;
    if (!io || !io->valid) return false;
    uint8_t value = with_color(io->madctl, bgr);
    /* SPI tx_param drains pending DMA color transfers and completes before
     * returning, so the stack byte is safe and no flush races this command. */
    if (io->transport->tx_param(io->transport, LCD_MADCTL, &value, 1) != ESP_OK)
        return false;
    io->bgr = bgr;
    return true;
}

static esp_err_t color_tx_param(esp_lcd_panel_io_t *base, int cmd,
                                const void *param, size_t size)
{
    color_io_t *io = (color_io_t *)base;
    if (cmd == LCD_MADCTL && param && size == 1) {
        uint8_t raw = *(const uint8_t *)param;
        uint8_t value = with_color(raw, io->bgr);
        esp_err_t err = io->transport->tx_param(io->transport, cmd, &value, 1);
        if (err == ESP_OK) {
            io->madctl = raw;
            io->valid = true;
        }
        return err;
    }
    esp_err_t err = io->transport->tx_param(io->transport, cmd, param, size);
    if (err == ESP_OK && cmd == LCD_SWRESET) io->valid = false;
    return err;
}

static esp_err_t color_rx_param(esp_lcd_panel_io_t *base, int cmd,
                                void *param, size_t size)
{
    color_io_t *io = (color_io_t *)base;
    return io->transport->rx_param
        ? io->transport->rx_param(io->transport, cmd, param, size)
        : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t color_tx_color(esp_lcd_panel_io_t *base, int cmd,
                                const void *pixels, size_t size)
{
    color_io_t *io = (color_io_t *)base;
    /* No copying/conversion; keep RGB565 and ILI9488's RGB666 DMA unchanged. */
    return io->transport->tx_color(io->transport, cmd, pixels, size);
}

static esp_err_t color_register_callbacks(esp_lcd_panel_io_t *base,
    const esp_lcd_panel_io_callbacks_t *callbacks, void *context)
{
    color_io_t *io = (color_io_t *)base;
    return io->transport->register_event_callbacks
        ? io->transport->register_event_callbacks(io->transport, callbacks, context)
        : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t color_del(esp_lcd_panel_io_t *base)
{
    color_io_t *io = (color_io_t *)base;
    esp_err_t err = io->transport->del(io->transport);
    if (err != ESP_OK) return err;
    bsp_disp_color_order_register(false, NULL);
    display_io = NULL;
    free(io);
    return ESP_OK;
}

esp_err_t bsp_lcd_color_io_wrap(esp_lcd_panel_io_handle_t *handle, bool default_bgr)
{
    if (!handle || !*handle || !(*handle)->tx_param || !(*handle)->tx_color ||
        !(*handle)->del) return ESP_ERR_INVALID_ARG;
    if (display_io) return ESP_ERR_INVALID_STATE;
    color_io_t *io = calloc(1, sizeof(*io));
    if (!io) return ESP_ERR_NO_MEM;
    io->base = (esp_lcd_panel_io_t) {
        .rx_param = color_rx_param,
        .tx_param = color_tx_param,
        .tx_color = color_tx_color,
        .del = color_del,
        .register_event_callbacks = color_register_callbacks,
    };
    io->transport = *handle;
    io->bgr = default_bgr;
    display_io = io;
    *handle = &io->base;
    bsp_disp_color_order_register(default_bgr, apply_color);
    return ESP_OK;
}
