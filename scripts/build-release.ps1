# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
  [string]$Version = "0.5.1"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$projectDir = Join-Path $repoRoot "firmware\atom-echo"
$pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
$python = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
$esptool = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\esptool.py"
$framework = Join-Path $env:USERPROFILE ".platformio\packages\framework-arduinoespressif32"
$buildDir = Join-Path $projectDir ".pio\build\m5stack-atom"
$outputDir = Join-Path $repoRoot "dist"
$output = Join-Path $outputDir "CharaDock-ATOM-Echo-v$Version.bin"
$checksum = Join-Path $outputDir "SHA256SUMS.txt"

foreach ($required in @($pio, $python, $esptool, $framework)) {
  if (-not (Test-Path -LiteralPath $required)) {
    throw "Required PlatformIO component was not found: $required"
  }
}

& $pio run --project-dir $projectDir --target clean
if ($LASTEXITCODE -ne 0) { throw "PlatformIO clean failed with exit code $LASTEXITCODE." }

& $pio run --project-dir $projectDir
if ($LASTEXITCODE -ne 0) { throw "PlatformIO build failed with exit code $LASTEXITCODE." }

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
& $python $esptool --chip esp32 merge_bin `
  --flash_mode dio --flash_freq 40m --flash_size 4MB `
  -o $output `
  0x1000 (Join-Path $buildDir "bootloader.bin") `
  0x8000 (Join-Path $buildDir "partitions.bin") `
  0xe000 (Join-Path $framework "tools\partitions\boot_app0.bin") `
  0x10000 (Join-Path $buildDir "firmware.bin")
if ($LASTEXITCODE -ne 0) { throw "Firmware merge failed with exit code $LASTEXITCODE." }

$binaryText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($output))
$privateRoots = @(
  [Environment]::GetFolderPath([Environment+SpecialFolder]::UserProfile),
  $repoRoot
) | Where-Object { $_ } | ForEach-Object { $_.Replace("\", "/").TrimEnd("/") } | Select-Object -Unique
foreach ($privateRoot in $privateRoots) {
  if ($binaryText.Contains($privateRoot) -or $binaryText.Contains($privateRoot.Replace("/", "\"))) {
    throw "Release image contains a private build path. Check the compiler prefix-map configuration."
  }
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $output).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(
  $checksum,
  "$hash  $(Split-Path -Leaf $output)`n",
  [Text.UTF8Encoding]::new($false)
)
Write-Host "Created $output"
Write-Host "Created $checksum"
