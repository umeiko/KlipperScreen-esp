/* Host test using the real ESP-IDF public IO interface and a fake transport. */
#include "bsp.h"
#include "bsp_lcd_color_io.h"
#include "esp_lcd_panel_io_interface.h"
#include <assert.h>
#include <stdio.h>

static esp_lcd_panel_io_t transport;
static esp_err_t next_error;
static int last_cmd;
static uint8_t last_byte;
static size_t last_size;
static const void *last_pixels;
static const esp_lcd_panel_io_callbacks_t *last_callbacks;
static void *last_context;

static esp_err_t tx(esp_lcd_panel_io_t *io, int cmd, const void *data, size_t size)
{
    assert(io == &transport);
    last_cmd = cmd;
    last_byte = size ? *(const uint8_t *)data : 0;
    last_size = size;
    return next_error;
}
static esp_err_t pixels(esp_lcd_panel_io_t *io, int cmd, const void *data, size_t size)
{
    last_pixels = data;
    return tx(io, cmd, data, size);
}
static esp_err_t callbacks(esp_lcd_panel_io_t *io,
    const esp_lcd_panel_io_callbacks_t *cbs, void *context)
{
    assert(io == &transport);
    last_callbacks = cbs;
    last_context = context;
    return next_error;
}
static esp_err_t destroy(esp_lcd_panel_io_t *io)
{
    assert(io == &transport);
    return next_error;
}

int main(void)
{
    transport = (esp_lcd_panel_io_t) {
        .tx_param = tx, .tx_color = pixels, .del = destroy,
        .register_event_callbacks = callbacks,
    };
    assert(!bsp_disp_can_color_order());
    assert(bsp_lcd_color_io_wrap(NULL, true) == ESP_ERR_INVALID_ARG);
    for (int native_bgr = 0; native_bgr <= 1; native_bgr++) {
        esp_lcd_panel_io_handle_t io = &transport;
        assert(bsp_lcd_color_io_wrap(&io, native_bgr) == ESP_OK);
        assert(io != &transport && bsp_disp_can_color_order());
        assert(!bsp_disp_set_color_order(BSP_COLOR_ORDER_RGB)); /* Before init. */
        esp_lcd_panel_io_handle_t second = &transport;
        assert(bsp_lcd_color_io_wrap(&second, true) == ESP_ERR_INVALID_STATE);
        for (int mode = 0; mode <= 2; mode++) {
            /* First mimic driver init, then all orientation/MADCTL bit patterns. */
            uint8_t initial = native_bgr ? 8 : 0;
            assert(io->tx_param(io, 0x36, &initial, 1) == ESP_OK);
            assert(bsp_disp_set_color_order(mode));
            bool bgr = mode == 0 ? native_bgr : mode == 2;
            for (unsigned raw = 0; raw <= 255; raw++) {
                uint8_t value = raw;
                assert(io->tx_param(io, 0x36, &value, 1) == ESP_OK);
                assert(last_cmd == 0x36 && last_size == 1);
                assert(last_byte == ((raw & ~8) | (bgr ? 8 : 0)));
                assert(value == raw); /* Never modify driver's buffer/cache. */
            }
            assert(bsp_disp_get_color_order() == (bsp_color_order_t)mode);
        }
        assert(!bsp_disp_set_color_order(3));
        uint8_t orientation = 0x60;
        assert(io->tx_param(io, 0x36, &orientation, 1) == ESP_OK);
        next_error = ESP_FAIL;
        assert(!bsp_disp_set_color_order(BSP_COLOR_ORDER_RGB));
        assert(bsp_disp_get_color_order() == BSP_COLOR_ORDER_BGR);
        orientation = 0xC0;
        assert(io->tx_param(io, 0x36, &orientation, 1) == ESP_FAIL);
        assert(io->del(io) == ESP_FAIL && bsp_disp_can_color_order());
        next_error = ESP_OK;
        assert(bsp_disp_set_color_order(BSP_COLOR_ORDER_RGB));
        assert(last_byte == 0x60); /* Failed MADCTL must not poison cached state. */
        assert(io->tx_param(io, 0x21, NULL, 0) == ESP_OK); /* Inversion unchanged. */
        assert(last_cmd == 0x21 && last_size == 0);
        uint8_t rgb666[] = { 0xFC, 0, 0, 0, 0xFC, 0 };
        assert(io->tx_color(io, 0x2C, rgb666, sizeof(rgb666)) == ESP_OK);
        assert(last_pixels == rgb666 && last_size == sizeof(rgb666));
        esp_lcd_panel_io_callbacks_t cbs = {0};
        assert(io->register_event_callbacks(io, &cbs, rgb666) == ESP_OK);
        assert(last_callbacks == &cbs && last_context == rgb666);
        assert(io->rx_param(io, 0x04, rgb666, 1) == ESP_ERR_NOT_SUPPORTED);
        assert(io->tx_param(io, 0x01, NULL, 0) == ESP_OK);
        assert(!bsp_disp_set_color_order(BSP_COLOR_ORDER_DEFAULT));
        assert(io->tx_param(io, 0x36, &orientation, 1) == ESP_OK);
        assert(last_byte == orientation); /* RGB override survives re-init. */
        assert(io->del(io) == ESP_OK && !bsp_disp_can_color_order());
    }
    puts("PASS: LCD RGB/BGR defaults, all MADCTL orientations, failure rollback, reset and DMA/callback passthrough");
    return 0;
}
