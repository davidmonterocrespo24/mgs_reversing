# Build the consola hardware test for the ESP32-S3.
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

$proj  = $PSScriptRoot
$build = Join-Path $proj "build"
if ($args -contains "clean") { Remove-Item -Recurse -Force $build -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $build | Out-Null

cmake -G Ninja -Wno-dev "-DIDF_TARGET=esp32s3" "-DCMAKE_BUILD_TYPE=Release" `
      "-DSDKCONFIG_DEFAULTS=$proj\sdkconfig.defaults" -S $proj -B $build
ninja -C $build
