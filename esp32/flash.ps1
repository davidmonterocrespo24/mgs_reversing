# Flash the MGS firmware.
#
#   flash.ps1 COM6 -Data path\to\mgs_data.bin   -> Waveshare board: app + FAT data
#                                                  partition (trimmed STAGE.DIR)
#   flash.ps1 COM6                              -> Waveshare board: app only
#   flash.ps1 COM9 -Xiao                        -> handheld: app only; its data
#                                                  lives on the microSD
param([string]$Port = "COM6", [string]$Data = "", [switch]$Xiao)

$PY = "C:\Espressif\python_env\idf4.4_py3.10_env\Scripts\python.exe"
$ESPTOOL = "C:\Espressif5.5\frameworks\esp-idf-v5.5.4\components\esptool_py\esptool\esptool.py"

if ($Xiao) {
    $B = "$PSScriptRoot\build-xiao"
    # partitions_xiao.csv has no data partition: 8 MB of flash, and the card
    # carries the full 71 MB STAGE.DIR anyway.
    $Data = ""
    Write-Host "== flashing the XIAO handheld (no data partition) =="
} else {
    $B = "$PSScriptRoot\build"
    Write-Host "== flashing the Waveshare board =="
}

if (-not (Test-Path "$B\mgs_esp32s3.bin")) {
    throw "no binary in $B - build first with build.ps1$(if ($Xiao) {' xiao'})"
}

$parts = @(
    "0x0", "$B\bootloader\bootloader.bin",
    "0x8000", "$B\partition_table\partition-table.bin",
    "0x10000", "$B\mgs_esp32s3.bin"
)
if ($Data) {
    $parts += @("0x610000", $Data)
}

& $PY $ESPTOOL --chip esp32s3 --port $Port --baud 921600 write_flash @parts
