# Porting to your own board

This project confines all board differences to a **BSP (board support package)** layer: the UI, the Moonraker protocol and networking code are completely hardware-agnostic. Porting = writing one BSP implementation file for your board + a few registry-style additions.

## 1. The BSP contract

Every board (including the desktop simulator) implements the same interface, defined in [`src/bsp/bsp.h`](https://github.com/umeiko/KlipperScreen-esp/blob/main/src/bsp/bsp.h):

| Function | Semantics |
|---|---|
| `bsp_init()` | Board init: display + touch + LVGL tick task. Called first by `app_main` |
| `bsp_get_display()` | Returns `lv_display_get_default()` |
| `bsp_lvgl_lock()` / `bsp_lvgl_unlock()` | LVGL thread mutex (recursive); every LVGL API call must hold it |
| `bsp_restart()` | Restart the device (render the "restarting" toast first, then `esp_restart`) |
| `bsp_lcd_push(x,y,w,h,px)` | Push a raw RGB565 block to the screen (boot animation and other non-LVGL rendering) |
| `bsp_delay_ms(ms)` | Millisecond delay |
| `bsp_set_brightness(0-100)` | Backlight brightness (LEDC PWM on ESP32) |
| `bsp_fade_out(ms)` | Fade backlight to black, blocking until done (called before language-switch reboot) |
| `bsp_set_screen_timeout(sec)` | Auto screen-off timeout, 0=never; backlight off on timeout, wake on touch |
| `bsp_time_sync_from_host(host,port)` | **No need to implement**: `bsp_wifi_esp32.c` provides a board-agnostic implementation |

`bsp_conf.h` (`bsp_conf_read/write` config persistence) is implemented by the shared `bsp_conf_littlefs.c` — nothing to do either.

## 2. Implementation steps

### 2.1 Pick the closest neighbour as a template

| Your board type | Copy which BSP |
|---|---|
| ESP32 + SPI display + XPT2046 resistive touch | `bsp_cyd_2432s028r.c` (dedicated touch bus) or `bsp_e32r35t.c` (touch shares the LCD bus) |
| ESP32-S3 + RGB parallel display + I2C capacitive touch | `bsp_jc8048w550.c` + `rgb44.c` |

Create `src/bsp/esp32/bsp_<board>.c`, wrapped entirely in `#if CONFIG_BOARD_<BOARD>` (the macro comes from Kconfig).

### 2.2 What a BSP must handle

1. **Pin definitions**: as macros at the top of the file (this project keeps pins out of sdkconfig).
2. **Display driver**: prefer managed components from the Espressif registry (already used: `esp_lcd_ili9341`, `esp_lcd_st7796`, `esp_lcd_touch_xpt2046`, `esp_lcd_touch_gt911`). A new chip needs a dependency in `src/ports/esp32/entry/idf_component.yml` and its component name in the REQUIRES list of `src/bsp/CMakeLists.txt`.
3. **LVGL buffer model**:
   - SPI displays: internal-RAM double buffers (`MALLOC_CAP_DMA`, tens of lines high) + `LV_DISPLAY_RENDER_MODE_PARTIAL`
   - RGB parallel displays: PSRAM dual framebuffers + `LV_DISPLAY_RENDER_MODE_DIRECT` + vsync page flip (see rgb44)
4. **flush_cb byte order**: SPI panels expect the high byte first; LVGL memory is little-endian RGB565 → swap bytes in place during flush (do **not** use `LV_COLOR_FORMAT_RGB565_SWAPPED` — measured to produce snow/noise).
5. **Async DMA**: `esp_lcd_panel_draw_bitmap` is asynchronous; wait for `on_color_trans_done` before reusing/freeing the pixel buffer (see the semaphore pattern in the existing BSPs).
6. **Touch coordinates**: for XPT2046 take raw 12-bit ADC values (`x_max/y_max = 4096`), skip driver-level mirroring, and map to screen coordinates with a two-point linear calibration persisted to LittleFS (`touch.json`); capacitive panels like GT911 use driver coordinates directly.
7. **Backlight & screen-off**: LEDC PWM + a periodic `screen_off_check` (copy from the template).

### 2.3 Register the board (build plumbing)

Follow the checklist in the [contribution guide](contributing-board.md) for Kconfig / CMakeLists / sdkconfig.defaults / build script. Local build:

```bash
bash tools/build-esp32.sh <board>                # build (set-target runs automatically on first build)
bash tools/build-esp32.sh <board> flash COM6     # build + flash
bash tools/build-esp32.sh <board> menuconfig     # tweak config
```

!!! warning "The sdkconfig pitfall"
    Editing `sdkconfig.defaults.<board>` has **no effect** on an already-generated `sdkconfig.<board>` — change both; note that `# CONFIG_XXX is not set` lines in sdkconfig override defaults.

### 2.4 UI adaptation

- The logical resolution comes from `lv_display_create(w, h)`; the UI scales automatically by `scr_h / 240`, no panel code changes needed.
- `src/ui/ui_layout.c` fixes the font tier per board at preprocessing time (dead-strips unused full-table CJK fonts to save flash): small screens (320×240/480×320) → `UI_FONT_BIG 0`, large screens (800×480) → `1`.
- Touch inaccurate or scrambled: run the two-point calibration first (delete `touch.json` in LittleFS and reboot), only then consider driver-level swap/mirror.

## 3. Desktop simulator

The UI code runs directly on a PC (no flashing):

```bash
bash tools/build-desktop.sh
```

The result is an SDL2 window app; the `KLIPPER_RES=800x480` environment variable switches resolution. Use it for panel layout work and screenshot verification — the BSP layer has a no-op implementation in `src/ports/desktop/`.
