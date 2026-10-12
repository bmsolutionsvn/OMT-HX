# Runs omthx-send against omthx-recv on one PC over localhost for a matrix of cases and prints a
# steady-state table. No network in the path, so these are codec and library figures, like
# docs\MEASUREMENTS.md. Build the tools first (scripts\test.ps1).
param(
  [int]$Seconds = 20,
  [string]$Only = ''   # substring filter on the case tag
)
$ErrorActionPreference = 'Stop'
$dir = Join-Path $PSScriptRoot '..\build\release'
$send = Join-Path $dir 'omthx-send.exe'
$recv = Join-Path $dir 'omthx-recv.exe'
if (!(Test-Path $send) -or !(Test-Path $recv)) { throw 'build the tools first: scripts\test.ps1' }
$addr = "$env:COMPUTERNAME (Measure)"

$cases = @(
  @{ Tag = 'hx 720p60';        Mode = 'hx';   Q = 'medium'; Size = '1280x720';  Fps = 60;  Alpha = $false },
  @{ Tag = 'hx 1080p60';       Mode = 'hx';   Q = 'medium'; Size = '1920x1080'; Fps = 60;  Alpha = $false },
  @{ Tag = 'hx 1440p60';       Mode = 'hx';   Q = 'medium'; Size = '2560x1440'; Fps = 60;  Alpha = $false },
  @{ Tag = 'hx 2160p60';       Mode = 'hx';   Q = 'medium'; Size = '3840x2160'; Fps = 60;  Alpha = $false },
  @{ Tag = 'hx 1080p30';       Mode = 'hx';   Q = 'medium'; Size = '1920x1080'; Fps = 30;  Alpha = $false },
  @{ Tag = 'hx 1080p120';      Mode = 'hx';   Q = 'medium'; Size = '1920x1080'; Fps = 120; Alpha = $false },
  @{ Tag = 'hx 1080p60 low';   Mode = 'hx';   Q = 'low';    Size = '1920x1080'; Fps = 60;  Alpha = $false },
  @{ Tag = 'hx 1080p60 high';  Mode = 'hx';   Q = 'high';   Size = '1920x1080'; Fps = 60;  Alpha = $false },
  @{ Tag = 'hx 720p60 alpha';  Mode = 'hx';   Q = 'medium'; Size = '1280x720';  Fps = 60;  Alpha = $true },
  @{ Tag = 'hx 1080p60 alpha'; Mode = 'hx';   Q = 'medium'; Size = '1920x1080'; Fps = 60;  Alpha = $true },
  @{ Tag = 'hx 1080p30 alpha'; Mode = 'hx';   Q = 'medium'; Size = '1920x1080'; Fps = 30;  Alpha = $true },
  @{ Tag = 'full 720p60';      Mode = 'full'; Q = 'medium'; Size = '1280x720';  Fps = 60;  Alpha = $false },
  @{ Tag = 'full 1080p60';     Mode = 'full'; Q = 'medium'; Size = '1920x1080'; Fps = 60;  Alpha = $false }
)

function Median([double[]]$v) {
  if ($v.Count -eq 0) { return 0.0 }
  $s = @($v | Sort-Object)
  return [double]$s[[int]($s.Count / 2)]
}
function Steady([object[]]$lines) {
  $l = @($lines)
  if ($l.Count -gt 2) { return $l[2..($l.Count - 1)] }  # drop a two-second warm-up
  return $l
}

$rows = @()
foreach ($c in $cases) {
  if ($Only -and $c.Tag -notlike "*$Only*") { continue }
  $slog = Join-Path $dir 'm.send.log'; $rlog = Join-Path $dir 'm.recv.log'
  Remove-Item $slog, $rlog -ErrorAction SilentlyContinue
  $sargs = "--name Measure --mode $($c.Mode) --quality $($c.Q) --size $($c.Size) --fps $($c.Fps) --seconds $Seconds"
  if ($c.Alpha) { $sargs += ' --alpha' }
  $sp = Start-Process -FilePath $send -ArgumentList $sargs -RedirectStandardOutput $slog -RedirectStandardError (Join-Path $dir 'm.send.err') -NoNewWindow -PassThru
  Start-Sleep -Milliseconds 1500
  $rp = Start-Process -FilePath $recv -ArgumentList "--source `"$addr`" --seconds $($Seconds - 1)" -RedirectStandardOutput $rlog -RedirectStandardError (Join-Path $dir 'm.recv.err') -NoNewWindow -PassThru
  $sp.WaitForExit(); $rp.WaitForExit(); Start-Sleep -Milliseconds 300

  $sl = Steady (@(Get-Content $slog) | Where-Object { $_ -match '\d+ fps' })
  $rl = Steady (@(Get-Content $rlog) | Where-Object { $_ -match '^(HX|Full)\s' })
  $enc = @($sl | ForEach-Object { if ($_ -match 'encode\s+([\d.]+) ms') { [double]$Matches[1] } })
  $sfps = @($sl | ForEach-Object { if ($_ -match '(\d+) fps') { [int]$Matches[1] } })
  $rmb = @($rl | ForEach-Object { if ($_ -match '([\d.]+) Mbps') { [double]$Matches[1] } })
  $dec = @($rl | ForEach-Object { if ($_ -match 'decode\s+([\d.]+) ms') { [double]$Matches[1] } })
  $fps = @($rl | ForEach-Object { if ($_ -match '(\d+) fps') { [int]$Matches[1] } })
  $davg = @($rl | ForEach-Object { if ($_ -match 'delay avg\s+([\d.]+)') { [double]$Matches[1] } })
  $dmax = @($rl | ForEach-Object { if ($_ -match 'max\s+([\d.]+) ms') { [double]$Matches[1] } })
  $disc = if ($rl.Count -and $rl[-1] -match 'discarded (\d+)') { [int]$Matches[1] } else { 0 }

  $rows += [pscustomobject]@{
    Case       = $c.Tag
    Sender_fps = if ($sfps.Count) { [int](Median $sfps) } else { 0 }
    Recv_fps   = if ($fps.Count) { [int](Median $fps) } else { 0 }
    Mbps       = [math]::Round((Median $rmb), 1)
    Encode_ms  = [math]::Round((Median $enc), 1)
    Decode_ms  = [math]::Round((Median $dec), 1)
    Delay_avg  = [math]::Round((Median $davg), 1)
    Delay_max  = if ($dmax.Count) { [math]::Round((($dmax | Measure-Object -Maximum).Maximum), 1) } else { 0 }
    Discarded  = $disc
    Secs       = $rl.Count
  }
  ($rows[-1] | Format-Table -AutoSize | Out-String).Trim() | Write-Host
}
$rows | Export-Csv -NoTypeInformation (Join-Path $dir 'measure.csv')
Write-Host "`nCSV: $(Join-Path $dir 'measure.csv')"