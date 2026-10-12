# Requires a built Windows simulator with -DKR_ENCODER_SETTINGS_PREVIEW=ON.
# Link a single smoke translation unit against existing simulator objects.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$build = Join-Path $repo 'src/ports/desktop/build'
$runtime = Join-Path $repo 'tools/msys64/ucrt64'
$cc = Join-Path $runtime 'bin/gcc.exe'
$testDir = Join-Path $repo ('tmp/encoder-smoke-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDir | Out-Null
$oldPath = $env:PATH
$oldConfig = $env:KLIPPER_CONFIG_DIR
$oldVideo = $env:SDL_VIDEODRIVER
try {
    $env:PATH = "$runtime/bin;$env:PATH"
    $objects = Get-ChildItem (Join-Path $build 'CMakeFiles/klipper_remote_simulator.dir') -Recurse -Filter '*.obj' |
        Where-Object { $_.Name -notin @('main.c.obj', 'panel_display.c.obj') } |
        Select-Object -ExpandProperty FullName
    $includes = @('src/ports/desktop', 'src/bsp', 'src/core', 'src/ui', 'third_party/lvgl') |
        ForEach-Object { '-I' + (Join-Path $repo $_) }
    & $cc -std=c99 -DLV_CONF_INCLUDE_SIMPLE -DKLIPPER_DESKTOP_SIMULATOR=1 -DKR_ENCODER_SETTINGS_PREVIEW=1 -DKR_DISPLAY_SETTINGS_PREVIEW=1 @includes "-I$runtime/include/SDL2" `
        (Join-Path $repo 'tests/test_encoder_settings.c') @objects `
        "$build/lvgl/lib/liblvgl.a" "$build/lvgl/lib/liblvgl_thorvg.a" `
        "-L$runtime/lib" -lmingw32 -lSDL2main -lSDL2 -lcjson -lwinhttp -lcrypt32 -lstdc++ -lm `
        -o "$testDir/encoder-smoke.exe"
    if ($LASTEXITCODE) { throw 'Smoke build failed' }
    $env:KLIPPER_CONFIG_DIR = $testDir
    $env:SDL_VIDEODRIVER = 'dummy'
    Push-Location $testDir
    try {
        & "$testDir/encoder-smoke.exe"
        if ($LASTEXITCODE) { throw "Smoke failed: $LASTEXITCODE" }
    } finally { Pop-Location }
    Write-Output "Snapshots: $testDir"
} finally {
    $env:PATH = $oldPath
    $env:KLIPPER_CONFIG_DIR = $oldConfig
    $env:SDL_VIDEODRIVER = $oldVideo
}
