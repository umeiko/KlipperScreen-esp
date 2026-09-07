# Porting to your own board (beginner's tutorial)

This tutorial assumes **little to no ESP32 experience**. Goal: walk you through porting this firmware to a new board, step by step, until you have a flashable binary. We use the **CYD 2432S028R** (the classic cheap 2.8" resistive-touch board) and its BSP file `src/bsp/esp32/bsp_cyd_2432s028r.c` as the reference implementation, explaining one function at a time.

The whole process is five steps:

1. Set up the development environment (ESP-IDF)
2. Clone the repo and build an existing board to verify the environment
3. Gather your board's hardware information
4. Write the BSP file (the main body of this tutorial, function by function)
5. Register the board + build the artifact + troubleshoot

---

## Step 0: Set up the environment

### 0.1 Install ESP-IDF v5.5.5

ESP-IDF is Espressif's official framework. **The version must be 5.5.5** (this project is locked to it).

- **Windows**: download the [ESP-IDF online installer](https://dl.espressif.com/dl/esp-idf/) and pick v5.5.5. Afterwards you should have:
    - IDF sources: `C:\esp\v5.5.5\esp-idf`
    - Toolchains: `C:\Espressif\tools`
- **Linux/macOS**: follow the [official guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32/get-started/) using `install.sh`.

The project ships environment wrapper scripts (`tools/idf.ps1` / `tools/idf-env.bat`); the build script calls them automatically, so you do **not** need to touch PATH yourself.

### 0.2 Clone the repo

```bash
git clone https://github.com/umeiko/KlipperScreen-esp.git
cd KlipperScreen-esp
```

### 0.3 Build an existing board first to verify the environment

```bash
bash tools/build-esp32.sh cyd_2432s028r
```

The first build takes 3–10 minutes (it downloads dependency components). This output at the end means your environment is fine:

```
Project build complete. To flash, run:
 idf.py flash
...
klipper_remote_display.bin binary size 0x2d55a0 bytes. Smallest app partition is 0x320000 bytes. ...
```

Artifacts land in `src/ports/esp32/build/`: `bootloader/bootloader.bin`, `partition_table/partition-table.bin`, `klipper_remote_display.bin`.

If you happen to own a CYD, flash it to verify the whole chain:

```bash
bash tools/build-esp32.sh cyd_2432s028r flash COM6   # replace COM6 with your port
```

!!! tip "Finding the serial port"
    Windows: Device Manager → Ports (COM & LPT); Linux: usually `/dev/ttyUSB0`.

---

## Step 1: Gather your board's hardware info

Before writing any code, confirm every item below from the **vendor wiki / schematic** (don't start with anything missing):

| What to find | Example (CYD) | Where to look |
|---|---|---|
| MCU model | ESP32 (dual-core 240MHz) | Board silkscreen / product page |
| Flash size | 4MB | Product page / `esptool flash_id` |
| Display driver IC | ILI9341 | Vendor wiki / flex-cable silkscreen |
| Display interface | SPI | Same |
| Display resolution | 240×320 (native portrait) | Same |
| Touch IC | XPT2046 (resistive) | Same |
| Touch interface | SPI (shared or dedicated bus?) | Pin table |
| All pins | SCLK=14, MOSI=13, ... | Vendor pin allocation table |
| Backlight pin + active level | GPIO21, active high | Schematic |

Usual sources: vendor wikis (e.g. lcdwiki), schematic PDFs, and ready-made TFT_eSPI `User_Setup.h` files (they are literally pin definitions).

!!! warning "SPI displays: pay attention"
    Whether the touch panel **shares** the SPI pins with the display or has a **dedicated** bus decides how many SPI buses your BSP initializes. CYD uses a dedicated bus (sharing measured to return all-zero MISO); the E32R35T shares — this project has working examples of both.

---

## Step 2: Write the BSP file (function-by-function tutorial)

### 2.0 What a BSP is

All hardware-specific code in this project is isolated in a layer called the BSP (Board Support Package). The upper-level UI only calls `bsp_xxx()` functions and knows nothing about your display or touch chip.

So porting = writing **one C file** implementing the functions declared in `src/bsp/bsp.h`. Open it — the 11 functions there are the checklist we will now implement one by one.

### 2.1 Create the file skeleton

Copying the reference implementation is fastest:

```bash
cp src/bsp/esp32/bsp_cyd_2432s028r.c src/bsp/esp32/bsp_myboard.c
```

The first and last lines of the file are the **board switch** — you must change them:

```c
#include "sdkconfig.h"
#if CONFIG_BOARD_MYBOARD        // ← change to your board macro

// ... the whole implementation ...

#endif /* CONFIG_BOARD_MYBOARD */
```

**Why the switch**: the build system compiles **every** board's BSP file together (CMake component registration happens before Kconfig loads, so sources cannot be filtered per board at the CMake level). Each file wraps itself in `#if`, and only the selected board compiles to real code. Macro naming: `CONFIG_BOARD_` + uppercased board name.

### 2.2 Pin definitions

Macros at the top of the file; fill in the pin table from Step 1. CYD looks like:

```c
#define PIN_LCD_SCLK   14    // SPI clock
#define PIN_LCD_MOSI   13    // SPI master out
#define PIN_LCD_MISO   12    // SPI master in
#define PIN_LCD_CS     15    // display chip select
#define PIN_LCD_DC     2     // data/command select
#define PIN_LCD_RST    4     // display reset (-1 if none)
#define PIN_LCD_BL     21    // backlight

#define PIN_TP_SCLK    25    // touch SPI (only for a dedicated bus)
#define PIN_TP_MOSI    32
#define PIN_TP_MISO    39
#define PIN_TOUCH_CS   33    // touch chip select
#define PIN_TOUCH_IRQ  36    // touch interrupt

#define LCD_H_RES      320   // logical width in landscape
#define LCD_V_RES      240   // logical height in landscape
#define LCD_SPI_HZ     (40 * 1000 * 1000)  // display SPI speed
#define DRAW_BUF_LINES 40    // LVGL draw buffer lines
```

Notes:

- `LCD_H_RES/V_RES` are the **landscape** logical resolution (CYD is natively 240×320 portrait; landscape is 320×240).
- `DRAW_BUF_LINES` is how many lines LVGL renders before pushing to the panel. Buffer size = `H_RES × lines × 2 bytes`; CYD uses 320×40×2 = 25KB, times two buffers. Internal RAM on ESP32 is tight — stay between 20 and 40 lines.
- If RST is tied to ESP32's EN (like on the E32R35T), use `-1` and the driver falls back to a software reset.

### 2.3 `bsp_lvgl_lock` / `bsp_lvgl_unlock` — the LVGL thread lock

**Purpose**: LVGL is not thread-safe. This project runs LVGL in its own task; any other task (network callbacks, serial CLI) must take the lock before touching the UI.

```c
static SemaphoreHandle_t lvgl_mux;

void bsp_lvgl_lock(void)   { xSemaphoreTakeRecursive(lvgl_mux, portMAX_DELAY); }
void bsp_lvgl_unlock(void) { xSemaphoreGiveRecursive(lvgl_mux); }
```

**Explanation**: just two wrappers around a FreeRTOS recursive mutex. `lvgl_mux` is created at the start of `bsp_init()`. **Copy verbatim, change nothing.**

### 2.4 `bsp_get_display` / `bsp_delay_ms` / `bsp_restart` — three one-liners

```c
lv_display_t *bsp_get_display(void) { return lv_display_get_default(); }

void bsp_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void bsp_restart(void)
{
    lv_refr_now(NULL);                 // paint the "restarting" toast first
    vTaskDelay(pdMS_TO_TICKS(800));    // give the user 800ms to see it
    esp_restart();
}
```

**Explanation**:

- `bsp_get_display`: returns the LVGL default display (the one created in `bsp_init`). Upper layers use it to query the resolution.
- `bsp_delay_ms`: plain delay for the boot animation.
- `bsp_restart`: switching languages rebuilds the whole UI, implemented as a plain reboot. `lv_refr_now` forces one frame first, otherwise the toast never reaches the screen.

**Copy all three verbatim.**

### 2.5 `bsp_set_brightness` — backlight brightness

**Purpose**: called by the brightness slider in Settings. Uses LEDC (hardware PWM) on the backlight pin.

```c
static uint8_t bl_duty = 255;
static int     bl_pct = 100;

void bsp_set_brightness(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct > 0 && pct < 5) pct = 5;     // floor: prevents getting stuck at a black screen
    bl_pct = pct;
    bl_duty = (uint8_t)(pct * 255 / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, bl_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
```

**Explanation**: converts a 0–100 percentage into an 8-bit PWM duty (0–255) for the LEDC channel. The LEDC timer/channel setup lives in `bsp_init` (see 2.11). **Copy verbatim**; if your backlight is **active low** (rare), invert the duty to `255 - bl_duty`.

### 2.6 Auto screen-off trio — `bsp_set_screen_timeout` + two internal helpers

**Purpose**: Settings can configure "turn the screen off after N idle seconds"; any touch wakes it.

```c
static uint32_t so_after_s;      // timeout in seconds, 0 = never
static bool     screen_off;
static int64_t  last_act_us;

void bsp_set_screen_timeout(uint32_t sec)
{
    so_after_s = sec;
    last_act_us = esp_timer_get_time();
    if (screen_off) {                    // setting changed while off → wake first
        screen_off = false;
        bsp_set_brightness(bl_pct);
    }
}

static void screen_activity(void)        // called by the touch callback: stamp + wake
{
    last_act_us = esp_timer_get_time();
    if (screen_off) {
        screen_off = false;
        bsp_set_brightness(bl_pct);
    }
}

static void screen_off_check(void)       // called periodically from the LVGL task
{
    if (screen_off || !so_after_s) return;
    if (esp_timer_get_time() - last_act_us > (int64_t)so_after_s * 1000000) {
        screen_off = true;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);   // screen off = PWM to zero
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
}
```

**Explanation**: a small state machine. `last_act_us` records the last touch; `screen_off_check` runs every 5ms. Screen-off only kills the backlight (LCD content stays), so wake is instant. **Copy verbatim.**

### 2.7 `bsp_fade_out` — graceful fade to black

**Purpose**: before the language-switch reboot, the backlight fades smoothly using hardware, then the whole screen is pushed black (otherwise the panel's GRAM keeps the old frame and flashes it briefly at next power-on).

```c
void bsp_fade_out(uint32_t ms)
{
    static bool fade_installed;
    if (!fade_installed) {
        ledc_fade_func_install(0);       // the LEDC fade feature needs its ISR installed once
        fade_installed = true;
    }
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0, ms);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);
    bl_duty = 0;

    static uint16_t black[LCD_H_RES * 40];    // static zero-init = all black
    for (int y = 0; y < LCD_V_RES; y += 40) {
        xSemaphoreTake(lcd_trans_done, 0);
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y + 40, black);
        xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(500));
    }
}
```

**Explanation**: the first half is the LEDC hardware fade; the second pushes 40-line black blocks until the screen is covered. The `lcd_trans_done` semaphore is explained next. **Copy verbatim** (the 40 here is just a chunk size, unrelated to `DRAW_BUF_LINES`).

### 2.8 `on_color_trans_done` + `bsp_lcd_push` — DMA pushing

**Purpose**: `bsp_lcd_push` is the low-level "push a raw RGB565 block to the screen" function, used by the boot animation (before LVGL is running).

**First understand the pitfall**: `esp_lcd_panel_draw_bitmap()` is **asynchronous DMA** — when it returns, the data is still being transferred. Freeing or overwriting the pixel buffer immediately means the DMA reads garbage, visible as stripe tearing. So you register a "transfer done" callback and wait on a semaphore:

```c
static SemaphoreHandle_t lcd_trans_done;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    LV_UNUSED(io); LV_UNUSED(edata); LV_UNUSED(user_ctx);
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(lcd_trans_done, &hp);   // DMA done, give the semaphore from ISR
    return hp == pdTRUE;
}

void bsp_lcd_push(int x, int y, int w, int h, const uint16_t *px)
{
    /* SPI panels want the pixel high byte first; memory is little-endian → copy+swap */
    size_t n = (size_t)w * h;
    uint16_t *tmp = malloc(n * 2);
    if (!tmp) return;
    for (size_t i = 0; i < n; i++) tmp[i] = (uint16_t)((px[i] >> 8) | (px[i] << 8));
    xSemaphoreTake(lcd_trans_done, 0);            // drain any stale signal
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, tmp);
    xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(500));  // wait for the DMA to finish
    free(tmp);
}
```

**Explanation**:

- Byte swap: RGB565 in memory is low-byte-first; SPI panels expect high-byte-first. Here we copy because `px` belongs to the caller and must not be modified.
- `xSemaphoreTake(lcd_trans_done, 0)` (zero timeout) "drains" the semaphore so a stale signal can't cause a false pass.
- The callback is registered in `bsp_init`.

**Copy verbatim**; RGB parallel displays (non-SPI) don't need the byte swap — see `bsp_jc8048w550.c`.

### 2.9 `flush_cb` — the bridge between LVGL and the panel (the most important function in the file)

**Purpose**: LVGL calls this every time it finishes rendering a region; your job is **to put those pixels on the screen**.

```c
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* swap byte order in place (do NOT use LV_COLOR_FORMAT_RGB565_SWAPPED — measured to produce noise) */
    uint16_t *p = (uint16_t *)px_map;
    int32_t n = (int32_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    for (int32_t i = 0; i < n; i++) p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
    lv_display_flush_ready(disp);    // tell LVGL "you may reuse this buffer"
}
```

**Explanation**:

- `area` is the dirty region, `px_map` the pixel data.
- Here the swap is **in place** (LVGL's buffer gets redrawn next round anyway), while `bsp_lcd_push` copies — the difference is who owns the buffer.
- `lv_display_flush_ready` is mandatory; LVGL's double buffering won't hand you the same buffer until its DMA has finished (handled inside esp_lcd via `on_color_trans_done`).

**Copy verbatim.** (Universal for SPI panels. RGB parallel displays use DIRECT mode — completely different, see rgb44.)

### 2.10 Touch — `tp_read_raw` / calibration / `touch_read_cb`

**Purpose**: turn touch-chip raw readings into screen coordinates. Resistive panels (XPT2046) differ per unit due to mounting and wiring, so this project uses a **two-point linear calibration** persisted to LittleFS as `touch.json`.

Core idea: **the driver layer performs no coordinate transformation at all** — it reads raw 12-bit ADC values (0–4095), and the calibration formula `screen = raw × slope + offset` absorbs every mirror/axis-swap.

This whole block (`tp_read_raw`, `touch_cal_save`, `touch_cal_load`, `cal_pump`, `cal_sample`, `touch_cal_run`) is about 160 lines — **copy it verbatim**; you only need to understand two things:

1. `tp_read_raw` swaps x/y:

    ```c
    *x = pt[0].y;   /* on the CYD in landscape the raw axes cross the screen axes;
        *y = pt[0].x;      after swapping, *x is always the horizontal (long) axis */
    ```

    You don't need to care **how** your panel's axes cross — the two-point calibration's signed slopes absorb any orientation. Copy as-is.

2. The behaviour of `touch_cal_load()` decides **whether first boot enters calibration**:
    - CYD ships factory defaults measured on real hardware → when the file is missing it writes the defaults and skips calibration.
    - Your new board has no factory values → make load `return false` when the file is missing, and `bsp_init` will automatically run the blocking calibration flow (copy the `bsp_e32r35t.c` version, where the end of `touch_cal_load` is `return false`).

`touch_read_cb` is LVGL's touch-read callback; besides the calibration mapping it has two small tricks:

```c
} else if (pressing && esp_timer_get_time() - last_valid_us < 50 * 1000) {
    rx = last_rx;   /* momentary sample loss mid-drag → bridge as still-pressed,
    ry = last_ry;      otherwise LVGL splits a drag into a series of taps */
}
```

And the tap that wakes a slept screen is "swallowed" (`wake_swallow`) so waking up doesn't also hit a button. **Copy verbatim.**

!!! note "Capacitive panels (GT911 etc.)"
    No calibration needed — the driver outputs screen coordinates directly and `touch_read_cb` is much simpler. See `bsp_jc8048w550.c`.

### 2.11 `bsp_init` — the init assembly line (the longest function, in 7 blocks)

This is the first function that runs after power-on. Following lines 401–532 of `bsp_cyd_2432s028r.c`, block by block:

**① Mutex + NVS + LittleFS**

```c
lvgl_mux = xSemaphoreCreateRecursiveMutex();

esp_err_t err = nvs_flash_init();
if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
}

esp_vfs_littlefs_conf_t fs_conf = {
    .base_path = "/littlefs",
    .partition_label = "storage",
    .format_if_mount_failed = true,
    .dont_mount = false,
};
ESP_ERROR_CHECK(esp_vfs_littlefs_register(&fs_conf));
```

Creates the mutex from 2.3; NVS is the key-value store the WiFi module uses; LittleFS mounts the `storage` partition (already defined in `partitions.csv`) at `/littlefs` to hold `touch.json`, auto-formatting on first boot. **Copy verbatim.**

**② Backlight LEDC setup**

```c
ledc_timer_config_t bl_timer = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .duty_resolution = LEDC_TIMER_8_BIT,
    .timer_num = LEDC_TIMER_0,
    .freq_hz = 5000,
    .clk_cfg = LEDC_AUTO_CLK,
};
ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
ledc_channel_config_t bl_ch = {
    .gpio_num = PIN_LCD_BL,                 // ← your backlight pin
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel = LEDC_CHANNEL_0,
    .timer_sel = LEDC_TIMER_0,
    .duty = 255,                            // full brightness at boot
    .hpoint = 0,
};
ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));
```

**Only the pin changes.** 8-bit / 5kHz works everywhere.

**③ SPI bus init (display)**

```c
spi_bus_config_t buscfg = {
    .sclk_io_num = PIN_LCD_SCLK,
    .mosi_io_num = PIN_LCD_MOSI,
    .miso_io_num = PIN_LCD_MISO,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * 2 + 8,  // max bytes per DMA transfer
};
ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
```

**Change the pins.** `max_transfer_sz` must be ≥ one LVGL buffer in bytes — copy the formula.

**④ Touch SPI bus (only for dedicated-bus boards)**

```c
spi_bus_config_t tp_buscfg = { ... PIN_TP_SCLK ... };
ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &tp_buscfg, SPI_DMA_DISABLED));
```

Touch is low-speed; no DMA needed. **If your touch shares the bus with the display, delete this block** (see `bsp_e32r35t.c`) and attach the touch to `SPI2_HOST` in block ⑥.

**⑤ Display: IO handle + driver + orientation**

```c
esp_lcd_panel_io_handle_t io_handle;
esp_lcd_panel_io_spi_config_t io_cfg = {
    .dc_gpio_num = PIN_LCD_DC,
    .cs_gpio_num = PIN_LCD_CS,
    .pclk_hz = LCD_SPI_HZ,
    .lcd_cmd_bits = 8,
    .lcd_param_bits = 8,
    .spi_mode = 0,
    .trans_queue_depth = 10,
};
ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io_handle));

/* transfer-done semaphore: bsp_lcd_push / fade_out wait on DMA with it */
lcd_trans_done = xSemaphoreCreateBinary();
esp_lcd_panel_io_callbacks_t io_cbs = { .on_color_trans_done = on_color_trans_done };
ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &io_cbs, NULL));

esp_lcd_panel_dev_config_t panel_cfg = {
    .reset_gpio_num = PIN_LCD_RST,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,   // red/blue swapped? change to RGB
    .bits_per_pixel = 16,
};
ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &panel_handle));  // ← your driver
ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
/* landscape: swap_xy rotates 90°, mirror fixes the mirroring direction */
ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, true));
ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
```

This is **the block you will most likely change**:

- Swap the driver function for your display IC: `esp_lcd_new_panel_ili9341` / `esp_lcd_new_panel_st7796` / … (dozens exist in the component registry — search [components.espressif.com](https://components.espressif.com)). A new driver also needs dependency registration, see Step 3.
- ST7796-class panels usually also need `esp_lcd_panel_invert_color(panel_handle, true)` (otherwise colours look like a photo negative).
- `swap_xy(true)` + `mirror(true, true)` decide the landscape orientation. If the image is flipped horizontally/vertically, adjust the two mirror arguments (4 combinations; one rebuild+flash per try).
- `rgb_ele_order`: red and blue swapped → toggle BGR ↔ RGB.

**⑥ Touch init**

```c
esp_lcd_panel_io_handle_t tp_io;
esp_lcd_panel_io_spi_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(PIN_TOUCH_CS);
ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &tp_io_cfg, &tp_io));
//                                            shared-bus boards use SPI2_HOST ↑

esp_lcd_touch_config_t tp_cfg = {
    .x_max = 4096,   // raw 12-bit ADC, no driver-level mapping (left to the calibration)
    .y_max = 4096,
    .rst_gpio_num = GPIO_NUM_NC,
    .int_gpio_num = PIN_TOUCH_IRQ,
    .levels = {.reset = 0, .interrupt = 0},
    .flags = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
};
ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &touch_handle));
```

**Change the bus and pins.** `x_max/y_max = 4096` with all three flags false is deliberate: raw values go into the calibration.

**⑦ LVGL init + task start**

```c
lv_init();
lv_tick_set_cb(tick_cb);          // tell LVGL how to read a millisecond clock

lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
size_t buf_sz = LCD_H_RES * DRAW_BUF_LINES * 2;
void *buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);   // DMA can read internal RAM
void *buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
ESP_ERROR_CHECK(buf1 && buf2 ? ESP_OK : ESP_ERR_NO_MEM);
lv_display_set_buffers(disp, buf1, buf2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
lv_display_set_flush_cb(disp, flush_cb);                 // hook up the bridge from 2.9

lv_indev_t *indev = lv_indev_create();
lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
lv_indev_set_read_cb(indev, touch_read_cb);              // hook up the touch from 2.10

/* no calibration data → run the two-point calibration (the LVGL task isn't
   running yet; the calibration pumps frames itself) */
if (!touch_cal_load()) {
    touch_cal_run();
}

xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);
```

Key points: double buffering (one buffer is being DMA'd while LVGL renders into the other); `MALLOC_CAP_DMA` is mandatory (SPI DMA cannot read PSRAM or the regular heap); `lvgl_task` is just a loop running `lv_timer_handler()` + `screen_off_check()` every 5ms (copy verbatim).

---

That's the whole BSP file. `bsp_time_sync_from_host` and config persistence (`bsp_conf_*`) have **shared board-agnostic implementations** — nothing for you to write.

## Step 3: Register the board (5 files, a few lines each)

| File | Purpose | What to add |
|---|---|---|
| `src/bsp/Kconfig.projbuild` | Makes the `CONFIG_BOARD_MYBOARD` macro exist | `config BOARD_MYBOARD` + one description line inside `choice BOARD` |
| `src/bsp/CMakeLists.txt` | Gets your new .c compiled | `"esp32/bsp_myboard.c"` in SRCS; the component name in REQUIRES if you use a new driver IC |
| `src/ports/esp32/entry/idf_component.yml` | Declares third-party driver deps | With a new driver IC add a line, e.g. `espressif/esp_lcd_st7796: "^1.4.0"` |
| `src/ports/esp32/sdkconfig.defaults.myboard` | Default config for the new board | New file: copy from a same-chip board, change `CONFIG_BOARD_XXX=y` to your macro |
| `tools/build-esp32.sh` | Teaches the build script the new board | A case in `board_conf()`: `TARGET=esp32; BDIR=build-myboard; SDKCFG=sdkconfig.myboard; DEFS="sdkconfig.defaults;sdkconfig.defaults.myboard"` |

Complete sdkconfig.defaults content (copy this for classic ESP32):

```
CONFIG_BOARD_MYBOARD=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y    # change if your flash isn't 4MB
CONFIG_ESP32_DEFAULT_CPU_FREQ_240=y
```

!!! warning "The sdkconfig pitfall"
    After the first build a full `sdkconfig.myboard` exists, and editing `sdkconfig.defaults.myboard` **has no effect** — change both files (search the same config line in sdkconfig; note that `# CONFIG_XXX is not set` overrides defaults).

Also add a font tier in `src/ui/ui_layout.c` (chooses big vs small fonts, dead-stripped at compile time to save flash):

```c
#elif defined(CONFIG_BOARD_MYBOARD)
#define UI_FONT_BIG 0    // 320×240 / 480×320 → 0; 800×480 → 1
```

## Step 4: Build, flash, verify

```bash
bash tools/build-esp32.sh myboard              # first run does set-target + full build
bash tools/build-esp32.sh myboard flash COM6   # build + flash
```

**Success looks like**: the build ends with `Project build complete` and the `binary size` fits the `Smallest app partition`. After flashing, the backlight turns on, the boot animation plays, and the main UI appears.

Serial logs (115200 8N1) show `BSP ready (...)` and touch-calibration loading — check the logs first when something's wrong.

## Step 5: Symptom cheat sheet

| Symptom | Cause | Where to fix |
|---|---|---|
| All white/black, backlight on | wrong driver IC / SPI mode | change `esp_lcd_new_panel_xxx` |
| Colours look like a negative | panel needs INVON | add/remove `esp_lcd_panel_invert_color(panel, true)` |
| Red and blue swapped | RGB/BGR order | toggle `rgb_ele_order` BGR ↔ RGB |
| Image flipped/mirrored | mounting orientation | the 4 combinations of `esp_lcd_panel_mirror` |
| Snow/noise in colours | misused `RGB565_SWAPPED` format | go back to the in-place byte-swap flush_cb |
| Touch totally dead | wrong bus / shared-vs-dedicated mix-up | re-check Step 1's pin table |
| Touch positions scrambled | not calibrated | delete `touch.json` and reboot into calibration (serial CLI `rm /littlefs/touch.json`) |
| Stripe tearing | DMA buffer reused too early | check the `on_color_trans_done` semaphore logic |
| Build error: `esp_lcd_new_panel_xxx` undeclared | dependency not registered | Step 3: idf_component.yml + CMakeLists |

Once it works and you want to contribute the board back: [Contributing a new board (PR guide)](contributing-board.md).
