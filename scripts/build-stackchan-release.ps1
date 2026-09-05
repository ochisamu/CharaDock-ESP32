# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
  [string]$Version = "0.1.0-preview"
)

$ErrorActionPreference = "Stop"
if ($Version -notmatch "^[0-9A-Za-z][0-9A-Za-z._-]{0,31}$") {
  throw "Version must contain only letters, digits, dot, underscore, or hyphen."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$projectDir = Join-Path $repoRoot "firmware\stackchan"
$pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
$python = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
$esptool = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\esptool.py"
$framework = Join-Path $env:USERPROFILE ".platformio\packages\framework-arduinoespressif32"
$buildDir = Join-Path $projectDir ".pio\build\m5stack-cores3"
$outputDir = Join-Path $repoRoot "dist\stackchan"
$output = Join-Path $outputDir "CharaDock-StackChan-K151-v$Version.bin"
$checksum = Join-Path $outputDir "SHA256SUMS.txt"

function Invoke-CheckedNative {
  param(
    [Parameter(Mandatory = $true)][string]$FilePath,
    [Parameter(Mandatory = $true)][string[]]$Arguments,
    [Parameter(Mandatory = $true)][string]$Description
  )
  $allArguments = @($FilePath) + $Arguments
  $quotedArguments = $allArguments | ForEach-Object {
    if ($_ -match '["\r\n]') {
      throw "Native argument contains an unsupported character: $_"
    }
    '"' + $_ + '"'
  }
  # PlatformIO's compiler subprocess lookup is reliable when its Windows
  # launcher inherits cmd.exe. Process.WaitForExit also avoids PowerShell 5
  # returning before a native launcher when this script is called from WSL.
  $commandLine = '"' + ($quotedArguments -join " ") + '"'
  $startInfo = [Diagnostics.ProcessStartInfo]::new()
  $startInfo.FileName = $env:COMSPEC
  $startInfo.Arguments = "/d /s /c $commandLine"
  $startInfo.WorkingDirectory = $repoRoot
  $startInfo.UseShellExecute = $false
  $pathExtensions = $env:PATHEXT
  if ($pathExtensions -notmatch "(?i)(^|;)\.EXE($|;)") {
    # WSLENV can reduce PATHEXT to .CPL when powershell.exe is launched from
    # WSL. SCons then cannot resolve the ESP32 toolchain executables by name.
    $pathExtensions = ".COM;.EXE;.BAT;.CMD;$pathExtensions"
  }
  $startInfo.EnvironmentVariables["PATHEXT"] = $pathExtensions
  $process = [Diagnostics.Process]::Start($startInfo)
  $process.WaitForExit()
  if ($process.ExitCode -ne 0) {
    throw "$Description failed with exit code $($process.ExitCode)."
  }
}

foreach ($required in @($pio, $python, $esptool, $framework)) {
  if (-not (Test-Path -LiteralPath $required)) {
    throw "Required PlatformIO component was not found: $required"
  }
}

$previousVersion = [Environment]::GetEnvironmentVariable(
  "CHARADOCK_STACKCHAN_FIRMWARE_VERSION",
  "Process"
)
try {
  $env:CHARADOCK_STACKCHAN_FIRMWARE_VERSION = $Version
  Invoke-CheckedNative `
    -FilePath $pio `
    -Arguments @("run", "--project-dir", $projectDir, "--target", "clean") `
    -Description "PlatformIO clean"
  Invoke-CheckedNative `
    -FilePath $pio `
    -Arguments @("run", "--project-dir", $projectDir) `
    -Description "PlatformIO build"
}
finally {
  if ($null -eq $previousVersion) {
    Remove-Item Env:CHARADOCK_STACKCHAN_FIRMWARE_VERSION -ErrorAction SilentlyContinue
  }
  else {
    $env:CHARADOCK_STACKCHAN_FIRMWARE_VERSION = $previousVersion
  }
}

$bootloader = Join-Path $buildDir "bootloader.bin"
$partitions = Join-Path $buildDir "partitions.bin"
$bootApp = Join-Path $framework "tools\partitions\boot_app0.bin"
$application = Join-Path $buildDir "firmware.bin"
foreach ($required in @($bootloader, $partitions, $bootApp, $application)) {
  if (-not (Test-Path -LiteralPath $required)) {
    throw "Required firmware component was not generated: $required"
  }
}

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
Invoke-CheckedNative `
  -FilePath $python `
  -Arguments @(
    $esptool,
    "--chip", "esp32s3",
    "merge_bin",
    "--flash_mode", "dio",
    "--flash_freq", "80m",
    "--flash_size", "16MB",
    "-o", $output,
    "0x0", $bootloader,
    "0x8000", $partitions,
    "0xe000", $bootApp,
    "0x10000", $application
  ) `
  -Description "Firmware merge"

$binaryText = [Text.Encoding]::ASCII.GetString(
  [IO.File]::ReadAllBytes($output)
)
if (-not $binaryText.Contains($Version)) {
  throw "Release image does not contain the requested firmware version."
}
$privateRoots = @(
  [Environment]::GetFolderPath([Environment+SpecialFolder]::UserProfile),
  $repoRoot
) | Where-Object { $_ } | ForEach-Object {
  $_.Replace("\", "/").TrimEnd("/")
} | Select-Object -Unique
foreach ($privateRoot in $privateRoots) {
  if (
    $binaryText.Contains($privateRoot) -or
    $binaryText.Contains($privateRoot.Replace("/", "\"))
  ) {
    throw "Release image contains a private build path."
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
