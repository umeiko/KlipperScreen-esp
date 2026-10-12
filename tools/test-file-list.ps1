# Build desktop once before running. No device or real printer required.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$build = Join-Path $repo 'src/ports/desktop/build'
$runtime = Join-Path $repo 'tools/msys64/ucrt64'
$testDir = Join-Path $repo ('tmp/files-smoke-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDir | Out-Null
$oldPath = $env:PATH; $oldConfig = $env:KLIPPER_CONFIG_DIR; $oldVideo = $env:SDL_VIDEODRIVER
try {
    $env:PATH = "$runtime/bin;$env:PATH"
    $includes = @('src/ports/desktop','src/bsp','src/core','src/ui','third_party/lvgl') |
        ForEach-Object { '-I' + (Join-Path $repo $_) }
    & "$runtime/bin/gcc.exe" -std=c99 -DLV_CONF_INCLUDE_SIMPLE @includes "-I$runtime/include/cjson" `
        "$repo/tests/test_file_list_stream.c" "$repo/src/core/file_list_stream.c" "-L$runtime/lib" -lcjson -o "$testDir/stream.exe"
    if ($LASTEXITCODE) { throw 'Stream test build failed' }
    & "$testDir/stream.exe"
    if ($LASTEXITCODE) { throw 'Stream test failed' }
    # Use Ninja's actual link inputs, not stale .obj files left by older configurations.
    $link = (Select-String -Path "$build/build.ninja" -Pattern '^build KlipperScreen-esp.exe:').Line
    $objects = [regex]::Matches($link, 'CMakeFiles/\S+\.obj') | ForEach-Object { $_.Value } |
        Where-Object { (Split-Path $_ -Leaf) -notin @('main.c.obj','printer_model.c.obj','panel_files.c.obj') } |
        ForEach-Object { Join-Path $build $_ }
    & "$runtime/bin/gcc.exe" -std=c99 -DLV_CONF_INCLUDE_SIMPLE @includes "-I$runtime/include/cjson" "-I$runtime/include/SDL2" `
        "$repo/tests/test_files_ui.c" @objects "$build/lvgl/lib/liblvgl.a" "$build/lvgl/lib/liblvgl_thorvg.a" `
        '-Wl,--wrap=lv_malloc_core,--wrap=lv_free_core,--wrap=lv_realloc_core' `
        "-L$runtime/lib" -lmingw32 -lSDL2main -lSDL2 -lcjson -lwinhttp -lcrypt32 -lssl -lcrypto -lws2_32 -lstdc++ -lm -o "$testDir/ui.exe"
    if ($LASTEXITCODE) { throw 'UI test build failed' }
    $env:KLIPPER_CONFIG_DIR = $testDir; $env:SDL_VIDEODRIVER = 'dummy'
    & "$testDir/ui.exe"
    if ($LASTEXITCODE) { throw 'UI test failed' }
    Write-Output "Tests: $testDir"
} finally {
    $env:PATH = $oldPath; $env:KLIPPER_CONFIG_DIR = $oldConfig; $env:SDL_VIDEODRIVER = $oldVideo
}
