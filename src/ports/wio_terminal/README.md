# Seeed Wio Terminal port

The Wio Terminal uses an ATSAMD51P19A (about 496 KiB available program flash,
192 KiB RAM), an onboard 320x240 ILI9341 LCD, a five-way switch, and a 4 MiB
W25Q32 QSPI flash.

This port deliberately keeps executable code in the SAMD51 internal flash and
uses fixed QSPI sectors for settings, leaving the rest for future font / asset
packs. Avoiding a full FAT stack preserves the limited internal program flash.
The onboard RTL8720DN is driven through Seeed's rpcWiFi stack.

ASCII and icons use no-kerning copies of LVGL's Montserrat fonts. Small
12/14/16px extension fonts add Latin-1 and common UI punctuation. Chinese uses
a three-tier compact chain: every CJK UI literal plus the 512 most frequent
simplified characters at 12px/1bpp, ranks 513-1024 at 10px/1bpp, then the
remaining common characters at 8px/1bpp. Mechanical/electronics/3D-printing
vocabulary is promoted to at least 10px by demoting the same number of
non-domain characters from the tail of the middle tier.
Regenerate all Wio fonts with `python tools/fontgen/gen_wio_fonts.py`.

Before using WiFi, update the RTL8720DN wireless core with Seeed's
`ambd_flash_tool` and the current `seeed-ambd-firmware` release. Old wireless
firmware can stop replying to eRPC calls. This port runs association in a
worker task and bounds each eRPC reply wait to 15 seconds, so a missing reply
cannot freeze LVGL or the physical controls.

Build and upload:

```powershell
platformio run -d src/ports/wio_terminal -e wio-terminal
platformio run -d src/ports/wio_terminal -e wio-terminal -t upload --upload-port COM3
python tools/make_uf2.py `
  src/ports/wio_terminal/.pio/build/wio-terminal/firmware.bin `
  src/ports/wio_terminal/.pio/build/wio-terminal/firmware.uf2
```

Controls: five-way switch = directional navigation + OK; top key C = Back.
The onboard backlight pin is digital-only; TC4 provides 2kHz pulse-density
dimming without adding work to the LVGL loop.

Bring-up, wireless-core firmware, build/upload and USB recovery notes are kept
in `docs/boards/wio-terminal.md` and `docs/boards/wio-terminal.zh.md`.
