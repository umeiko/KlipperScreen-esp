# Porting to your own board (beginner's tutorial)

This tutorial is for someone connecting an ESP32 display for the first time. The goal is to prove one layer at a time and finish with a board port that another person can understand and maintain.

Start from `templates/board/`. Its display section is a **common SPI + ST7789 example**, not a universal display implementation. A board using ILI9341, ST7796, I80 parallel, RGB parallel, QSPI, or MIPI needs the matching display path described below.

![Porting on a breadboard: the main UI running on a ZJY-1.54IPS 240×240 display](screenshots/porting_breadboard.png)

The port now has seven stages, each with a clear pass condition:

1. Set up the toolchain and build an existing board.
2. Identify the hardware and classify the display interface.
3. Create and register a clean board scaffold.
4. **Bring up only the display** with a stable solid-colour test.
5. Connect the proven display transport to the BSP and LVGL, then add input.
6. Build and verify the release firmware.
7. Diagnose failures by stage and prepare a contribution.

!!! warning "Solve one layer at a time"
    Do not add touch, WiFi, Moonraker, and the full UI during display bring-up. A lit backlight does not prove that the LCD works; the backlight LED and LCD pixels are usually separate circuits.

---

## Step 0: Set up the environment

### 0.1 Install ESP-IDF v5.5.5

This project is pinned to ESP-IDF v5.5.5.

- Windows: install v5.5.5 with Espressif's installer. The usual locations are `C:\esp\v5.5.5\esp-idf` and `C:\Espressif\tools`.
- Linux/macOS: use the `install.sh` included with that ESP-IDF version.

The repository's `tools/idf.ps1`, `tools/idf-env.bat`, and build scripts activate the environment. You do not need to modify PATH permanently.

### 0.2 Clone the project and verify the toolchain

```bash
git clone https://github.com/umeiko/KlipperScreen-esp.git
cd KlipperScreen-esp
bash tools/build-esp32.sh cyd_2432s028r
```

The build must end with `Project build complete`, and the application must fit inside `Smallest app partition`. The first build downloads dependencies and is much slower than later incremental builds.

**Pass condition**: an existing board builds before you change any source file. Fix the environment first if it does not.

---

## Step 1: Collect complete hardware information

### 1.1 Make a board fact sheet

Fill this table from the schematic, vendor example, flex-cable markings, and controller data sheet. A product title that only gives the screen size is not enough.

| Item | What to record | Example |
|---|---|---|
| ESP chip | Exact model | ESP32, ESP32-S3, ESP32-P4 |
| Flash / PSRAM | Size, mode, frequency | 16MB Flash, 8MB OPI PSRAM |
| Display controller or timing IC | Full part number | ILI9341, ST7789, ST7796, ST7262 |
| **Pixel interface** | SPI, I80, RGB, QSPI, MIPI DSI | SPI |
| Native resolution | Width × height before rotation | 240×320 |
| Display pins | Every signal name and GPIO | SCLK, MOSI, CS, DC, RST |
| Display parameters | SPI mode/frequency or RGB timing | 40MHz, mode 0 |
| Backlight | GPIO, active level, PWM support | GPIO21, active high |
| Optional touch | Controller, interface, shared bus | XPT2046, separate SPI |
| Optional rotary | A, B, push-button GPIOs | GPIO4/5/6 |

A useful source order is: **a stable example for the exact board > schematic and panel data sheet > code for the same controller > product page > guesses**.

### 1.2 Classify the interface by its pin names

"Parallel LCD" is not one implementation. I80 and RGB both have many data wires, but they work very differently.

| Typical pins | Interface | How the image reaches the panel | Starting point in this project |
|---|---|---|---|
| `SCLK/MOSI/CS/DC/RST` | SPI command panel | MCU sends commands and pixels; panel GRAM retains the image | Board template, CYD, E32R35T |
| `D0..D7/15 + WR/RD/CS/DC` | I80/8080 command parallel | Like SPI, but transfers 8/16 bits in parallel | ESP-IDF I80 example + matching panel driver |
| `R0..B4 + PCLK/HSYNC/VSYNC/DE` | RGB/DOTCLK parallel | MCU continuously streams complete frames | ESP-IDF RGB panel example; JC8048 only as a special case |
| `CLK + D0..D3 + CS` | QSPI | Four data lines carry controller-specific commands/pixels | Matching controller driver or vendor example |
| `D0P/D0N, CLKP/CLKN` | MIPI DSI | High-speed differential link | ESP-IDF DSI example on a chip with DSI support |

!!! tip "Controller and interface are separate facts"
    The same ST7789 controller can be wired through SPI or I80. Select the bus code from the pins that the board actually exposes, not from the controller name alone.

### 1.3 Check the memory budget

RGB565 uses two bytes per pixel:

```text
frame bytes = width × height × 2
320 × 240  = 153,600 bytes
480 × 320  = 307,200 bytes
800 × 480  = 768,000 bytes
```

SPI and I80 command panels usually need only a 20–40-line DMA buffer. RGB panels usually need at least one complete framebuffer; double buffering doubles that memory. A large RGB display generally needs PSRAM, and the display DMA may also compete with the CPU for PSRAM bandwidth.

**Pass condition**: you can name the interface type and have every display pin and timing value on the fact sheet. Do not copy a BSP while the interface is still unknown.

---

## Step 2: Create and register the board scaffold

Copy the clean files first:

```bash
cp templates/board/bsp_board_template.c src/bsp/esp32/bsp_myboard.c
cp templates/board/sdkconfig.defaults.board_template src/ports/esp32/sdkconfig.defaults.myboard
```

Replace `BOARD_TEMPLATE`, `board_template`, and every `TODO(board)`. At this stage the display block may still be the example ST7789 transport; Step 3 replaces it before hardware testing.

Update these locations:

| File | Change |
|---|---|
| `src/bsp/Kconfig.projbuild` | Add `CONFIG_BOARD_MYBOARD` to the board choice |
| `src/bsp/CMakeLists.txt` | Add `bsp_myboard.c` and new display/touch component dependencies |
| `src/ports/esp32/entry/idf_component.yml` | Add any new registry driver dependency |
| `src/ports/esp32/sdkconfig.defaults.myboard` | Chip, Flash, PSRAM, board, and rotary defaults |
| `src/ui/ui_layout.c` | Select the small/large font tier for the resolution |
| `tools/build-esp32.sh` | Add target, build directory, and sdkconfig name |

Start board defaults from the template. Copy only chip/Flash/PSRAM settings from a board with the same ESP chip. Do not copy another board's display GPIOs or touch configuration.

!!! warning "The sdkconfig trap"
    A first build creates a complete `sdkconfig.myboard`. Later edits to `sdkconfig.defaults.myboard` do not update it. Either change both files or deliberately regenerate the old config. A `# CONFIG_XXX is not set` line can also override defaults.

---

## Step 3: Bring up only the display

This chapter has one goal: keep stable red, green, blue, white, and black bands on the panel. Work only in the display-transport parts of the board file created in Step 2; LVGL and input come later.

### 3.1 Separate the backlight from the pixels

- Backlight on, solid white panel: this usually proves only that the backlight has power. The display controller may not be initialized.
- Backlight off, normal serial logs: check the backlight GPIO, active level, and supply first. Pixels may already be changing but remain invisible.
- Backlight on, stable colour bands: the bus, initialization, and basic pixel transfer are working.

Keep the backlight at 100% during bring-up. Add brightness and screen-off after the display passes.

### 3.2 Find a known-good minimal reference

Prefer a vendor example for the exact board. Build and flash it unchanged, verify that it is stable, then extract:

- display interface and pins;
- reset, backlight, and display-enable levels;
- SPI mode/frequency, or RGB PCLK and porch/pulse timing;
- controller initialization commands;
- RGB/BGR, inversion, rotation, and visible-area offset;
- whether buffers live in internal RAM or PSRAM.

If the vendor example also fails, solve the wiring, supply, or documentation error before involving this project.

### 3.3 Every interface follows the same bring-up order

1. Configure supply, backlight, and reset GPIOs.
2. Initialize the pixel bus.
3. Create the panel or timing driver.
4. Reset the panel.
5. Send its initialization sequence.
6. Enable display output.
7. Send the five-colour test.
8. Leave it still for at least 30 seconds and watch for flicker, drift, and tearing.

Only steps 2, 3, 5, and 7 change substantially between display types.

### 3.4 Path A: SPI command panel

This path covers ILI9341, ST7789, ST7796, and similar controllers wired through SPI. The board template implements this path.

#### What to change first

1. Replace the example pins with schematic values. A missing MISO is normal for a write-only display.
2. Begin at 10–20MHz. Raise the clock only after a stable test.
3. Copy `spi_mode` from a proven example instead of guessing through four modes.
4. Replace `esp_lcd_new_panel_st7789()` with the factory for the real controller.
5. Register a new driver in `idf_component.yml` and the BSP CMake dependencies when it is not already present.
6. Apply only the required `swap_xy`, `mirror`, `invert_color`, and `set_gap` settings.

The bus, panel IO, and controller driver are three separate layers:

```text
GPIO/SPI host
    └─ esp_lcd_new_panel_io_spi()     decides how bytes are sent
          └─ esp_lcd_new_panel_xxx()  decides which init commands are sent
                └─ draw_bitmap()      writes a pixel rectangle into panel GRAM
```

Changing controller often means more than changing one include and one factory call. Initialization commands, pixel format, visible-area offsets, and sleep/wake commands can differ.

#### LVGL mode for an SPI panel

Start with `LV_DISPLAY_RENDER_MODE_PARTIAL` and two 20–40-line buffers allocated with `MALLOC_CAP_DMA`. The flush callback sends the dirty `area` through `esp_lcd_panel_draw_bitmap()`.

The DMA transfer is asynchronous. Call `lv_display_flush_ready()` only after the transfer has finished and LVGL may safely reuse the buffer. The template waits with `on_color_done` and a semaphore.

#### Common SPI-only failures

- Solid white: wrong CS/DC/RST, wrong controller driver, or no initialization commands.
- Whole image shifted or clipped: use `esp_lcd_panel_set_gap()`.
- Red and blue exchanged: toggle RGB/BGR.
- Photo-negative colours: toggle `esp_lcd_panel_invert_color()`.
- Random colour noise: lower the SPI clock, then verify RGB565 byte order.
- First frame correct, animation corrupt: the DMA buffer was freed or rewritten before transfer completion.

### 3.5 Path B: I80/8080 command parallel

An I80 panel exposes `WR`, `RD`, `CS`, `DC`, and 8 or 16 data lines. Although it is parallel, its model is still close to an SPI command panel: panel GRAM retains pixels after the MCU writes a rectangle.

Compared with SPI, the main replacements are:

```text
esp_lcd_new_i80_bus()
esp_lcd_new_panel_io_i80()
```

The matching `esp_lcd_new_panel_xxx()`, `esp_lcd_panel_draw_bitmap()`, transfer-done callback, and LVGL PARTIAL-buffer design can usually remain. Data width, `WR` clock, data-line order, and maximum transfer size must come from the schematic or a proven example.

!!! warning "I80 is not RGB"
    I80 has `WR/DC/CS`; RGB has `PCLK/HSYNC/VSYNC/DE`. Their drivers, buffers, and timing are not interchangeable.

### 3.6 Path C: RGB/DOTCLK parallel

An RGB panel exposes colour data lines plus `PCLK/HSYNC/VSYNC/DE`. The panel scans the incoming stream continuously. It generally cannot retain a static image in GRAM like an SPI command panel.

#### Parameters that must come from a reliable source

- GPIO for every R/G/B data bit, in the correct order;
- PCLK frequency and active edge;
- HSYNC/VSYNC pulse width, back porch, and front porch;
- DE use and active level;
- resolution and total line/frame timing;
- framebuffer location/count and PSRAM configuration.

Start from ESP-IDF v5.5.5 `examples/peripherals/lcd/rgb_panel` or a vendor example for the exact board. An ordinary RGB board should try the official `esp_lcd_new_rgb_panel()` first. Do not begin by copying the JC8048W550 `rgb44` path.

#### LVGL mode for an RGB panel

RGB output normally scans a complete framebuffer:

| Design | Memory | Behaviour |
|---|---:|---|
| One full framebuffer | 1 frame | Saves memory, but writing while scanning may tear |
| Two full framebuffers | 2 frames | Renders off-screen and swaps at VSYNC for a steadier image |
| Bounce buffers | Full frame + small internal buffers | Can address some PSRAM/DMA limits, but is parameter-sensitive |

The JC8048W550 uses `rgb44` with LVGL DIRECT double buffering. This is a measured workaround for that ESP32-S3/800×480 board. It also requires VSYNC swapping, waiting for the physical swap, and correct cache writeback when framebuffers in PSRAM are managed outside the standard driver. See the [JC8048W550 RGB display guide](jc8048w550-rgb-display-guide.md).

Consider that special transport only when all three are true:

1. The vendor's minimal example is stable.
2. The official RGB panel minimal example shows a repeatable underrun, offset, or tear with the same hardware timing.
3. Measurements place the failure in the transfer model instead of PCLK, timing, pins, or UI workload.

#### Common RGB-only failures

- Panel cycles through built-in test colours: PCLK or sync timing is not accepted.
- Whole image rolls or shifts periodically: total line/frame timing is wrong.
- Colour channels are scrambled: R/G/B data-line order or width is wrong.
- Static image stable, scrolling jitters: PSRAM bandwidth, DMA underrun, or cache coherency.
- Small updates stay stale while large updates sometimes appear: inspect cache writeback and page-swap synchronization for a self-managed framebuffer.

Change one RGB parameter per experiment and record the vendor and current values side by side.

### 3.7 Path D: QSPI, MIPI, or an unknown interface

QSPI is not ordinary SPI with three extra MOSI wires. MIPI DSI cannot use RGB timing code. Confirm that the ESP chip supports the interface, then start from an example for the same ESP-IDF version or from the controller vendor driver.

This repository does not currently provide a drop-in QSPI or MIPI board template. The BSP contract, UI, input, and configuration layers remain reusable, but the display transport is a new adapter. A contribution should include the data sheet, a known-good minimal example, and a colour-band result so the protocol is documented rather than guessed again.

### 3.8 Use one five-colour acceptance test

Implement `bsp_lcd_push(x, y, w, h, pixels)` first. Temporarily call this test after panel/framebuffer initialization and before `lv_init()`. It uses only a 20-line buffer:

```c
static void display_smoke_test(void)
{
    static const uint16_t colors[] = {
        0xF800,  /* red   */
        0x07E0,  /* green */
        0x001F,  /* blue  */
        0xFFFF,  /* white */
        0x0000,  /* black */
    };
    const int lines = 20;
    uint16_t *buf = heap_caps_malloc(LCD_H_RES * lines * 2, MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(buf ? ESP_OK : ESP_ERR_NO_MEM);

    for (int band = 0; band < 5; band++) {
        int y1 = band * LCD_V_RES / 5;
        int y2 = (band + 1) * LCD_V_RES / 5;
        for (int y = y1; y < y2; y += lines) {
            int h = y + lines <= y2 ? lines : y2 - y;
            for (int i = 0; i < LCD_H_RES * h; i++) buf[i] = colors[band];
            bsp_lcd_push(0, y, LCD_H_RES, h, buf);
        }
    }
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
```

`bsp_lcd_push()` absorbs the transport differences. SPI/I80 writes panel GRAM and waits for DMA. RGB writes the test pixels into the active test framebuffer and ensures that the scanner can see them.

Check all of the following:

- all five bands have the correct order and colour;
- bands cover the full visible area without a fixed offset;
- orientation matches the product mounting;
- no flicker, roll, or random lines for 30 seconds;
- ten consecutive reboots all bring the panel up.

Save a photo and serial log, remove the infinite loop, and continue.

### 3.9 Record the proven display baseline

At the top of the BSP, record the source of the parameters, native resolution, interface, stable clock/timing, colour order, orientation, and every non-default initialization command. This becomes the baseline for future regressions.

**Pass condition**: the panel shows stable colour bands without starting LVGL. Do not continue before it passes.

---

## Step 4: Connect the display to the BSP and LVGL

### 4.1 Understand what stays and what changes

Continue with the board file created in Step 2. Do not clone an existing BSP wholesale; it can contain another board's touch calibration, timing, memory policy, and hardware workaround.

The template contains two kinds of code:

- **Common lifecycle**: locking, storage, restart, LVGL task, and screen-activity contract. Usually keep this.
- **Display transport**: bus, panel, `bsp_lcd_push`, `flush_cb`, buffers, and render mode. Replace this with the design proven in Step 3.

For SPI/ST7789, modify the template item by item. For another SPI controller, replace the panel driver and initialization differences. For I80 or RGB, remove the SPI transport and add the matching implementation; do not retain both paths.

### 4.2 Display boundary required by upper layers

| BSP API | Upper-layer use | Board guarantee |
|---|---|---|
| `bsp_lcd_push()` | Boot animation before LVGL | Input buffer is safe to reuse when the call returns |
| `bsp_get_display()` | UI queries the default display | Returns the display created by `lv_display_create()` |
| `bsp_set_brightness()` | Settings and wake-up | Accepts 0–100 and handles backlight polarity |
| `bsp_fade_out()` | Fade before restart | At least turns the backlight off safely; clear a frame when practical |
| `bsp_disp_can_*()` | Shows optional invert/rotate settings | Returns false when hardware/transport cannot implement the feature |

The return-time guarantee of `bsp_lcd_push()` matters because the boot animation reuses its pixel memory. Returning while asynchronous DMA is still reading it causes bands and random blocks.

### 4.3 Select the LVGL buffer mode

| Display transport | Recommended starting point | Buffer location | Flush responsibility |
|---|---|---|---|
| SPI command panel | PARTIAL, two 20–40-line buffers | Internal DMA RAM | Send dirty rectangle, wait, then `flush_ready` |
| I80 command panel | PARTIAL, two local buffers | DMA-accessible memory | Same model as SPI on a different bus |
| RGB parallel | Full-frame design recommended by driver | Often PSRAM | Maintain continuous scan, cache, and swap synchronization |
| Self-managed double-frame RGB | DIRECT, two full frames | Chip-dependent | Request swap only on final flush; signal ready after physical swap |

Do not select FULL or DIRECT because the name sounds faster. The LVGL render mode must match the physical transport and buffer ownership.

### 4.4 Restore the complete UI in three passes

1. **Basic LVGL test**: create only the display, a solid background, and one rectangle.
2. **Project boot animation**: confirm repeated `bsp_lcd_push()` updates are stable.
3. **Full `ui_app_create()`**: test page transitions, list scrolling, and large repaints.

If pass 1 works and pass 2 fails, inspect `bsp_lcd_push` buffer lifetime. If the first two work and only full-UI scrolling fails, inspect refresh throughput, DMA/PSRAM bandwidth, and page swapping.

### 4.5 Common BSP functions

These functions normally do not depend on the display controller and can retain the template implementation:

- `bsp_lvgl_lock()` / `bsp_lvgl_unlock()` protect LVGL;
- `bsp_delay_ms()` / `bsp_restart()` provide delay and restart;
- NVS and LittleFS initialization store network, Moonraker, and UI settings;
- `lvgl_task()` runs `lv_timer_handler()` and the screen-off check;
- `bsp_screen_activity()` is the shared activity/wake entry point for every input.

Change backlight PWM for the actual active level. A board with a dedicated backlight IC may not use LEDC GPIO at all; it only needs to preserve the `bsp_set_brightness(0..100)` behaviour.

**Pass condition**: the boot animation and complete UI remain stable during continuous page changes and scrolling.

---

## Step 5: Add input last

Do not debug touch coordinates before the display passes Step 4.

### 5.1 Choose the product input shape

- **Touch only**: the BSP creates a pointer; rotary Kconfig stays disabled.
- **Touch + rotary**: the BSP creates a pointer and the shared input layer creates an encoder. Both work together.
- **Rotary only**: the BSP creates no dummy pointer and enables only rotary Kconfig.

### 5.2 Resistive touch

XPT2046-style resistive controllers return raw ADC coordinates and need calibration. Use `touch_cal_load()` / `touch_cal_run()` and store the result in LittleFS `touch.json`. A new board without measured factory values should enter calibration when the file is absent.

Touch may share the display SPI bus or use a separate bus. Confirm this from the schematic. Do not initialize the same SPI host twice on a shared-bus board.

### 5.3 Capacitive touch

Capacitive controllers such as GT911, CST816S, and FT5x06 normally return screen coordinates. Apply only the swap/mirror required by mounting, then report them to LVGL. Do not copy resistive `touch.json` or two-point calibration.

#### 5.3.1 A complete CST816S example to copy

The template contains two complete files:

- `templates/board/touch_input_board_template.h`: the small interface visible to the BSP.
- `templates/board/touch_input_board_template.c`: the concrete I2C, CST816S, screen-wake, and LVGL pointer implementation.

The BSP calls only this function. It does not need to know CST816S registers or LVGL read details:

```c
esp_err_t board_template_touch_input_create(
    lv_display_t *display,
    i2c_master_bus_handle_t i2c_bus,
    const board_template_touch_input_config_t *config);
```

The function performs four steps:

1. Create the touch I2C IO with `ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG()`.
2. Create the controller with `esp_lcd_touch_new_i2c_cst816s()`.
3. Pass coordinates and press/release state to LVGL in `touch_read_cb()`.
4. Call `bsp_screen_activity()` and swallow the first touch when it only wakes the screen.

CST816S has one unusual constraint: it responds to I2C for a short time after a touch event. The example therefore uses the INT pin and a semaphore and reads only after an interrupt. Do not change it to unconditional I2C reads on every LVGL poll. This behavior and the option for chips that fail while reading their ID are documented by the [Espressif CST816S driver](https://github.com/espressif/esp-bsp/tree/master/components/lcd_touch/esp_lcd_touch_cst816s).

To connect the example to a new board:

1. Copy both `touch_input_board_template` files into `src/bsp/esp32/`. Replace their filenames and every `board_template` token with the board name.
2. Add the new `.c` file to `SRCS` in `src/bsp/CMakeLists.txt`.
3. Add the driver to `src/ports/esp32/entry/idf_component.yml`:

    ```yaml
    espressif/esp_lcd_touch_cst816s: "^1.1.0"
    ```

4. Set `BOARD_HAS_CST816S_TOUCH` to `1` in the board BSP and fill SDA, SCL, RST, and INT. Reuse an existing I2C bus handle when the board already has one; do not create the same port twice.
5. Begin with `swap_xy`, `mirror_x`, and `mirror_y` all `false`. Show a four-corner test page and change only the flags that the physical mounting requires.

If a product page says **CST816T**, first verify the chip marking and data sheet. The example uses the **CST816S** driver explicitly supported by Espressif; similar names do not prove compatible registers or interrupt behavior. For another controller in the `esp_lcd_touch` family, you usually replace only the include, `ESP_LCD_TOUCH_IO_*_CONFIG()`, and `esp_lcd_touch_new_*()` call. The `touch_read_cb()` bridge to LVGL can stay.

If initialization stops while reading the chip ID, confirm the controller and then try this board sdkconfig option:

```ini
CONFIG_ESP_LCD_TOUCH_CST816S_DISABLE_READ_ID=y
```

### 5.4 Rotary encoder

The shared `bsp_input_init()` creates the encoder from Kconfig. Do not add GPIO interrupt, PCNT, or focus traversal code to the board BSP.

```ini
CONFIG_INPUT_ROTARY_ENCODER=y
CONFIG_INPUT_ROTARY_GPIO_A=4
CONFIG_INPUT_ROTARY_GPIO_B=5
CONFIG_INPUT_ROTARY_GPIO_BUTTON=6
CONFIG_INPUT_ROTARY_COUNTS_PER_DETENT=4
CONFIG_INPUT_ROTARY_PHASE_PULLUPS=y
CONFIG_INPUT_ROTARY_BUTTON_ACTIVE_LOW=y
```

A bare encoder's common pin normally goes to GND. Connect A/B and the button to their configured GPIOs. Disable phase pull-ups when a module already has external ones. Enable `CONFIG_INPUT_ROTARY_REVERSE` if direction is backwards. Reduce counts from 4 to 2 or 1 if one physical detent skips multiple controls.

Rotary-only products get dedicated interactions: the Moonraker host uses four 0–255 octets, and temperature changes directly on its card. Arbitrary hostname or API-key text should be factory-provisioned, entered through a maintenance interface, or entered with touch; common same-LAN deployments may also use Moonraker `trusted_clients`.

### 5.5 Screen-off wake

Both touch and rotary call `bsp_screen_activity()`. Its return value tells the driver that this action only woke the screen, so the first tap/turn is swallowed instead of activating a control.

**Pass condition**: every declared input traverses pages, controls modals, and wakes from screen-off. Capacitive builds never show calibration, and rotary-only builds create no pointer.

---

## Step 6: Build, flash, and verify by layer

```bash
bash tools/build-esp32.sh myboard
bash tools/build-esp32.sh myboard flash COM6
```

Verify in this order instead of stopping when the main screen appears:

1. **Build**: the binary fits the application partition.
2. **Repeated boot**: ten boots all light the panel; no intermittent white screen.
3. **Static image**: no random line, flash, or colour jump for one minute.
4. **Display load**: rapid page changes and list scrolling remain stable.
5. **Input**: touch, rotary, or both match the declared hardware.
6. **Network**: configure Moonraker, receive state, and issue one low-risk control.
7. **Screen-off**: timeout turns it off and the first input only wakes it.

Build at least one existing board afterward to verify that shared UI and BSP interfaces were not broken.

---

## Step 7: Diagnose by symptom

### 7.1 Display quick table

| Symptom | Check first |
|---|---|
| Backlight off | Backlight supply, GPIO, active level, PWM |
| Backlight on but solid white/black | Reset, CS/DC, panel driver, initialization sequence |
| Unstructured noise | Wiring, supply, excessive clock, byte order |
| Fixed image offset | Native resolution and `set_gap` |
| Red/blue exchanged | RGB/BGR or RGB data-line order |
| Photo-negative colour | Invert setting |
| Mirrored or rotated 90° | swap_xy and mirror; use a direction-marked test image |
| First frame correct, animation corrupt | DMA buffer reused before transfer completion |
| Whole RGB image rolls | PCLK, HSYNC/VSYNC, porch timing |
| RGB static image stable, scrolling jitters | DMA underrun, PSRAM bandwidth, cache/swap synchronization |
| Panel factory function missing at build | Driver dependency and header are not registered |

### 7.2 Input quick table

| Symptom | Check first |
|---|---|
| Resistive touch coordinates scrambled | Raw-axis mapping and two-point calibration |
| Capacitive panel enters calibration | Resistive flow was copied by mistake |
| Touch completely dead | Controller, bus, CS/IRQ, shared-bus design |
| Rotary direction backwards | `CONFIG_INPUT_ROTARY_REVERSE` |
| One detent skips items | `COUNTS_PER_DETENT` |
| Wake also activates a control | Use the `bsp_screen_activity()` return value |

If a problem remains, return to the latest passing stage and do a controlled A/B. Include the board fact sheet, colour-band photo, serial log, known-good vendor example, and one-variable experiment notes when asking for help.

Read [Contributing a new board (PR guide)](contributing-board.md) before submitting the port.
