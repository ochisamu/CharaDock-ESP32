# SPDX-License-Identifier: Apache-2.0
param(
  [Parameter(Mandatory = $true)]
  [ValidatePattern('^COM[0-9]+$')]
  [string]$Port,
  [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$projectDir = Join-Path $repoRoot 'firmware\waveshare-rlcd-4.2'
$buildDir = Join-Path $projectDir '.pio\build\waveshare-rlcd-42'
$platformIo = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\platformio.exe'
$python = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
$bootApp = Join-Path $env:USERPROFILE '.platformio\packages-charadock-pioarduino3\framework-arduinoespressif32\tools\partitions\boot_app0.bin'

foreach ($required in @($platformIo, $python)) {
  if (-not (Test-Path -LiteralPath $required)) {
    throw "PlatformIO is not installed at $required"
  }
}

$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
if (-not $SkipBuild) {
  & $platformIo run --project-dir $projectDir
  if ($LASTEXITCODE -ne 0) { throw 'RLCD 4.2 build failed.' }
}

$bootloader = Join-Path $buildDir 'bootloader.bin'
$partitions = Join-Path $buildDir 'partitions.bin'
$application = Join-Path $buildDir 'firmware.bin'
foreach ($required in @($bootloader, $partitions, $bootApp, $application)) {
  if (-not (Test-Path -LiteralPath $required)) {
    throw "Required firmware image is missing: $required"
  }
}
if ((Get-Item -LiteralPath $application).Length -gt 0x600000) {
  throw 'RLCD 4.2 application exceeds its 6 MiB partition.'
}

# Write discrete boot/application segments instead of firmware.factory.bin.
# The gap at 0x9000-0xdfff is the device NVS partition and is intentionally
# untouched so Wi-Fi credentials and the host pairing secret survive updates.
& $python -X utf8 -m esptool --chip esp32s3 --port $Port `
  write-flash --no-progress --flash-mode dio --flash-freq 80m --flash-size detect `
  0x0000 $bootloader `
  0x8000 $partitions `
  0xe000 $bootApp `
  0x10000 $application
if ($LASTEXITCODE -ne 0) { throw 'RLCD 4.2 flash failed.' }

Write-Host "RLCD 4.2 update verified on $Port; NVS was preserved."
