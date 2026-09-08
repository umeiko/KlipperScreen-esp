# Board port template

Start every new board port from this directory. Existing BSP files contain
board-specific workarounds and are examples to consult, not bases to clone.

1. Read the hardware-classification and display-only bring-up chapters in
   [`docs/porting.md`](../../docs/porting.md), and identify SPI, I80, RGB,
   QSPI, or MIPI before editing code.
2. Copy `bsp_board_template.c` to `src/bsp/esp32/bsp_<board>.c`.
3. Copy `sdkconfig.defaults.board_template` to
   `src/ports/esp32/sdkconfig.defaults.<board>`.
4. For CST816S capacitive touch, also copy `touch_input_board_template.c/.h`.
   Replace their filenames and every `board_template` token with the board name.
   Use them as the adapter example for a different touch controller.
5. Replace every `BOARD_TEMPLATE`, `board_template`, and `TODO(board)` marker.
6. Register the board using the checklist in
   [`docs/contributing-board.md`](../../docs/contributing-board.md).
7. Build the new board, then build one existing board as a shared-code check.

The C template is specifically a small **SPI command-panel + ST7789** example.
Only a similar SPI display should modify it item by item. I80, RGB/DOTCLK,
QSPI, and MIPI require replacement of the whole display transport, including
the bus, buffers, and `flush_cb`; changing only the ST7789 panel factory does
not turn the SPI example into a parallel implementation. Keep the common BSP
lifecycle. Do not create a dummy pointer on a no-touch board.

Choose one hardware input shape: touch, touch + rotary, or rotary only. Only
resistive controllers such as XPT2046 need `touch.json` and calibration.
Capacitive controllers such as GT911 and CST816S normally report screen
coordinates directly and must not call the calibration flow.
`touch_input_board_template.c` is a complete CST816S implementation example with
interrupt-gated I2C reads, coordinate reporting, and screen wake. Verify the
data sheet for similarly named parts such as CST816T instead of assuming that
the CST816S driver is compatible.

Rotary encoders are shared optional inputs. Do not put EC11/encoder code in the
board BSP. Enable the `Optional input devices` Kconfig menu in the board's
sdkconfig defaults and provide A, B, and push-button GPIOs. It works alongside a
touch input without UI changes, and the same configuration is complete on a
rotary-only board.
