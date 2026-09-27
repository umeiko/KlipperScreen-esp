# Issue #8: bounded file-list browsing (v0.5.8-wip.3)

This is a **pre-release fix candidate**, not a claim that every affected device
has been verified. [Download v0.5.8-wip.3](https://github.com/umeiko/KlipperScreen-esp/releases/tag/v0.5.8-wip.3).
The normal `latest` download links follow the newest stable release. Use the
tagged page above when reproducing this exact v0.5.8-wip.3 test candidate.

## What the new report establishes

The supplied log explicitly identifies `v0.5.8-wip.2`. Available heap falls from
74 KB to 57 KB, 37 KB and 18 KB. It then reports **StoreProhibited** with
`EXCVADDR=0x000000f8`, not just a watchdog timeout. The log identifies the regular
`2432S028R` BSP, although the supplied download screenshot points to the PLUS
package. Neither observation justifies dismissing the report as an old-version test.

The previous change only batched widget creation and capped visible rows. It
still allocated the whole file list and retained too many widgets for a small
ESP32 heap. A host test reproduced a stall with a 50 KB additional UI-memory
budget. This explains a failure path, but the supplied PC addresses cannot be
mapped reliably without that exact build's ELF; a different build's ELF is not
valid evidence for the precise crashing source line.

## Changes

- ESP32 reads the file list on a separate HTTP worker using 512-byte chunks,
  without the full WebSocket receive buffer, full JSON tree and serialized copies.
- A bounded parser consumes one record at a time and keeps only eight selected
  records. The UI reuses eight rows and has previous/next/refresh buttons.
- Every page is fetched again; the whole directory listing is not cached.
  Files may move between pages if the server listing changes during browsing.
- The worker never calls LVGL or allocates an async LVGL timer. The UI polls
  completed results, cancels on exit, and ignores stale replies. Timeout no
  longer requires new printer status notifications.
- Low-memory admission checks leave headroom for the UI/network. Errors use
  an already-created status label. Filename marquee timers are removed.
- Pre-release CI retains the matching per-board ELF as Actions artifacts for
  30 days, so future backtraces can be decoded against the correct binary.

Limits: 8 MiB per response, 1 KiB per file record, bounded nesting, about 20 s
worker deadline and a 25 s UI timeout. Oversized, malformed or incomplete
responses fail without creating more rows. Paths longer than the existing
95-byte print-path limit are skipped (page indicator shows `*`), never silently
truncated into the name of a different file. Organizing such filenames or a
listing beyond these limits is still necessary. The server API has no assumed
pagination parameter; this is client-side page selection during streaming.

## Retest

1. Download from the pre-release page, not a `latest` link. Select
   `ESP-IDFv5.5-cyd_2432s028r.zip` for the regular board or
   `ESP-IDFv5.5-cyd_2432s028r_plus.zip` for the PLUS ST7789 variant, according to
   the actual hardware.
2. Confirm **App version: v0.5.8-wip.3** in the boot log and the correct `BSP ready`
   board name. Both are needed; a screenshot of a download choice is not proof
   of which binary is running.
3. On the affected printer with many files, open the list, browse several pages,
   return and reopen repeatedly. Check that navigation remains responsive and
   filenames still open the correct details. Do not start a print just to test browsing.
4. Interrupt the connection during a fetch; the screen should remain navigable
   and offer a refresh after failure. Reconnect and retry.
5. Report the version, board, file count, `files: page offset=...` diagnostic
   lines and any crash/backtrace. Remove Wi-Fi passwords/API keys first.

Source checks: `powershell -File tools/test-file-list.ps1` after a desktop build.
It checks fragmented parsing, 1,000/10,000 records, memory allocation failure,
malformed/oversized responses, real LVGL pagination and lifecycle behavior.
Host memory measurements are not ESP32 heap measurements. A successful CI
build alone is not hardware acceptance; issue #8 stays open until the affected
device passes this retest.
