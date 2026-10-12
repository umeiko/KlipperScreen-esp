# Small host check against real ESP-IDF headers; no device or flashing needed.
param(
    [string]$IdfPath = 'C:/esp/v5.5.5/esp-idf',
    [string]$SdkconfigDir = 'src/ports/esp32/build-ec11-knob-esp32-st7789/config'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$cc = Join-Path $repo 'tools/msys64/ucrt64/bin/gcc.exe'
$output = Join-Path $repo 'tmp/lcd-color-io-test.exe'
New-Item -ItemType Directory -Force (Split-Path $output) | Out-Null
$includes = @('src/ports/desktop', 'src/bsp', 'src/bsp/esp32', 'third_party/lvgl', $SdkconfigDir) |
    ForEach-Object { '-I' + (Join-Path $repo $_) }
$idfIncludes = @('esp_lcd/include', 'esp_lcd/interface', 'esp_common/include', 'hal/include', 'soc/include', 'soc/esp32/include') |
    ForEach-Object { '-I' + (Join-Path $IdfPath ('components/' + $_)) }
$oldPath = $env:PATH
try {
$env:PATH = (Split-Path $cc) + ';' + $env:PATH
& $cc -std=c99 -Wall -Wextra -DLV_CONF_INCLUDE_SIMPLE @includes @idfIncludes `
    (Join-Path $repo 'tests/test_lcd_color_io.c') `
    (Join-Path $repo 'src/bsp/esp32/bsp_lcd_color_io.c') `
    (Join-Path $repo 'src/bsp/bsp_display_color.c') -o $output
if ($LASTEXITCODE) { throw 'LCD color IO test build failed' }
& $output
if ($LASTEXITCODE) { throw 'LCD color IO test failed' }
} finally { $env:PATH = $oldPath }
