param([ValidateSet('release', 'debug')][string]$Preset = 'release')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path (Join-Path $root 'third_party\libomt\libomt.lib'))) {
  & (Join-Path $PSScriptRoot 'build-libomt.ps1')
}
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = "$vs\VC\vcpkg" }
Set-Location $root
cmake --preset $Preset
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build --preset $Preset
exit $LASTEXITCODE
