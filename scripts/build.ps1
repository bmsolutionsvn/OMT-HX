param(
  [ValidateSet('release', 'debug')][string]$Preset = 'release',
  [switch]$Shared  # also build omthx.dll/omthx.so, which the OBS plugin loads
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (Test-Path (Join-Path $root '.git')) {
  git -C $root submodule update --init
  if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
# The developer shell calls vswhere.exe by name, so its folder must be on PATH.
$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$env:PATH = "$installer;$env:PATH"
$vs = & "$installer\vswhere.exe" -latest -products * -property installationPath
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = "$vs\VC\vcpkg" }
Set-Location $root
cmake --preset $Preset "-DOMTHX_SHARED=$($Shared.IsPresent.ToString().ToUpper())"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build --preset $Preset
exit $LASTEXITCODE