param(
  [string]$Port = 'COM8',
  [int]$Baud = 921600
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$esptool = Join-Path $env:LOCALAPPDATA 'Arduino15\packages\esp32\tools\esptool_py\5.2.0\esptool.exe'
$mergedBin = Join-Path $projectRoot '.arduino-build\M5Dial_WLED_Remote.ino.merged.bin'

if (-not (Test-Path $esptool)) {
  throw "esptool not found at $esptool"
}

if (-not (Test-Path $mergedBin)) {
  throw "Merged firmware image not found at $mergedBin"
}

Write-Host "Erasing flash on $Port..."
& $esptool --chip esp32s3 --port $Port erase_flash
if ($LASTEXITCODE -ne 0) {
  throw "Flash erase failed with exit code $LASTEXITCODE"
}

Write-Host "Writing merged firmware image to $Port..."
& $esptool --chip esp32s3 --port $Port --baud $Baud write_flash 0x0 $mergedBin
if ($LASTEXITCODE -ne 0) {
  throw "Flash write failed with exit code $LASTEXITCODE"
}

Write-Host "Recovery flash complete."
