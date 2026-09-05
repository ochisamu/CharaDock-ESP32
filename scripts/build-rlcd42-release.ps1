# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
  [string]$Version = "0.2.0-preview"
)

$ErrorActionPreference = "Stop"
if ($Version -notmatch "^[0-9A-Za-z][0-9A-Za-z._-]{0,31}$") {
  throw "Version must contain only letters, digits, dot, underscore, or hyphen."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$projectDir = Join-Path $repoRoot "firmware\waveshare-rlcd-4.2"
$pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
$buildDir = Join-Path $projectDir ".pio\build\waveshare-rlcd-42"
$factoryImage = Join-Path $buildDir "firmware.factory.bin"
$outputDir = Join-Path $repoRoot "dist\rlcd-4.2"
$output = Join-Path $outputDir "CharaDock-Waveshare-RLCD-4.2-v$Version.bin"
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
  $commandLine = '"' + ($quotedArguments -join " ") + '"'
  $startInfo = [Diagnostics.ProcessStartInfo]::new()
  $startInfo.FileName = $env:COMSPEC
  $startInfo.Arguments = "/d /s /c $commandLine"
  $startInfo.WorkingDirectory = $repoRoot
  $startInfo.UseShellExecute = $false
  $pathExtensions = $env:PATHEXT
  if ($pathExtensions -notmatch "(?i)(^|;)\.EXE($|;)") {
    $pathExtensions = ".COM;.EXE;.BAT;.CMD;$pathExtensions"
  }
  $startInfo.EnvironmentVariables["PATHEXT"] = $pathExtensions
  $process = [Diagnostics.Process]::Start($startInfo)
  $process.WaitForExit()
  if ($process.ExitCode -ne 0) {
    throw "$Description failed with exit code $($process.ExitCode)."
  }
}

if (-not (Test-Path -LiteralPath $pio)) {
  throw "PlatformIO was not found: $pio"
}

$previousVersion = [Environment]::GetEnvironmentVariable(
  "CHARADOCK_RLCD_FIRMWARE_VERSION",
  "Process"
)
try {
  $env:CHARADOCK_RLCD_FIRMWARE_VERSION = $Version
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
    Remove-Item Env:CHARADOCK_RLCD_FIRMWARE_VERSION -ErrorAction SilentlyContinue
  }
  else {
    $env:CHARADOCK_RLCD_FIRMWARE_VERSION = $previousVersion
  }
}

if (-not (Test-Path -LiteralPath $factoryImage)) {
  throw "Combined address-0x0 firmware was not generated: $factoryImage"
}
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
Copy-Item -LiteralPath $factoryImage -Destination $output -Force

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
