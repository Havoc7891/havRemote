# SPDX-License-Identifier: MIT

param([Parameter(Mandatory)][string]$CaseFile)

$ErrorActionPreference = 'Stop'
$case = Get-Content -LiteralPath $CaseFile -Raw | ConvertFrom-Json
$env:PATH = "$($case.CMakeDirectory);$($case.CMakeDirectory);$env:PATH"
$env:HAVREMOTE_MINGW_ROOT = $case.EnvironmentRoot
$env:HAVREMOTE_HELPER_MARKER = 'shared'
$env:HAVREMOTE_HELPER_FAIL_STAGE = $case.FailStage
Set-Location -LiteralPath $case.CallerDirectory
$originalDirectory = (Get-Location).Path
$originalRoot = $env:HAVREMOTE_MINGW_ROOT
$originalPath = $env:PATH
$arguments = @{ Configuration = $case.Configuration }
if ($case.ExplicitRootSupplied) {
  $arguments.MinGwRoot = $case.ExplicitRoot
}
if ($case.Test) {
  $arguments.Test = $true
}
if ($case.Package) {
  $arguments.Package = $true
}

$exitCode = 0
$errorText = ''
try {
  & (Join-Path $case.SourceDirectory 'scripts/build.ps1') @arguments
  $exitCode = $LASTEXITCODE
}
catch {
  $exitCode = 1
  $errorText = $_.ToString()
  Write-Output $errorText
}
finally {
  [ordered]@{
    ExitCode = $exitCode
    Error = $errorText
    DirectoryRestored = ((Get-Location).Path -ceq $originalDirectory)
    RootRestored = ($env:HAVREMOTE_MINGW_ROOT -ceq $originalRoot)
    PathRestored = ($env:PATH -ceq $originalPath)
  } | ConvertTo-Json | Set-Content -LiteralPath $case.ReportPath -Encoding UTF8
}
exit $exitCode
