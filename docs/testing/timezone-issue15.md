# Issue #15: Guition timezone acceptance build

This package targets **Guition ESP32-4848S040C_I / _I_Y_1 / _I_Y_3**,
board target **`esp32s3-4848s040`**, as confirmed in issues
[#12](https://github.com/umeiko/KlipperScreen-esp/issues/12) and
[#15](https://github.com/umeiko/KlipperScreen-esp/issues/15).
It is a test build awaiting hardware acceptance.

## Download and flash

In the PR's **ESP32 timezone acceptance** Actions run, download the artifact
**ESP-IDFv5.5-esp32s3-4848s040-issue15** while signed into GitHub. Extract the
entire archive into a folder. `BUILD.txt` identifies the source commit;
`SHA256SUMS` contains checksums for the images and flash tools.

Connect the board's USB-C/CH340 port, then run `flash.bat` on Windows or
`bash flash.sh /dev/ttyUSB0` on Linux/macOS (requires esptool 5+; replace the
serial port with your actual device). Flash all three included images.
There is no need to erase flash or compensate by changing the Pi's clock.
The scripts do not erase the configuration/storage partition.

## Acceptance steps

1. Open **Settings → Display → Time zone**, select **CET/CEST**, and return to
   the home screen. The clock should update without restarting the display.
   For the reported example, UTC 12:21 should display **13:21 CET**, rather
   than 20:21. In summer, it should use UTC+2 automatically.
2. Leave the home screen visible for at least a minute. The clock should
   continue advancing normally and agree with the Pi/phone's Europe/Berlin time.
3. Restart or power-cycle the display. Confirm **CET/CEST** remains selected
   and the clock is still correct after reconnecting to Moonraker.
4. Reconnect Wi-Fi/Moonraker once more. The next HTTP time synchronization
   must not reset the display to UTC+8. Also check a file's modification time
   in its details page, which uses the same display timezone.
5. Confirm the existing display, touch and printer connection still work.

Please report the result in issue #15 or the acceptance PR. If something is
wrong, include the selected timezone, `BUILD.txt`, display/local times taken
at the same instant, and a photo. A `--:--` clock before the first successful
HTTP time synchronization is expected; please mention if it remains that way.

Existing devices retain UTC+8 until a timezone is explicitly selected. The
display's timezone is independent of the Pi/Fluidd setting. Automated tests
cover the Europe/Berlin spring/fall DST changes; these still need no manual
seasonal offset adjustment on the display.
