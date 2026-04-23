$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sketchSource = Join-Path $projectRoot 'M5Dial_WLED_Remote.ino'
$sketchDir = Join-Path $projectRoot '.arduino-tmp\M5Dial_WLED_Remote'
$sketchTarget = Join-Path $sketchDir 'M5Dial_WLED_Remote.ino'
$buildPath = Join-Path $sketchDir 'build\esp32.esp32.m5stack_dial'
$outputDir = Join-Path $projectRoot '.arduino-build'
$firmwareDir = Join-Path $projectRoot 'firmware'
$firmwareBin = Join-Path $firmwareDir 'M5Dial_WLED_Remote.bin'
$firmwareMergedBin = Join-Path $firmwareDir 'M5Dial_WLED_Remote.merged.bin'
$arduinoCli = 'C:\Program Files\Arduino CLI\arduino-cli.exe'
$fqbn = 'esp32:esp32:m5stack_dial:PartitionScheme=min_spiffs'

if (-not (Test-Path $arduinoCli)) {
  throw "Arduino CLI not found at $arduinoCli"
}

New-Item -ItemType Directory -Force -Path $sketchDir | Out-Null
New-Item -ItemType Directory -Force -Path $buildPath | Out-Null
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
New-Item -ItemType Directory -Force -Path $firmwareDir | Out-Null

# Compile from a sketch folder whose name matches the .ino filename.
Copy-Item -LiteralPath $sketchSource -Destination $sketchTarget -Force

& $arduinoCli compile --fqbn $fqbn --build-path $buildPath $sketchDir

if ($LASTEXITCODE -ne 0) {
  throw "Arduino CLI compile failed with exit code $LASTEXITCODE"
}

$artifacts = @(
  'M5Dial_WLED_Remote.ino.bin',
  'M5Dial_WLED_Remote.ino.bootloader.bin',
  'M5Dial_WLED_Remote.ino.partitions.bin',
  'M5Dial_WLED_Remote.ino.merged.bin',
  'M5Dial_WLED_Remote.ino.elf',
  'M5Dial_WLED_Remote.ino.map'
)

foreach ($artifact in $artifacts) {
  $source = Join-Path $buildPath $artifact
  if (Test-Path $source) {
    Copy-Item -LiteralPath $source -Destination (Join-Path $outputDir $artifact) -Force
  }
}

Copy-Item -LiteralPath (Join-Path $buildPath 'M5Dial_WLED_Remote.ino.bin') -Destination $firmwareBin -Force
Copy-Item -LiteralPath (Join-Path $buildPath 'M5Dial_WLED_Remote.ino.merged.bin') -Destination $firmwareMergedBin -Force

Write-Host "Firmware exported to $firmwareBin"
Write-Host "Full flash image exported to $firmwareMergedBin"
