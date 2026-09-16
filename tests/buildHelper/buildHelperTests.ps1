# SPDX-License-Identifier: MIT

[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$SourceDirectory,
  [Parameter(Mandatory)][string]$TestBinaryRoot,
  [Parameter(Mandatory)][string]$CMakePath,
  [Parameter(Mandatory)][string]$MakeProgram
)

$ErrorActionPreference = 'Stop'
foreach ($path in @($SourceDirectory, $TestBinaryRoot, $CMakePath, $MakeProgram)) {
  if (-not [IO.Path]::IsPathRooted($path)) {
    throw "Expected an absolute path: $path"
  }
}
if (-not (Test-Path -LiteralPath $CMakePath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $MakeProgram -PathType Leaf)) {
  throw 'Native CMake and mingw32-make are required for the build helper tests.'
}

$fixture = Join-Path $TestBinaryRoot ('build-helper-' + [guid]::NewGuid().ToString('N'))
if (Test-Path -LiteralPath $fixture) {
  throw "The fixture directory already exists: $fixture"
}
$powershell = (Get-Process -Id $PID).Path
$fixtureTemplates = Join-Path $SourceDirectory 'tests/buildHelper'
$cmakeDirectory = Split-Path -Parent $CMakePath
$projectPresets = Get-Content -LiteralPath (Join-Path $SourceDirectory 'CMakePresets.json') -Raw | ConvertFrom-Json
$projectBase = $projectPresets.configurePresets | Where-Object { $_.name -eq 'mingw-base' }
$rootCacheExpression = $projectBase.cacheVariables.HAVREMOTE_MINGW_ROOT
if ($rootCacheExpression -cne '$env{HAVREMOTE_MINGW_ROOT}') {
  throw 'The shared MinGW preset must synchronize HAVREMOTE_MINGW_ROOT from its environment.'
}
New-Item -ItemType Directory -Path $fixture | Out-Null
$passed = 0

function Assert-True([bool]$Condition, [string]$Message) {
  if (-not $Condition) {
    throw $Message
  }
}

function Run-Case {
  param(
    [string]$Name,
    [string]$Configuration = 'Release',
    [ValidateSet('local', 'explicit', 'empty', 'environment', 'missing', 'invalid')]
    [string]$RootMode = 'local',
    [string]$FailStage = '',
    [bool]$Package = $false,
    [bool]$SeedStaleCache = $false,
    [string]$LocalConfiguration = '',
    [int]$ExpectedExitCode = 0,
    [string[]]$ExpectedStages = @(),
    [string]$ExpectedError = ''
  )

  $caseDirectory = Join-Path $fixture $Name
  $source = Join-Path $caseDirectory 'source with spaces'
  $caller = Join-Path $caseDirectory 'caller with spaces'
  $root = Join-Path $caseDirectory 'toolchain with spaces'
  $oldRoot = Join-Path $caseDirectory 'original toolchain'
  foreach ($directory in @((Join-Path $source 'scripts'), $caller, (Join-Path $root 'bin'))) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
  }
  Copy-Item -LiteralPath (Join-Path $SourceDirectory 'scripts/build.ps1') -Destination (Join-Path $source 'scripts/build.ps1')
  foreach ($file in @('CMakeLists.txt', 'stage.cmake.in')) {
    Copy-Item -LiteralPath (Join-Path $fixtureTemplates $file) -Destination $source
  }

  $base = [ordered]@{
    name = 'mingw-base'
    hidden = $true
    generator = 'MinGW Makefiles'
    cacheVariables = @{
      CMAKE_MAKE_PROGRAM = $MakeProgram.Replace('\', '/')
      HAVREMOTE_MINGW_ROOT = $rootCacheExpression
      FIXTURE_EXPECTED_ROOT = $root.Replace('\', '/')
      FIXTURE_EXPECTED_MARKER = 'shared'
    }
  }
  $configurePresets = @($base)
  $buildPresets = @()
  $testPresets = @()
  foreach ($config in @('debug', 'release')) {
    $preset = "mingw-$config"
    $configurePresets += @{
      name = $preset
      inherits = 'mingw-base'
      binaryDir = ('${sourceDir}/shared ' + $config)
    }
    $buildPresets += @{ name = $preset; configurePreset = $preset }
    $testPresets += @{ name = $preset; configurePreset = $preset; output = @{ outputOnFailure = $true } }
  }
  @{
    version = 9
    configurePresets = $configurePresets
    buildPresets = $buildPresets
    testPresets = $testPresets
  } | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $source 'CMakePresets.json') -Encoding UTF8

  if ($RootMode -ne 'missing') {
    $localBase = @{
      name = 'local-environment'
      hidden = $true
      environment = @{
        HAVREMOTE_HELPER_ROOT = $root.Replace('\', '/')
        HAVREMOTE_MINGW_ROOT = '$env{HAVREMOTE_HELPER_ROOT}'
        HAVREMOTE_HELPER_MARKER = 'inherited local environment'
        PATH = '$env{HAVREMOTE_MINGW_ROOT}/bin;$penv{PATH}'
      }
      cacheVariables = @{ FIXTURE_EXPECTED_MARKER = 'inherited local environment' }
    }
    $localConfigure = @($localBase)
    $localBuild = @()
    $localTest = @()
    foreach ($config in @('debug', 'release')) {
      if ($LocalConfiguration -and $config -ne $LocalConfiguration) {
        continue
      }
      $preset = "mingw-$config-local"
      $localConfigure += @{
        name = $preset
        inherits = @('local-environment', "mingw-$config")
        binaryDir = ('${sourceDir}/alternate output/' + $config)
      }
      $localBuild += @{ name = $preset; configurePreset = $preset }
      $localTest += @{ name = $preset; configurePreset = $preset; output = @{ outputOnFailure = $true } }
    }
    @{
      version = 9
      configurePresets = $localConfigure
      buildPresets = $localBuild
      testPresets = $localTest
    } | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $source 'CMakeUserPresets.json') -Encoding UTF8
  }

  $caseFile = Join-Path $caseDirectory 'case.json'
  $reportPath = Join-Path $caseDirectory 'result.json'
  $explicitRoot = ''
  $environmentRoot = ''
  if ($RootMode -eq 'explicit') {
    $explicitRoot = $root
    $environmentRoot = $oldRoot
  }
  elseif ($RootMode -eq 'environment') {
    $environmentRoot = $root
  }
  elseif ($RootMode -eq 'empty') {
    $environmentRoot = $root
  }
  elseif ($RootMode -eq 'invalid') {
    $explicitRoot = Join-Path $caseDirectory 'missing toolchain'
  }
  @{
    SourceDirectory = $source
    CallerDirectory = $caller
    CMakeDirectory = $cmakeDirectory
    Configuration = $Configuration
    ExplicitRoot = $explicitRoot
    ExplicitRootSupplied = ($RootMode -in @('explicit', 'empty', 'invalid'))
    EnvironmentRoot = $environmentRoot
    FailStage = $FailStage
    Test = $true
    Package = $Package
    ReportPath = $reportPath
  } | ConvertTo-Json | Set-Content -LiteralPath $caseFile -Encoding UTF8

  $binaryDir = if ($RootMode -eq 'local') {
    Join-Path $source ("alternate output/" + $Configuration.ToLowerInvariant())
  }
  else {
    Join-Path $source ("shared " + $Configuration.ToLowerInvariant())
  }
  if ($SeedStaleCache) {
    New-Item -ItemType Directory -Path $binaryDir -Force | Out-Null
    "HAVREMOTE_MINGW_ROOT:PATH=$($oldRoot.Replace('\', '/'))" |
      Set-Content -LiteralPath (Join-Path $binaryDir 'CMakeCache.txt') -Encoding UTF8
  }

  $start = New-Object Diagnostics.ProcessStartInfo
  $start.FileName = $powershell
  $runner = Join-Path $fixtureTemplates 'invokeHelper.ps1'
  $start.Arguments = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$runner`" -CaseFile `"$caseFile`""
  $start.UseShellExecute = $false
  $start.RedirectStandardOutput = $true
  $start.RedirectStandardError = $true
  $process = [Diagnostics.Process]::Start($start)
  $stdout = $process.StandardOutput.ReadToEndAsync()
  $stderr = $process.StandardError.ReadToEndAsync()
  if (-not $process.WaitForExit(60000)) {
    $process.Kill()
    throw "$Name exceeded the 60-second case timeout"
  }
  $output = $stdout.Result + $stderr.Result
  $output | Set-Content -LiteralPath (Join-Path $caseDirectory 'output.log') -Encoding UTF8
  $exitCode = $process.ExitCode
  $process.Dispose()
  Assert-True ($exitCode -eq $ExpectedExitCode) "$Name returned $exitCode instead of $ExpectedExitCode`n$output"
  Assert-True (Test-Path -LiteralPath $reportPath) "$Name did not restore the caller and return a report`n$output"
  $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
  Assert-True ($report.ExitCode -eq $ExpectedExitCode) "$Name reported an unexpected native exit code"
  Assert-True ($report.DirectoryRestored -and $report.RootRestored -and $report.PathRestored) "$Name did not restore the caller's directory, root, and PATH"
  if ($ExpectedError) {
    Assert-True ($output -match $ExpectedError) "$Name did not report the expected setup error`n$output"
  }
  $stagesPath = Join-Path $source 'stages.txt'
  $stages = @()
  if (Test-Path -LiteralPath $stagesPath) {
    $stages = @(Get-Content -LiteralPath $stagesPath)
  }
  Assert-True (($stages -join ',') -eq ($ExpectedStages -join ',')) "$Name ran unexpected stages: $($stages -join ',')`n$output"
  if ($Package -and $ExpectedExitCode -eq 0) {
    $zip = Join-Path $binaryDir 'packages/fixture.zip'
    Assert-True (Test-Path -LiteralPath $zip -PathType Leaf) "$Name did not package the selected binary directory"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
      Assert-True (@($archive.Entries | Where-Object { $_.Name -eq 'payload.txt' }).Count -eq 1) "$Name produced a ZIP without the fixture payload"
    }
    finally {
      $archive.Dispose()
    }
  }
  Write-Output "$Name passed"
  $script:passed += 1
}

try {
  Run-Case -Name local-release -Package $true -ExpectedStages configure,build,test,symbols,package
  Run-Case -Name local-debug -Configuration Debug -ExpectedStages configure,build,test
  Run-Case -Name explicit-root -RootMode explicit -Package $true -ExpectedStages configure,build,test,symbols,package
  Run-Case -Name environment-root -RootMode environment -Configuration Debug -ExpectedStages configure,build,test
  Run-Case -Name local-stale-cache -SeedStaleCache $true -ExpectedStages configure,build,test
  Run-Case -Name explicit-stale-cache -RootMode explicit -SeedStaleCache $true -ExpectedStages configure,build,test
  Run-Case -Name missing-local -RootMode missing -ExpectedExitCode 1 -ExpectedError 'HAVREMOTE_MINGW_ROOT|MinGwRoot|CMakeUserPresets'
  Run-Case -Name wrong-local -LocalConfiguration debug -ExpectedExitCode 1 -ExpectedError 'mingw-release-local'
  Run-Case -Name invalid-root -RootMode invalid -ExpectedExitCode 1 -ExpectedError 'bin directory'
  Run-Case -Name empty-root -RootMode empty -ExpectedExitCode 1 -ExpectedError 'MinGwRoot must'
  Run-Case -Name debug-package -RootMode invalid -Configuration Debug -Package $true -ExpectedExitCode 1 -ExpectedError 'Release'
  Run-Case -Name configure-failure -FailStage configure -Package $true -ExpectedExitCode 1 -ExpectedStages configure
  Run-Case -Name build-failure -FailStage build -Package $true -ExpectedExitCode 2 -ExpectedStages configure,build
  Run-Case -Name test-failure -FailStage test -Package $true -ExpectedExitCode 8 -ExpectedStages configure,build,test
  Run-Case -Name symbols-failure -FailStage symbols -Package $true -ExpectedExitCode 2 -ExpectedStages configure,build,test,symbols
  Run-Case -Name package-failure -FailStage package -Package $true -ExpectedExitCode 2 -ExpectedStages configure,build,test,symbols,package
  Write-Output "$passed build helper cases passed"
}
catch {
  Write-Output $_.ToString()
  throw
}
finally {
  Remove-Item -LiteralPath $fixture -Recurse -Force
}
