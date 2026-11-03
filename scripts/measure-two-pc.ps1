# The cases of scripts\measure.ps1, plus the 4K60 sender and receiver paths, between two PCs. Run the
# receiver role on one PC and the sender role on the other, in either order; each prints its own half
# of the table and writes a CSV. Delay is the sender's round trip, timed on the sender's clock alone
# (send -> receiver's app -> the receiver's short reply), so the PCs' clocks do not need to agree.
#
#   receiver PC:  scripts\measure-two-pc.ps1 -Role receiver -Peer SENDER-PC
#   sender PC:    scripts\measure-two-pc.ps1 -Role sender
#
# The two stay in step without a shared clock: case N is the source "<PC> (Bench NN)", so the receiver
# can only watch the case it is on. The receiver watches each case for -Seconds once pictures arrive;
# the sender of a case stops 3 s after its receiver leaves, then starts the next case.
param(
  [Parameter(Mandatory)][ValidateSet('sender', 'receiver')][string]$Role,
  [string]$Peer = '',       # receiver: the sending PC's name
  [int]$Seconds = 20,
  [string]$Only = '',       # substring filter on the case tag; use the same on both PCs
  [string]$ExeDir = ''      # folder with omthx-send.exe and omthx-recv.exe
)
$ErrorActionPreference = 'Stop'
if (-not $ExeDir) {
  $ExeDir = @($PSScriptRoot, (Join-Path $PSScriptRoot '..\build\release') |
    Where-Object { Test-Path (Join-Path $_ 'omthx-send.exe') })[0]
}
if (-not $ExeDir) { throw 'omthx-send.exe / omthx-recv.exe not found; pass -ExeDir' }
$send = Join-Path $ExeDir 'omthx-send.exe'
$recv = Join-Path $ExeDir 'omthx-recv.exe'
if ($Role -eq 'receiver' -and -not $Peer) { throw 'pass -Peer <the sending PC''s name>' }

# Send: mem = BGRA from memory (what measure.ps1 sends), gpu = D3D11 textures, uyvy = UYVY from memory.
# Recv: what the receiver turns the picture into.
$cases = @(
  @{ Tag = 'hx 720p60';          Mode = 'hx';   Q = 'medium'; Size = '1280x720';  Fps = 60 },
  @{ Tag = 'hx 1080p60';         Mode = 'hx';   Q = 'medium'; Size = '1920x1080'; Fps = 60 },
  @{ Tag = 'hx 1440p60';         Mode = 'hx';   Q = 'medium'; Size = '2560x1440'; Fps = 60 },
  @{ Tag = 'hx 2160p60';         Mode = 'hx';   Q = 'medium'; Size = '3840x2160'; Fps = 60 },
  @{ Tag = 'hx 1080p30';         Mode = 'hx';   Q = 'medium'; Size = '1920x1080'; Fps = 30 },
  @{ Tag = 'hx 1080p120';        Mode = 'hx';   Q = 'medium'; Size = '1920x1080'; Fps = 120 },
  @{ Tag = 'hx 1080p60 low';     Mode = 'hx';   Q = 'low';    Size = '1920x1080'; Fps = 60 },
  @{ Tag = 'hx 1080p60 high';    Mode = 'hx';   Q = 'high';   Size = '1920x1080'; Fps = 60 },
  @{ Tag = 'hx 720p60 alpha';    Mode = 'hx';   Q = 'medium'; Size = '1280x720';  Fps = 60; Alpha = $true },
  @{ Tag = 'hx 1080p60 alpha';   Mode = 'hx';   Q = 'medium'; Size = '1920x1080'; Fps = 60; Alpha = $true },
  @{ Tag = 'hx 1080p30 alpha';   Mode = 'hx';   Q = 'medium'; Size = '1920x1080'; Fps = 30; Alpha = $true },
  @{ Tag = 'h264 1080p60';       Mode = 'hx';   Codec = 'h264'; Q = 'medium'; Size = '1920x1080'; Fps = 60 },
  @{ Tag = 'h264 1080p60 low';   Mode = 'hx';   Codec = 'h264'; Q = 'low';    Size = '1920x1080'; Fps = 60 },
  @{ Tag = 'h264 1080p60 high';  Mode = 'hx';   Codec = 'h264'; Q = 'high';   Size = '1920x1080'; Fps = 60 },
  @{ Tag = 'h264 720p60 alpha';  Mode = 'hx';   Codec = 'h264'; Q = 'medium'; Size = '1280x720';  Fps = 60; Alpha = $true },
  @{ Tag = 'h264 1080p60 alpha'; Mode = 'hx';   Codec = 'h264'; Q = 'medium'; Size = '1920x1080'; Fps = 60; Alpha = $true },
  @{ Tag = 'h264 720p60';        Mode = 'hx';   Codec = 'h264'; Q = 'medium'; Size = '1280x720';  Fps = 60 },
  @{ Tag = 'full 720p60';        Mode = 'full'; Q = 'medium'; Size = '1280x720';  Fps = 60 },
  @{ Tag = 'full 1080p60';       Mode = 'full'; Q = 'medium'; Size = '1920x1080'; Fps = 60 }
)
# The 4K60 paths: each way a picture can enter the sender, to each way the receiver can take it.
foreach ($s in 'mem', 'gpu', 'uyvy') {
  foreach ($r in 'bgra', 'nv12', 'gpu') {
    $cases += @{ Tag = "hx 2160p60 $s>$r"; Mode = 'hx'; Q = 'medium'; Size = '3840x2160'; Fps = 60; Send = $s; Recv = $r }
  }
}
$cases = @($cases | Where-Object { -not $Only -or $_.Tag -like "*$Only*" })

function Median([double[]]$v) {
  if (-not $v -or $v.Count -eq 0) { return 0.0 }
  $s = @($v | Sort-Object)
  return [double]$s[[int]($s.Count / 2)]
}
function Num([object[]]$lines, [string]$pattern) {
  @($lines | ForEach-Object { if ($_ -match $pattern) { [double]$Matches[1] } })
}

$logDir = Join-Path $ExeDir 'measure-two-pc'
New-Item -ItemType Directory -Force $logDir | Out-Null
$rows = @()
$i = 0
foreach ($c in $cases) {
  $i++
  $name = 'Bench {0:D2}' -f $i
  $log = Join-Path $logDir ('{0:D2}-{1}.log' -f $i, $Role)
  if ($Role -eq 'sender') {
    $a = "--name `"$name`" --mode $($c.Mode) --quality $($c.Q) --size $($c.Size) --fps $($c.Fps)" +
      " --until-watched $($Seconds + 30) --seconds 300"
    if ($c.Alpha) { $a += ' --alpha' }
    if ($c.Codec) { $a += " --codec $($c.Codec)" }
    if ($c.Send -eq 'gpu') { $a += ' --gpu' } elseif ($c.Send -eq 'uyvy') { $a += ' --uyvy' }
    Write-Host ("[{0}/{1}] {2}: sending as {3}, waiting for the receiver" -f $i, $cases.Count, $c.Tag, $name)
    $p = Start-Process -FilePath $send -ArgumentList $a -RedirectStandardOutput $log -RedirectStandardError "$log.err" -NoNewWindow -PassThru
    $p.WaitForExit()
    $l = @(Get-Content $log | Where-Object { $_ -match 'connections [1-9]' })
    if ($l.Count -gt 2) { $l = $l[2..($l.Count - 1)] }  # two-second warm-up
    $rtt = Num $l 'round trip ([\d.]+) ms'
    $rows += [pscustomobject]@{
      Case = $c.Tag
      Sender_fps = [int](Median (Num $l '(\d+) fps'))
      Mbps = [math]::Round((Median (Num $l '([\d.]+) Mbps')), 1)
      Send_ms = [math]::Round((Median (Num $l 'encode\s+([\d.]+) ms')), 1)
      RoundTrip_ms = [math]::Round((Median $rtt), 1)
      RoundTrip_max = if ($rtt.Count) { [math]::Round((($rtt | Measure-Object -Maximum).Maximum), 1) } else { 0 }
      Dropped = if ($l.Count -and $l[-1] -match 'dropped (\d+)') { [int]$Matches[1] } else { 0 }
      Secs = $l.Count
    }
  } else {
    $pixels = if ($c.Recv) { $c.Recv } else { 'bgra' }
    Write-Host ("[{0}/{1}] {2}: watching {3} as {4}" -f $i, $cases.Count, $c.Tag, $name, $pixels)
    $a = "--source `"$Peer ($name)`" --pixels $pixels --live-seconds $($Seconds + 2) --seconds 300"
    $p = Start-Process -FilePath $recv -ArgumentList $a -RedirectStandardOutput $log -RedirectStandardError "$log.err" -NoNewWindow -PassThru
    $p.WaitForExit()
    $all = @(Get-Content $log | Where-Object { $_ -match '^(HX|Full)\s' })
    $l = @($all | Where-Object { $_ -notmatch '\s0 fps' })  # the seconds with pictures
    if ($l.Count -gt 2) { $l = $l[2..($l.Count - 1)] }
    $decoder = if ($l.Count -and $l[-1] -match '^(?:HX|Full)\s+(.+?)\s+\d+ fps') { $Matches[1] } else { '' }
    $rows += [pscustomobject]@{
      Case = $c.Tag
      Recv_fps = [int](Median (Num $l '(\d+) fps'))
      Receive_ms = [math]::Round((Median (Num $l 'decode\s+([\d.]+) ms')), 1)
      Discarded = if ($l.Count -and $l[-1] -match 'discarded (\d+)') { [int]$Matches[1] } else { 0 }
      Decoder = $decoder
      Secs = $l.Count
    }
  }
  ($rows[-1] | Format-Table -AutoSize | Out-String).Trim() | Write-Host
}
$csv = Join-Path $logDir "$Role.csv"
$rows | Export-Csv -NoTypeInformation $csv
Write-Host "`n$($rows | Format-Table -AutoSize | Out-String)"
Write-Host "CSV: $csv"
