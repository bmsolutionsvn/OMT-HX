param(
  [ValidateSet('release', 'debug')][string]$Preset = 'release',
  [string]$Filter = ''
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1') -Preset $Preset
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Set-Location (Join-Path $PSScriptRoot "..\build\$Preset")
if ($Filter) { & .\omthx_tests.exe "--test-case=$Filter" } else { & .\omthx_tests.exe }
exit $LASTEXITCODE
