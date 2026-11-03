# Checks one OMT-HX stream between two PCs and says which end is the limit for a size and frame
# rate. Start the receiver role first (it retries until the sender appears), then the sender role:
#
#   receiver PC:  scripts\link-check.ps1 -Role receiver -Peer STUDIO-PC -Seconds 20
#   sender PC:    scripts\link-check.ps1 -Role sender   -Seconds 20 -Size 3840x2160 -Fps 60
#
# -Gpu (sender) and -Pixels nv12|gpu (receiver) measure the paths that skip the copies to memory.
#
# The tools must be on both PCs: the OMT-HX checkout's build\release, or any folder holding
# omthx-send.exe / omthx-recv.exe and their DLLs, passed with -ExeDir.
# The delay figure the receiver prints only means something when both PCs share a synchronised
# clock; this script judges by fps and by the per-stage times instead.
param(
  [Parameter(Mandatory)][ValidateSet('sender', 'receiver')][string]$Role,
  [string]$Peer = '',        # receiver: name of the PC that will send (its name in the OMT address)
  [string]$Source = '',      # exact OMT address; defaults to "<Peer> (Link Check)"
  [string]$ExeDir = '',      # folder with omthx-send.exe and omthx-recv.exe
  [string]$Size = '3840x2160',
  [int]$Fps = 60,
  [ValidateSet('low', 'medium', 'high')][string]$Quality = 'medium',
  [ValidateSet('hevc', 'h264')][string]$Codec = 'hevc',
  [int]$Seconds = 20,
  [switch]$Gpu,              # sender: send from D3D11 textures (no copy to memory), as BreadCamera does
  [switch]$Uyvy,             # sender: send UYVY from memory with no GPU before the encoder, as an SDI input does
  [ValidateSet('bgra', 'nv12', 'gpu')][string]$Pixels = 'bgra'  # receiver: what the decoded picture becomes
)
$ErrorActionPreference = 'Stop'

if (-not $ExeDir) {
  $candidates = @($PSScriptRoot, (Join-Path $PSScriptRoot '..\build\release'))
  $ExeDir = @($candidates | Where-Object { Test-Path (Join-Path $_ 'omthx-send.exe') })[0]
}
if (-not $ExeDir) { throw 'omthx-send.exe / omthx-recv.exe not found; pass -ExeDir' }
$send = Join-Path $ExeDir 'omthx-send.exe'
$recv = Join-Path $ExeDir 'omthx-recv.exe'
if (!(Test-Path $send) -or !(Test-Path $recv)) {
  throw "omthx-send.exe / omthx-recv.exe not found in $ExeDir; pass -ExeDir"
}

function Median([double[]]$v) {
  if (-not $v -or $v.Count -eq 0) { return 0.0 }
  $s = @($v | Sort-Object)
  return [double]$s[[int]($s.Count / 2)]
}

function Show-LinkInfo {
  Write-Host '--- link ---'
  $adapters = @(Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'Up' })
  if ($adapters.Count) {
    foreach ($a in $adapters) { Write-Host ("  {0}: {1}  {2}" -f $a.Name, $a.LinkSpeed, $a.InterfaceDescription) }
  } else {
    Write-Host '  (no adapter information)'
  }
  if ($Peer) {
    $pings = @(Test-Connection -ComputerName $Peer -Count 4 -ErrorAction SilentlyContinue)
    if ($pings.Count) {
      $avg = ($pings | Measure-Object -Property ResponseTime -Average).Average
      Write-Host ("  ping {0}: avg {1:N0} ms, {2} of 4" -f $Peer, $avg, $pings.Count)
    } else {
      Write-Host "  ping $Peer failed (ICMP is often blocked; rely on the adapter speed above)"
    }
  }
}

# Runs a tool, prints its lines as they are written, and returns all of them.
function Invoke-Tool([string]$exe, [string]$arguments, [string]$logPath) {
  Remove-Item $logPath, "$logPath.err" -ErrorAction SilentlyContinue
  $p = Start-Process -FilePath $exe -ArgumentList $arguments -RedirectStandardOutput $logPath `
    -RedirectStandardError "$logPath.err" -NoNewWindow -PassThru
  $printed = 0
  while (-not $p.HasExited) {
    Start-Sleep -Milliseconds 300
    if (Test-Path $logPath) {
      $lines = @(Get-Content $logPath)
      for ($i = $printed; $i -lt $lines.Count; $i++) { Write-Host $lines[$i] }
      $printed = $lines.Count
    }
  }
  $lines = @(Get-Content $logPath -ErrorAction SilentlyContinue)
  for ($i = $printed; $i -lt $lines.Count; $i++) { Write-Host $lines[$i] }
  if ($p.ExitCode -ne 0 -and (Test-Path "$logPath.err")) {
    @(Get-Content "$logPath.err") | Where-Object { $_ } | ForEach-Object { Write-Host $_ -ForegroundColor DarkYellow }
  }
  return $lines
}

$budget = 1000.0 / $Fps
Show-LinkInfo

if ($Role -eq 'sender') {
  $log = Join-Path $ExeDir 'link-check.send.log'
  if ($Gpu -and $Uyvy) { throw 'pass -Gpu or -Uyvy, not both' }
  $from = if ($Gpu) { 'from GPU textures' } elseif ($Uyvy) { 'UYVY from memory, as SDI' } else { 'BGRA from memory' }
  Write-Host ("--- sending {0} @ {1} fps, {2}, {3}, {4} ---" -f $Size, $Fps, $Quality, $Codec, $from)
  $lines = Invoke-Tool $send ("--name `"Link Check`" --mode hx --quality $Quality --codec $Codec " +
    "--size $Size --fps $Fps --seconds $Seconds" + $(if ($Gpu) { ' --gpu' } elseif ($Uyvy) { ' --uyvy' } else { '' })) $log

  $stats = @($lines | Where-Object { $_ -match '\d+ fps' })
  if ($stats.Count -gt 2) { $stats = $stats[2..($stats.Count - 1)] }  # two-second warm-up
  $fpsList = @($stats | ForEach-Object { if ($_ -match '(\d+) fps') { [double]$Matches[1] } })
  $enc = @($stats | ForEach-Object { if ($_ -match 'encode\s+([\d.]+) ms') { [double]$Matches[1] } })
  $drop = 0
  if ($stats.Count -and $stats[-1] -match 'dropped\s+(\d+)') { $drop = [int]$Matches[1] }
  $encR = [math]::Round((Median $enc), 1)
  $fpsR = [math]::Round((Median $fpsList), 0)

  Write-Host ''
  Write-Host ("budget at {0} fps: {1:N1} ms per frame" -f $Fps, $budget)
  if ($encR -gt $budget) {
    Write-Host ("SENDER IS THE LIMIT: encode {0:N1} ms > {1:N1} ms budget ({2:N0} fps sent)" -f $encR, $budget, $fpsR) -ForegroundColor Yellow
  } elseif ($fpsR -lt 0.95 * $Fps) {
    Write-Host ("SENDER sent {0:N0} fps with encode only {1:N1} ms: the pacing loop, not the codec, is late" -f $fpsR, $encR) -ForegroundColor Yellow
  } else {
    Write-Host ("sender holds {0:N0} fps (encode {1:N1} ms, budget {2:N1} ms)" -f $fpsR, $encR, $budget) -ForegroundColor Green
  }
  if ($drop -gt 0) {
    Write-Host ("the TCP send queue dropped $drop frames: the link or the receiver was behind") -ForegroundColor Yellow
  }
  # Timed on this PC's clock alone (the receiver names frames it handed out), so no clock sync is needed.
  $rtt = @($lines | ForEach-Object { if ($_ -match 'round trip ([\d.]+) ms') { [double]$Matches[1] } })
  if ($rtt.Count) {
    Write-Host ("round trip, send -> receiver's app -> reply: median {0:N1} ms, max {1:N1} ms over {2} s" -f `
      (Median $rtt), ($rtt | Measure-Object -Maximum).Maximum, $rtt.Count)
  }
  exit 0
}

# ---- receiver ----
if (-not $Source) {
  if ($Peer) {
    $Source = "$Peer (Link Check)"
  } else {
    $listed = @(& $recv --list 2>$null | Where-Object { $_ -like '*(Link Check)*' })
    if ($listed.Count -eq 1) { $Source = $listed[0] }
    else {
      Write-Host 'sources seen on the network:'
      @(& $recv --list 2>$null) | ForEach-Object { Write-Host "  $_" }
      throw 'name the sender with -Peer <PC name>, or pass -Source "<PC> (Link Check)"'
    }
  }
}
$log = Join-Path $ExeDir 'link-check.recv.log'
Write-Host ("--- receiving {0} as {1} ---" -f $Source, $Pixels)
# The sender may not be up yet: retry until it is, then run the whole period.
$lines = @()
for ($attempt = 1; $attempt -le 30; $attempt++) {
  $lines = Invoke-Tool $recv ("--source `"$Source`" --seconds $Seconds --pixels $Pixels") $log
  if (@($lines | Where-Object { $_ -match '^(HX|Full)\s' }).Count) { break }
  Write-Host '  no source yet; retrying in 1 s' -ForegroundColor DarkYellow
  Start-Sleep -Seconds 1
}

$stats = @($lines | Where-Object { $_ -match '^(HX|Full)\s' })
# Silent seconds before the source started sending are not part of the run.
$begin = 0
while ($begin -lt $stats.Count -and $stats[$begin] -match '\s0 fps') { $begin++ }
$stats = if ($begin -lt $stats.Count) { @($stats[$begin..($stats.Count - 1)]) } else { @() }
if ($stats.Count -gt 2) { $stats = $stats[2..($stats.Count - 1)] }
# The sender's run usually ends before this one: drop the silent seconds after it stopped, and the
# part-second before them. Silent seconds in the middle of the run stay, they are stalls.
$end = $stats.Count
while ($end -gt 0 -and $stats[$end - 1] -match '\s0 fps') { $end-- }
$stopped = $stats.Count - $end
if ($stopped -gt 0 -and $end -gt 1) { $end-- }
if ($end -gt 0) { $stats = $stats[0..($end - 1)] }
$fpsList = @($stats | ForEach-Object { if ($_ -match '(\d+) fps') { [double]$Matches[1] } })
$dec = @($stats | ForEach-Object { if ($_ -match 'decode\s+([\d.]+) ms') { [double]$Matches[1] } })
$mbps = @($stats | ForEach-Object { if ($_ -match '([\d.]+) Mbps') { [double]$Matches[1] } })
$decR = [math]::Round((Median $dec), 1)
$fpsR = [math]::Round((Median $fpsList), 0)
$disc = if ($stats.Count -and $stats[-1] -match 'discarded (\d+)') { [int]$Matches[1] } else { 0 }

Write-Host ''
Write-Host ("budget at {0} fps: {1:N1} ms per frame" -f $Fps, $budget)
if ($decR -gt $budget) {
  Write-Host ("RECEIVER IS THE LIMIT: decode {0:N1} ms > {1:N1} ms budget ({2:N0} fps received)" -f $decR, $budget, $fpsR) -ForegroundColor Yellow
} elseif ($fpsR -lt 0.95 * $Fps) {
  Write-Host ("receiver has headroom (decode {0:N1} ms) but only {1:N0} fps arrived: the sender or the link is behind - read the sender role's output" -f $decR, $fpsR) -ForegroundColor Yellow
} else {
  Write-Host ("receiver holds {0:N0} fps (decode {1:N1} ms, budget {2:N1} ms, {3:N1} Mbps)" -f $fpsR, $decR, $budget, (Median $mbps)) -ForegroundColor Green
}
if ($stopped -gt 0) {
  Write-Host ("the source stopped sending {0} s before the end; judged on the {1} s it was sending" -f $stopped, $stats.Count)
}
Write-Host ("frames discarded over the run: {0} (a few at the start are the wait for the first keyframe)" -f $disc)
Write-Host 'note: OBS adds one full-size copy per frame on top of these figures (about 5-10 ms at 4K)'
exit 0
