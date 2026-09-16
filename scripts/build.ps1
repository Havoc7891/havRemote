# SPDX-License-Identifier: MIT

[CmdletBinding()]
param(
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Debug',
  [string]$MinGwRoot = $env:HAVREMOTE_MINGW_ROOT,
  [switch]$Test,
  [switch]$Package
)

$ErrorActionPreference = 'Stop'
if ($Package -and $Configuration -ne 'Release') {
  throw 'Packaging is supported only for Release builds.'
}

$cmake = Get-Command cmake.exe -CommandType Application -ErrorAction Stop |
  Select-Object -First 1
$cmakeDirectory = Split-Path -Parent $cmake.Source
$ctest = Join-Path $cmakeDirectory 'ctest.exe'

$preset = if ($Configuration -eq 'Release') {
  'mingw-release'
}
else {
  'mingw-debug'
}

$hasToolchainRoot = -not [string]::IsNullOrWhiteSpace($MinGwRoot)
if ($PSBoundParameters.ContainsKey('MinGwRoot') -and -not $hasToolchainRoot) {
  throw '-MinGwRoot must name the MinGW distribution''s mingw64 directory.'
}
if ($hasToolchainRoot) {
  if (-not (Test-Path -LiteralPath (Join-Path $MinGwRoot 'bin') -PathType Container)) {
    throw "The MinGW root has no bin directory: $MinGwRoot"
  }
  $MinGwRoot = (Resolve-Path -LiteralPath $MinGwRoot).ProviderPath
}

$previousRoot = $env:HAVREMOTE_MINGW_ROOT
$previousPath = $env:PATH
Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
  $configureArguments = @()
  if ($hasToolchainRoot) {
    $env:HAVREMOTE_MINGW_ROOT = $MinGwRoot
    $env:PATH = "$(Join-Path $MinGwRoot 'bin');$env:PATH"
    $configureArguments += "-DHAVREMOTE_MINGW_ROOT:PATH=$MinGwRoot"
  }
  else {
    $preset += '-local'
    $availablePresets = & $cmake.Source --list-presets=configure
    if ($LASTEXITCODE -ne 0) {
      exit $LASTEXITCODE
    }
    $presetPattern = '^\s*"' + [regex]::Escape($preset) + '"(?:\s|$)'
    if (-not ($availablePresets -match $presetPattern)) {
      throw "Create the '$preset' preset in CMakeUserPresets.json, set HAVREMOTE_MINGW_ROOT, or pass -MinGwRoot. See docs/building.md."
    }
  }

  Write-Host "Using CMake preset: $preset"
  & $cmake.Source --preset $preset @configureArguments
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }
  & $cmake.Source --build --preset $preset
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }

  if ($Test) {
    & $ctest --preset $preset
    if ($LASTEXITCODE -ne 0) {
      exit $LASTEXITCODE
    }
  }

  if ($Package) {
    & $cmake.Source --build --preset $preset --target package-symbols
    if ($LASTEXITCODE -ne 0) {
      exit $LASTEXITCODE
    }
    & $cmake.Source --build --preset $preset --target package
    if ($LASTEXITCODE -ne 0) {
      exit $LASTEXITCODE
    }
  }
}
finally {
  $env:HAVREMOTE_MINGW_ROOT = $previousRoot
  $env:PATH = $previousPath
  Pop-Location
}
