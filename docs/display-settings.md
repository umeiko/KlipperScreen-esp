# Display and encoder settings

Available from **v0.5.8**, under **Settings → Display settings**. Options appear
only when the firmware's input/display backend supports them; the normal
Windows/macOS desktop applications do not expose these hardware adjustments.

## Encoder step: 1 / 2 / 4

This number means **quadrature counts per physical detent**, not detents per
revolution or a model number. It appears only in ESP32 builds with a rotary encoder.

- If two clicks move one menu item, try reducing the value, for example **4 → 2**.
- If one click skips several items, try increasing the value.
- The menu contains only **1, 2, 4** and displays the current number. Before your
  first change, the board's compiled default is used.

Selecting a number starts a **20-second trial**. Rotate the encoder to check
that each detent moves one item, then choose **Confirm** to save. **Restore** is
focused initially. Restore, Back, timeout or leaving the page restores the previous
setting. Restarting before confirmation also keeps the previously saved setting.

This adjustment does not repair noisy contacts, wiring faults or lost pulses.

## Screen color order: Default / RGB / BGR

Open **Screen color order** to enter its own page. Choose one of three list rows:
**Default**, **RGB**, **BGR**. Moving the focus does not change the setting; press
to select. The current row is marked, and selection takes effect immediately
and is saved across restarts. No trial countdown is needed because navigation
continues working even with the wrong colors.

Use the three labeled swatches to check the screen: **red / green / blue**.
Their labels follow the selected UI language. If red and blue are exchanged,
try the other order. Green should not change. **Default** restores the board's
original color order; it is not universally the same as RGB.

Color order is independent of inversion and RGB565 byte order. Rotation and
horizontal mirror retain your selected color order. Do not use this option to
diagnose every kind of color distortion.

The SPI LCD backends for ST7735S, ST7789, ILI9341, ST7796 and ILI9488 are connected,
including the JLC-SZP custom SPI path. JC8048W550's RGB parallel display keeps its
existing stable path and does not expose this setting. See [supported boards](boards.md).

## Validation boundary

The Windows interaction was accepted before release. Automated checks cover
trial/rollback, settings persistence, translations, RGB/BGR behavior and MADCTL
orientation preservation. Builds check software integration, not the wiring or
appearance of every physical panel. Check the three swatches and orientation on
your actual module after flashing.
