# Builds libomt.dll from pinned upstream source plus patches/libomtnet-hx.patch and collects it,
# with libvmx.dll, into third_party/libomt. -Stock skips the patch (used once, to watch the test fail).
# Needs: git, the .NET 8 SDK, Visual Studio with the C++ desktop workload. Windows PowerShell 5.1 or newer.
param([switch]$Stock)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$work = Join-Path $root 'build\omt-src'
$out = Join-Path $root 'third_party\libomt'
$pins = [ordered]@{
  libomtnet = '7d29031ff3f355ffa1f0cd08ee24d02e258445dd'
  libomt    = 'bb4b67be8e5964f1017592b9fc5406495e704e5d'
}
$zipUrl = 'https://github.com/openmediatransport/libomtnet/releases/download/v1.0.0.19/OpenMediaTransport.Binaries.Release.v1.0.0.19.zip'
$zipSha = 'c62460174499dabe703e3e57372daf5f1dd95f890355d11e30052b9ed35bdac4'

function Check([string]$what) { if ($LASTEXITCODE) { throw "$what failed with exit code $LASTEXITCODE" } }

if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Force $work, $out | Out-Null

foreach ($name in $pins.Keys) {
  $dir = Join-Path $work $name
  git init -q $dir; Check "git init $name"
  git -C $dir fetch -q --depth 1 "https://github.com/openmediatransport/$name.git" $pins[$name]; Check "git fetch $name"
  git -C $dir -c advice.detachedHead=false checkout -q FETCH_HEAD; Check "git checkout $name"
}

if (-not $Stock) {
  git -C (Join-Path $work 'libomtnet') apply --whitespace=nowarn (Join-Path $root 'patches\libomtnet-hx.patch')
  Check 'applying patches\libomtnet-hx.patch'
}

# Native AOT needs the MSVC linker. .NET 8 does not find Visual Studio 2026 by itself, so use its developer shell.
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

dotnet build (Join-Path $work 'libomtnet\libomtnet.csproj') -c Release -f netstandard2.0 -nologo -v q
Check 'dotnet build libomtnet'
dotnet publish (Join-Path $work 'libomt\libomt.sln') -r win-x64 -c Release -p:IlcUseEnvironmentalTools=true -nologo -v q
Check 'dotnet publish libomt'

$zip = Join-Path $work 'omt-release.zip'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $zipUrl -OutFile $zip -UseBasicParsing
$sha = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
if ($sha -ne $zipSha) { throw "OMT release zip has SHA-256 $sha, expected $zipSha" }
Expand-Archive $zip -DestinationPath (Join-Path $work 'omt-release') -Force

$bin = Join-Path $work 'libomt\bin\Release\net8.0\win-x64'
Copy-Item (Join-Path $bin 'native\libomt.dll'), (Join-Path $bin 'native\libomt.lib'), (Join-Path $bin 'publish\libomt.h') $out -Force
Copy-Item (Join-Path $work 'omt-release\Libraries\Winx64\libvmx.dll') $out -Force
Copy-Item (Join-Path $work 'libomtnet\LICENSE.txt') (Join-Path $out 'LICENSE-OMT.txt') -Force

Get-ChildItem $out | ForEach-Object { '{0,10}  {1}' -f $_.Length, $_.Name }
