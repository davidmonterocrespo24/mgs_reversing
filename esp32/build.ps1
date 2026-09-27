# Build the Metal Gear Solid firmware for the ESP32-S3.
# Same environment the SOTN port uses (IDF 5.5.4 + its Xtensa toolchain).
$ErrorActionPreference = "Continue"   # cmake writes info: lines to stderr

$TOOLS   = "C:\Espressif5.5"
$PY_VENV = "C:\Espressif\python_env\idf4.4_py3.10_env"
$env:IDF_PATH            = "$TOOLS\frameworks\esp-idf-v5.5.4"
$env:IDF_TOOLS_PATH      = $TOOLS
$env:IDF_TARGET          = "esp32s3"
$env:IDF_PYTHON_ENV_PATH = $PY_VENV
$env:VIRTUAL_ENV         = $PY_VENV
$env:MSYSTEM             = $null     # IDF 5.x refuses to run under MSys
$env:ESP_ROM_ELF_DIR = (Get-ChildItem "$TOOLS\tools\esp-rom-elfs\*" -Directory |
                        Select-Object -First 1).FullName

$xt = (Get-ChildItem "$TOOLS\tools\xtensa-esp-elf\*\xtensa-esp-elf\bin" | Select-Object -First 1).FullName
$cm = (Get-ChildItem "$TOOLS\tools\cmake\*\bin"   | Select-Object -First 1).FullName
$nj = (Get-ChildItem "$TOOLS\tools\ninja\*" -Directory | Select-Object -First 1).FullName
$cc = (Get-ChildItem "$TOOLS\tools\ccache\*\*" -Directory -ErrorAction SilentlyContinue |
       Select-Object -First 1).FullName
$env:PATH = "$PY_VENV\Scripts;$xt;$cm;$nj;$cc;" + $env:PATH

$proj = $PSScriptRoot

# Two boards, two build trees. They differ in flash size, partition table and
# panel controller, so they cannot share a build directory -- swapping between
# them in one tree leaves stale CMake cache entries that fail in confusing
# ways. Separate directories make switching free.
#   build.ps1            -> Waveshare ESP32-S3-Touch-LCD-2 (16 MB, ST7789)
#   build.ps1 xiao       -> XIAO ESP32S3 Sense handheld    (8 MB,  ILI9341)
#   build.ps1 xiao clean -> ...from scratch
if ($args -contains "xiao") {
    $build   = Join-Path $proj "build-xiao"
    $sdkcfg  = "$proj\sdkconfig.xiao"
    $extra   = "-DMGS_BOARD=XIAO"
    Write-Host "== board: XIAO ESP32S3 Sense (ILI9341 320x240, 8 MB) =="
} else {
    $build   = Join-Path $proj "build"
    $sdkcfg  = "$proj\sdkconfig.defaults"
    $extra   = "-DMGS_BOARD=WAVESHARE"
    Write-Host "== board: Waveshare ESP32-S3-Touch-LCD-2 (ST7789, 16 MB) =="
}

if ($args -contains "clean") { Remove-Item -Recurse -Force $build -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $build | Out-Null

# SDKCONFIG must be per-board too, not just SDKCONFIG_DEFAULTS. IDF generates
# `sdkconfig` in the project root on first build and from then on the DEFAULTS
# file is ignored -- so the second board silently inherits the first board's
# flash size and partition table. That is how an 8 MB build came out claiming a
# 6 MB app partition. Giving each board its own generated config inside its own
# build directory removes the shared state entirely.
cmake -G Ninja -Wno-dev "-DIDF_TARGET=esp32s3" "-DCMAKE_BUILD_TYPE=Release" `
      $extra "-DSDKCONFIG_DEFAULTS=$sdkcfg" "-DSDKCONFIG=$build\sdkconfig" `
      -S $proj -B $build
ninja -C $build
