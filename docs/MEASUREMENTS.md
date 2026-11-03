# OMT-HX measurements

The 2026-11-03 section and the two-PC part of 2026-11-04 are two PCs over a real network
(`scripts\measure-two-pc.ps1`): use them for what a link does. The other sections were measured with `omthx-send` and `omthx-recv`, driven by
`scripts\measure.ps1`, on one PC over `localhost`, test picture with grain. One PC means no network in
the path: those are codec and library figures, not link figures. It also means sender and receiver
share one CPU and one GPU, so each runs slower than it would alone. Each row is the median of a 20 s run
after a 2 s warm-up.

Since 2026-10-20 the GPU codes the fill alone and alpha goes by VMX on the CPU, not stacked under the
fill. The current alpha figures are in the two-PC table and the 2026-10-20 table; the tables after
those keep the stacked-alpha numbers they were measured with.

## 2026-11-04: the receiver's copy catches up

The overlapped copy to memory of 2026-11-03 cost a frame of delay up to 1440p: a receiver whose copy
once missed the half-frame wait handed out each picture a frame late from then on. Now a copy left over
from the last call goes out as soon as it is done, unless the next frame comes first (then the copies
overlap, which 4K needs on a GTX 1650), and every wait polls the copy instead of a blocking `Map`.

Waiting for each copy before handing the picture out (no overlap) was tried and dropped: on the
GTX 1650 it held 4K to 46 to 48 fps to NV12 and 20 to 24 to BGRA, polled or not.

**One PC** (`measure.ps1 -Seconds 15`, the delay on one clock, so to the millisecond), delay avg ms:

| Case | 2026-10-19 | Overlap only (two runs) | Now |
|------|-----------|-------------------------|-----|
| hx 720p60 | 9.7 | 9.1 / 10.1 | 8.5 |
| hx 1080p60 | 15.4 | 16.4 / 35.5 | 15.3 |
| hx 1080p60 low | 14.9 | 25.4 / 28.0 | 14.2 |
| hx 1080p60 high | 17.0 | 26.4 / 33.8 | 15.9 |
| hx 1080p30 | 15.0 | 42.2 / 16.9 | 14.6 |
| hx 1080p120 | 15.7 | 24.8 / 468 (117 fps) | 22.5 |
| hx 1440p60 | 26.0 | 42.1 / 66.2 | 31.6 |
| h264 1080p60 low | 15.4 | 27.8 / 13.5 | 13.5 |
| h264 720p60 | 9.1 | 22.2 / 10.1 | 8.6 |
| hx 2160p60 fps | 43 / 42 | 47 / 47 | 49 / 49 |

The same overlap-only build landed on either figure from run to run; its delay max reached 106 to
123 ms in the rows that fell behind. The rows with alpha and the full rows do not use the copy and did
not change. 1440p moved between 21 and 36 ms over today's runs, the alpha rows by a similar amount.

**Two PCs** (`measure-two-pc.ps1`, as 2026-11-03 below): every case except 1080p120 holds its rate,
and every 4K60 path holds 60 / 60. Round trip ms, 2026-11-03 then now:

- hx 1080p60 high 51.4 -> 35.0, hx 1080p120 42.6 -> 34.9 (104 fps received, was 115).
- hx 1080p60 51.2 -> 50.4, hx 1440p60 52.4 -> 52.1, h264 1080p60 51.2 -> 51.2: the round trip steps a
  whole frame (35, 52 ms), so a picture a few ms over the step reads the same as one a frame late; the
  one-PC figures above are the finer measure.
- 4K60: 50.2 (gpu>gpu) to 72.9 (mem>bgra), within a step of 2026-11-03 on every path; mem>bgra 77.7 ->
  72.7.
- One run had the sender stall for up to 2.8 s per frame in h264 1080p60 (25 fps) and briefly in two
  4K cases, one of them to a `gpu` receiver, which does not use the copy. Rerun, h264 1080p60 held
  60 / 60 at 51.2 ms.

## 2026-11-03: two PCs

`scripts\measure-two-pc.ps1 -Seconds 20` (receiver role on one PC, sender role on the other), OMT-HX
`main` at 9192c09. The cases are `measure.ps1`'s, plus the 4K60 paths at the end.

- **Sender PC:** AMD Ryzen 7 5700X, Radeon RX 6600 (`hevc_amf`, `h264_amf`), wired 1 Gbps Ethernet.
- **Receiver PC:** GeForce GTX 1650 (decodes, `d3d11va`), Intel UHD 630 beside it, Wi-Fi 6 (Intel AX200,
  2.4 Gbps link rate).
- **Delay** is the **round trip on the sender's clock alone**: the app hands a frame to the sender ->
  encode -> network -> decode -> the receiver's app has the picture -> the receiver's short reply ->
  the sender reads it. The two PCs' clocks play no part (the receiver's own "delay" column compares
  two clocks and is not used). It is an upper bound on the one-way delay: the sender reads replies
  when it sends its next frame, so the figure rounds up to a frame (the steps 17, 35, 52, 69, 87 ms at
  60 fps), and the reply's own trip back over Wi-Fi is in it.
- **Send ms**: the sender's time per frame (pack, upload, encode). **Receive ms**: the receiver's time
  for the picture it hands out (decode, and for the memory paths the copy to memory). With a `gpu`
  receiver the picture is handed over while the GPU still decodes it, so its Receive ms and round trip
  leave the decode out. Full (VMX) is decoded inside libomt and not timed here.
- **Discarded** is the receiver's total over the run, including the frames before the first keyframe
  it waits for when it joins (11 to 19 at 60 fps here). In the per-second link-check runs of the same
  day, the discards all came in that first wait.
- The 4K60 paths: the sender takes the picture as BGRA from memory (`mem`, what every other row and
  `measure.ps1` send), as D3D11 textures (`gpu`: `sendVideoD3D11`, as BreadCamera and desktop capture
  send) or as UYVY from memory (`uyvy`: `sendVideoUyvy`, as an SDI or NDI input sends). The receiver
  hands it out as BGRA in memory (`bgra`, every other row), NV12 in memory (`nv12`) or a D3D11 texture
  (`gpu`).

| Case | Sender fps | Recv fps | Mbps | Send ms | Receive ms | Round trip ms | Round trip max | Discarded |
|------|-----------|----------|------|---------|------------|---------------|----------------|-----------|
| hx 720p60 | 60 | 60 | 13 | 4.1 | 6.6 | 17.3 | 30.8 | 11 |
| hx 1080p60 | 60 | 60 | 29.9 | 7.8 | 2.2 | 51.2 | 52.3 | 12 |
| hx 1440p60 | 60 | 60 | 52.7 | 10.2 | 2.6 | 52.4 | 53.2 | 14 |
| hx 2160p60 | 60 | 60 | 119.5 | 16.5 | 8.6 | 87.6 | 92.8 | 17 |
| hx 1080p30 | 30 | 30 | 14.9 | 7 | 12 | 34.9 | 35.8 | 12 |
| hx 1080p120 | 120 | 115 | 59.8 | 6.6 | 2.2 | 42.6 | 52.1 | 15 |
| hx 1080p60 low | 60 | 60 | 15 | 6.5 | 1.8 | 34.9 | 51.5 | 12 |
| hx 1080p60 high | 60 | 60 | 49.9 | 6.8 | 1.9 | 51.4 | 52.5 | 12 |
| hx 720p60 alpha | 60 | 60 | 17.8 | 4.9 | 5 | 18.2 | 19.7 | 12 |
| hx 1080p60 alpha | 60 | 60 | 38.2 | 9.1 | 9 | 36.3 | 52.3 | 12 |
| hx 1080p30 alpha | 30 | 30 | 19.1 | 8.9 | 9 | 36.5 | 37.2 | 11 |
| h264 1080p60 | 60 | 60 | 41.8 | 6.8 | 2 | 51.2 | 68 | 12 |
| h264 1080p60 low | 60 | 60 | 20.9 | 7.4 | 2 | 34.8 | 35.6 | 12 |
| h264 1080p60 high | 60 | 60 | 69.7 | 7.8 | 1.8 | 51.4 | 53 | 13 |
| h264 720p60 alpha | 60 | 60 | 23.2 | 5.2 | 5.3 | 18 | 18.9 | 12 |
| h264 1080p60 alpha | 60 | 60 | 49.7 | 10.9 | 10 | 36.5 | 53 | 14 |
| h264 720p60 | 60 | 60 | 18.6 | 4 | 7.2 | 17.5 | 33.8 | 13 |
| full 720p60 | 60 | 60 | 71 | n/a | n/a | 33.4 | 50.8 | 0 |
| full 1080p60 | 60 | 60 | 201.6 | n/a | n/a | 33.1 | 34.5 | 0 |
| hx 2160p60 mem>bgra | 59 | 59 | 117.6 | 17.2 | 7.6 | 77.7 | 90.2 | 18 |
| hx 2160p60 mem>nv12 | 59 | 59 | 117.4 | 16.8 | 3.1 | 73.1 | 122.8 | 17 |
| hx 2160p60 mem>gpu | 59 | 59 | 117.5 | 17.1 | 0.7 | 55.9 | 64 | 15 |
| hx 2160p60 gpu>bgra | 60 | 60 | 119.5 | 9.1 | 7.8 | 67 | 83.7 | 15 |
| hx 2160p60 gpu>nv12 | 60 | 60 | 119.5 | 9.4 | 3 | 67 | 67.7 | 16 |
| hx 2160p60 gpu>gpu | 60 | 60 | 119.5 | 9.4 | 1 | 50.1 | 83 | 15 |
| hx 2160p60 uyvy>bgra | 60 | 60 | 119.5 | 12 | 7.4 | 69.7 | 86.4 | 19 |
| hx 2160p60 uyvy>nv12 | 60 | 60 | 119.5 | 12.1 | 3 | 69.5 | 85.4 | 17 |
| hx 2160p60 uyvy>gpu | 60 | 60 | 119.6 | 11.8 | 0.6 | 52.6 | 54.1 | 17 |

- **4K60 holds on both ends**, every path, over Wi-Fi: 60 / 60 (59 / 59 with the picture taken from
  memory as BGRA). On one PC on 2026-10-19 it reached 43 / 42. The changes of 2026-11-03 that made it:
  the encoder no longer waits for each packet before taking the next picture (`hevc_amf` 4K measured
  56 fps waiting, 100 fps overlapped); the receiver decodes on the high-performance GPU instead of
  adapter 0; pictures come to memory without waiting on each copy; a receiver that falls behind shows
  the newest picture instead of letting libomt drop coded frames.
- **The GPU the decoder runs on mattered most.** FFmpeg's default is adapter 0, which on the receiver
  PC is the Intel UHD 630. `readback-bench.exe` on that PC, 4K picture to memory:

  | GPU | NV12 (12.4 MB) GPU copy | NV12 CPU read | BGRA (33.2 MB) GPU copy | BGRA CPU read |
  |-----|------------------------|---------------|------------------------|---------------|
  | Intel UHD 630 | 19.0 ms (0.66 GB/s) | 1.7 ms | 12.5 ms (2.65 GB/s) | 3.9 ms |
  | GeForce GTX 1650 | 1.3 ms (9.5 GB/s) | 1.4 ms | 3.0 ms (11.2 GB/s) | 3.9 ms |

  Decoding on the UHD 630, a 4K NV12 receiver held 48 to 50 fps and a BGRA one 26 to 32; on the
  GTX 1650 both hold 60. The decoder now asks Windows for the high-performance GPU and names it in its
  stats (`hevc (d3d11va, NVIDIA GeForce GTX 1650)`).
- **Where the picture enters the sender** costs 7 to 8 ms at 4K: Send ms 9.4 from D3D11 textures, 12
  from UYVY in memory, 16.5 to 17.2 from BGRA in memory (the CPU conversion and the upload). BGRA from
  memory is at the limit at 4K60 (59 fps); a source that has the picture on the GPU or as UYVY should
  send it that way.
- **Where the picture leaves the receiver** costs about one frame at 4K: round trip 50 to 56 ms to a
  texture, 67 to 78 ms to memory. On this GPU the decode and the copy of the newest 4K picture do not
  finish within the half frame the receiver waits for it, so memory receivers run one frame behind
  (with no frame dropped).
- **1080p60 to memory is a frame slower than it needs to be**: 51 ms round trip against 36 ms with
  alpha, which takes the path without the overlapped copy, and 35 ms at `low`. The likely cause, not
  yet confirmed: the receiver falls a frame behind when one copy misses its half-frame wait, and once
  behind it stays behind until the stream pauses. Confirmed and fixed on 2026-11-04 (above).
- **1080p120** receives 115 of 120 fps: the last case that does not hold its rate.
- The round trip does not include the receiving app's own work (OBS adds one full-size copy per frame,
  about 5 to 10 ms at 4K).

## 2026-10-20: alpha by VMX

Same PC, final build of the VMX-alpha change, `scripts\measure.ps1 -Seconds 20`, with the two 1080p60
alpha rows repeated at 60 s. The rows without alpha were close to their 2026-10-19 figures.

| Case | Sender fps | Recv fps | Mbps | Send ms | Receive ms | Delay avg ms | Delay max ms | Discarded |
|------|-----------|----------|------|---------|------------|--------------|--------------|-----------|
| hx 720p60 alpha | 60 | 60 | 16.9 | 5.8 | 4.8 | 11.8 | 16.5 | 27 |
| hx 1080p60 alpha | 60 | 60 | 36.9 | 10.5 | 9.4 | 21.5 | 31.9 | 26 |
| hx 1080p30 alpha | 30 | 30 | 18.4 | 9.7 | 8.6 | 19.8 | 32 | 1 |
| h264 720p60 alpha | 60 | 60 | 22.3 | 6.2 | 4.1 | 11.6 | 25.4 | 26 |
| h264 1080p60 alpha | 60 | 60 | 48.3 | 11.4 | 9.4 | 22.1 | 25.2 | 26 |

- The 60 s repeat held 60 / 60 sent and received on both 1080p60 rows: HEVC 21.5 ms delay (send 10.3,
  receive 9.6), H.264 20.4 ms (send 10.5, receive 8.8). The stacked run's ten-minute fall to 57 fps
  did not appear in this short repeat, but ten minutes were not rerun.
- **Delay** at 1080p60 fell from 26.8 to 21.5 ms (HEVC) and 25.5 to 22.1 ms (H.264) against the stacked
  rows below. Send fell by 3.5 and 2.1 ms, receive by 1.5 and 0.6 ms: the GPU codes 1080p instead of
  2176p, and VMX alpha costs about 1 ms at each end.
- **Bandwidth** rose by 8 to 19%: 31.9 to 36.9 Mbps at 1080p60 HEVC, 44.6 to 48.3 at H.264. VMX is
  intra, so alpha costs 14 to 25 KB every frame; stacked alpha rode inter prediction once the plane
  stopped changing.
- **Matte quality** is lower than stacked. The test suite reads mean alpha error 0.128 end to end, and
  0.086 for the plane alone at medium (VMX quality 70), against 0.030 for stacked alpha.

### Alpha quality: Low, Medium, High

Alpha is not a setting: it always codes at High (VMX quality 90), because the plane is small. The three
qualities were measured before fixing that. Same PC and tools, `-Seconds 20`, 1080p60 alpha, the fill at
medium; only the alpha plane's quality changed. Plane figures are from the test suite's 1080p matte.

| Alpha | VMX quality | Plane bytes/frame | Plane error | HEVC Mbps | H.264 Mbps | HEVC send ms |
|-------|-------------|-------------------|-------------|-----------|------------|--------------|
| Low | 52 | 14,261 | 0.103 | 36.6 | 48.1 | 10.1 |
| Medium | 70 | 14,755 | 0.086 | 36.9 | 48.3 | 10.5 |
| High | 90 | 17,508 | 0.037 | 38.2 | 49.7 | 10.6 |

- High costs 1.6 Mbps more than Low at 1080p60 HEVC (1.6 at H.264) and holds the same 60 / 60, at
  21 to 23 ms delay. Low and Medium are within 0.3 Mbps of each other because VMX's flat-block floor
  dominates a matte; High's larger plane is the one that visibly improves the edge.
- Encode time is unchanged to within 0.5 ms: the fill's GPU code dominates, and alpha runs beside it.
  The cost is bandwidth, not time, so High is the default.

## 2026-10-19: after the latency and CPU work

Same PC, same tools, `scripts\measure.ps1 -Seconds 15`, on the final build of the change. What changed
since 2026-10-13:

- The encoder hands back each picture's packet in the same call. AMF returns it a few ms after the
  picture, and taking only what was ready sent every frame one frame late (16.7 ms at 60 fps, 33 ms
  at 30 fps). The README called this a receiver-held frame; the decoder holds none.
- BGRA <-> NV12 runs on swscale's slice threads (8): 5.2 to 1.2 ms to pack and 4.1 to 1.0 ms to
  unpack a 1080p frame, timed alone.
- The UYVY path (no colour matrix) runs in bands on the thread pool, with a multiply-shift for the
  alpha multiply instead of a divide: 7.3 to 2.1 ms for a 1080p straight-alpha frame, timed alone.
- The periodic keyframe is every 10 s instead of 2 s; receivers still get one within 200 ms of asking.

| Case | Sender fps | Recv fps | Mbps | Send ms | Receive ms | Delay avg ms | Delay max ms | Discarded |
|------|-----------|----------|------|---------|------------|--------------|--------------|-----------|
| hx 720p60 | 60 | 60 | 13 | 4.3 | 5.6 | 9.7 | 26.8 | 27 |
| hx 1080p60 | 60 | 60 | 29.9 | 7.2 | 6.9 | 15.4 | 18.9 | 26 |
| hx 1440p60 | 60 | 60 | 52.7 | 12.4 | 10.6 | 26 | 48.6 | 24 |
| hx 2160p60 | 43 | 42 | 83.6 | 24.2 | 22.8 | 3109 | 4866 | 16 |
| hx 1080p30 | 30 | 30 | 14.9 | 7.1 | 6.8 | 15 | 31.1 | 1 |
| hx 1080p120 | 120 | 120 | 59.8 | 7.3 | 7 | 15.7 | 40.6 | 51 |
| hx 1080p60 low | 60 | 60 | 15 | 7.2 | 6.3 | 14.9 | 21.1 | 27 |
| hx 1080p60 high | 60 | 60 | 49.9 | 7.7 | 8 | 17 | 32 | 27 |
| hx 720p60 alpha | 60 | 60 | 14.2 | 7 | 4.5 | 12.6 | 14.2 | 26 |
| hx 1080p60 alpha | 60 | 60 | 31.9 | 14 | 10.9 | 26.8 | 42.1 | 27 |
| hx 1080p30 alpha | 30 | 30 | 16 | 12.9 | 9.3 | 23.3 | 27.8 | 1 |
| h264 1080p60 | 60 | 60 | 41.8 | 7.6 | 7.2 | 16.1 | 23.9 | 26 |
| h264 1080p60 low | 60 | 60 | 21 | 8.1 | 5.8 | 15.4 | 38.4 | 26 |
| h264 1080p60 high | 60 | 60 | 69.7 | 7.6 | 8.1 | 17.6 | 43 | 27 |
| h264 720p60 alpha | 60 | 60 | 19.8 | 6.7 | 4.1 | 12 | 15.9 | 27 |
| h264 1080p60 alpha | 60 | 60 | 44.6 | 13.5 | 10 | 25.5 | 35.6 | 25 |
| h264 720p60 | 60 | 60 | 18.6 | 4.2 | 3.4 | 9.1 | 35.2 | 26 |
| full 720p60 | 60 | 60 | 71 | n/a | n/a | 4.2 | 43.2 | 0 |
| full 1080p60 | 60 | 60 | 201.6 | n/a | n/a | 8.5 | 11.9 | 0 |

- **Delay** fell by about one frame plus the CPU time saved: 15 ms at 1080p60 (was 42), 10 ms at
  720p60 (was 29), 27 ms at 1080p60 alpha (was 70), 15 ms at 1080p30 (was 58).
- **Ceiling.** 1440p60 and 1080p120 now hold real time on one PC (were 49 and 25 fps received), and
  1080p60 alpha holds 60 / 60 (was 58). 4K reaches about 43 fps (was 28), still short of 60.
- `Send ms` now includes the wait for the packet (about 3 ms at 1080p), which used to fall into the
  next frame's delay instead.

## 2026-10-13 run (before the work above)

- Date: 2026-10-13
- GPU: AMD Radeon RX 6600
- CPU: AMD Ryzen 7 5700X 8-Core Processor
- Encoder: hevc_amf   Decoder: hevc (d3d11va)
- Codec: HEVC (H.265) and H.264 (AVC), HEVC the default; see "H.264 beside HEVC" below
- Run: `scripts\measure.ps1 -Seconds 20`
- The PC was not idle: other programs used 13 to 56% of the CPU, and a remote-desktop program was
  using the same hardware encoder. Figures move by a few ms from run to run.

| Case | Sender fps | Recv fps | Mbps | Send ms | Receive ms | Delay avg ms | Delay max ms | Discarded |
|------|-----------|----------|------|---------|------------|--------------|--------------|-----------|
| hx 720p60 | 60 | 60 | 13.0 | 4.9 | 6.2 | 28.8 | 96.6 | 29 |
| hx 1080p60 | 60 | 60 | 29.8 | 10.6 | 13.2 | 41.6 | 54.6 | 28 |
| hx 1080p60 low | 60 | 60 | 14.9 | 12.1 | 14.4 | 43.8 | 61.6 | 28 |
| hx 1080p60 high | 60 | 60 | 49.8 | 12.0 | 14.9 | 45.5 | 190.8 | 32 |
| hx 1080p30 | 30 | 30 | 14.8 | 11.7 | 13.9 | 57.7 | 110.0 | 2 |
| hx 1080p120 | 101 | 25 | 48.8 | 9.8 | 13.3 | 2529.6 | 3869.3 | 1248 |
| hx 720p60 alpha | 60 | 60 | 14.3 | 7.9 | 8.8 | 34.3 | 40.8 | 27 |
| hx 1080p30 alpha | 30 | 30 | 16.1 | 17.0 | 19.8 | 71.7 | 396.6 | 2 |
| hx 1080p60 alpha | 60 | 58 | 31.8 | 15.7 | 16.8 | 70.0 | 248.6 | 68 |
| hx 1440p60 | 58 | 49 | 51.4 | 16.8 | 19.2 | 1301.8 | 1669.3 | 191 |
| hx 2160p60 | 31 | 28 | 59.8 | 31.3 | 35.0 | 5628.2 | 10040.1 | 42 |
| full 720p60 | 60 | 60 | 71.0 | n/a | n/a | 4.8 | 7.3 | 0 |
| full 1080p60 | 60 | 60 | 201.6 | n/a | n/a | 9.9 | 13.1 | 0 |

Notes:

- `Send ms` is what the tools print as `encode`: the time inside `sendVideo`, which is the CPU
  colour conversion and alpha packing plus the hardware encode call. `Receive ms` is what they print
  as `decode`: hardware decode, the copy to memory, and the CPU conversion back to BGRA. The split is
  in "Where the time goes" below.
- Delay is from the frame's timestamp to the receiver holding the BGRA picture.
- `Discarded` counts the receiver's start-up wait for the first keyframe (about 27 frames at 60 fps)
  plus any frame skipped after that; for the rows that keep up it does not rise after the start.
- Rows where `Sender fps` is below the target (`hx 1080p120`, `hx 1440p60`, `hx 2160p60`) are not
  latency figures. `omthx-send` never skips a late frame, so its timestamps fall behind the clock by
  the frames it could not send: at 31 of 60 fps that is about 9.7 s after 20 s, which is the 10 s in
  the 4K row. Where the receiver is slower than the sender, libomt drops frames and the receiver
  waits for the next keyframe, which is why `Discarded` is high in those rows.

## Where the time goes

Each end timed alone, in one process, paced at the frame rate (a one-off timing program, not in the
repository). Median per frame; the budget at 60 fps is 16.67 ms.

| Stage | 1080p60 | 1080p60 alpha (stacked) |
|-------|---------|--------------------------|
| Sender: pack on the CPU | 6.0 ms | 7.7 ms |
| of which BGRA to NV12 | 4.9 ms | 5.0 ms |
| Sender: hardware encode call | 3.2 ms | 4.9 ms |
| **Sender total** | **9.1 ms** | **12.6 ms** |
| Receiver: hardware decode and copy to memory | 4.0 ms | 7.4 ms |
| Receiver: unpack on the CPU | 4.9 ms | 5.7 ms |
| of which NV12 to BGRA | 4.4 ms | 4.4 ms |
| **Receiver total** | **8.9 ms** | **13.1 ms** |

- The hardware codec is not the limit. Driven by the `ffmpeg` program alone, `hevc_amf` encodes
  1920x1080 at about 320 fps and the 1080p alpha picture (1920x2176) at 195 fps; D3D11 decoding with
  the copy to memory runs at about 520 and 305 fps.
- The largest single cost at each end is the colour conversion, about 5 ms on one CPU core.
- The alpha figures are for straight (not premultiplied) input, which the sender must multiply by
  alpha. When every pixel is partly see-through, as during a fade, the sender total is 13.0 ms.
  Before that multiply was made cheaper (opaque and fully transparent pixels now skip it, and it
  runs on the thread pool), the sender needed 17.5 ms and missed 60 fps by itself.
- In the stacked-alpha table the same stages read higher (15.7 and 16.8 ms for 1080p60 alpha) because
  sender and receiver run at once on one PC. The VMX build codes the fill alone, so only its fill
  column applies; alpha adds about 1 ms at each end (2026-10-20).

## Findings

- **Ceiling.** HX holds 60 fps up to 1080p on this PC. At 1440p the sender reaches 58 fps and the
  receiver 49; at 4K each end needs more than 30 ms per frame, so about 30 fps.
- **Bandwidth** at medium follows the bits-per-pixel target: 13 Mbps at 720p60, 30 at 1080p60. The
  1440p and 4K rows are below their targets (53 and 119 Mbps) by the frames that were not sent.
- **Quality** at 1080p60: low 15, medium 30, high 50 Mbps, at about 42 to 46 ms delay either way.
  Send time does not depend on the bitrate.
- **Frame rate.** 1080p30 works, at 14.8 Mbps and about 58 ms delay: one frame is 33 ms at 30 fps,
  so the delay is higher than at 60 fps. 1080p120 does not: the sender reaches about 101 fps, the
  receiver only 25 fps, and 1248 frames are discarded.
- **Alpha (optional), stacked (2026-10-19).** See 2026-10-20 for the current VMX figures. Alpha is
  off by default, so the rows without "alpha" run without it. Turned on, it coded a picture of double
  height. 720p60 alpha holds 60 fps (14.3 Mbps, 34 ms) and 1080p30
  alpha holds 30 fps (16.1 Mbps). 1080p60 alpha falls just short on one PC: over three 20 s runs it
  read 60 sent / 58 received, 60 / 60 (48 ms delay) and 58 / 55, and over ten minutes the receiver's
  median was 57 fps. The receiver is the slower end, at 17 to 18 ms. Each end alone needs about
  13 ms, so two PCs should hold 60 fps; that is not measured here. See the ten-minute run below.
- **Full mode** has no codec step of ours: 71 Mbps at 720p60 and 201.6 Mbps at 1080p60, at about 5
  to 10 ms delay. `lastEncodeMs`/`lastDecodeMs` are n/a as libomt owns both.

## From the test suite

- Round trip at 1280x720 with stacked alpha: mean alpha error 0.030, mean colour error 0.303 (0 to 255).
- VMX alpha at 1280x720: mean alpha error 0.128 end to end; the plane alone reads 0.103, 0.086 and
  0.037 at qualities 52, 70 and 90, at 14.3, 14.8 and 17.5 KB a frame.
- A receiver joining a running HX source got its first picture after 59 to 63 ms (VMX-alpha build; two
  runs, 2026-10-20).

## Ten-minute run (1080p60 alpha, stacked)

Measured 2026-10-19, before alpha moved to VMX. Sender and receiver on one PC, as in the table above
it.

- It does not hold 60 fps on one PC. The sender's median was 60 fps (68 of 600 seconds below 58);
  the receiver's median was 57 fps (311 of 599 seconds below 58, low 20).
- Frames discarded: 27 at the start (the wait for the first keyframe), 1855 by the end. The receiver
  is the slower end (17.2 ms median against 15.3 ms for the sender), so libomt drops frames and the
  receiver waits for the next keyframe each time.
- Delay: 49 ms in the last minute. It read 1354 ms in the first minute and peaked at 3415 ms, which
  is the test sender's timestamps falling behind while it was late (see the notes above).
- The picture never stopped: the slowest second still showed 20 frames.

## Not measured here

- `hevc_nvenc`: neither sending PC has had an NVIDIA GPU (the two-PC run decodes on one, but encodes on
  the RX 6600). Run the HX rows with an NVIDIA sender before relying on it.
- Two PCs over a wired network: the 2026-11-03 receiver was on Wi-Fi.
- A receiver whose only GPU is an integrated one, with the 2026-11-03 build: the overlapped copy is
  meant for it, but it was measured only on the older build (Intel UHD 630: 48 to 50 fps NV12 at 4K).
- An idle PC. Repeat the table with nothing else running before quoting a figure to the millisecond.
- Picture quality by eye on real camera footage.

## H.264 beside HEVC

Measured 2026-10-13 with `hevc_amf` and `h264_amf`, on the same PC as above, by
`scripts\measure.ps1` with `-Only "60"` for the 1080p60 rows and `-Only "720p60"` for the 720p60
rows (the H.264 rows use `--codec h264`). Each HEVC row sits directly above its H.264 partner.

| Case | Sender fps | Recv fps | Mbps | Send ms | Receive ms | Delay avg ms | Delay max ms | Discarded |
|------|-----------|----------|------|---------|------------|--------------|--------------|-----------|
| hx 720p60 | 60 | 60 | 13.0 | 5.3 | 6.9 | 29.8 | 35.9 | 29 |
| h264 720p60 | 60 | 60 | 18.6 | 5.2 | 6.2 | 29.2 | 33.4 | 28 |
| hx 1080p60 | 60 | 60 | 29.8 | 11.2 | 14.3 | 43.4 | 47.7 | 28 |
| h264 1080p60 | 60 | 60 | 41.8 | 10.5 | 13.3 | 41.4 | 233.9 | 59 |
| hx 1080p60 low | 60 | 60 | 15.0 | 10.9 | 13.7 | 42.3 | 56.9 | 28 |
| h264 1080p60 low | 60 | 60 | 20.8 | 10.6 | 13.0 | 41.2 | 51.6 | 27 |
| hx 1080p60 high | 60 | 60 | 49.8 | 10.5 | 13.9 | 43.4 | 50.1 | 28 |
| h264 1080p60 high | 60 | 60 | 69.4 | 11.3 | 15.5 | 44.1 | 57.7 | 27 |
| hx 720p60 alpha | 60 | 60 | 14.2 | 7.0 | 7.9 | 32.6 | 37.0 | 29 |
| h264 720p60 alpha | 60 | 60 | 19.8 | 7.0 | 7.9 | 33.2 | 50.2 | 28 |
| hx 1080p60 alpha | 60 | 60 | 32.0 | 14.4 | 16.9 | 67.8 | 91.0 | 32 |
| h264 1080p60 alpha | 60 | 60 | 44.7 | 15.2 | 17.1 | 59.1 | 93.8 | 26 |

Notes:

- `h264 1080p60 alpha` held 60 fps received in this run (58 or more). So did `hx 1080p60 alpha`;
  this session was lighter than the ten-minute run above, where HEVC's 1080p60 alpha fell to a
  median of 57 fps.
- At 1080p60 medium the two codecs are close. H.264 encoded in 10.5 ms against HEVC's 11.2 ms, and
  decoded in 13.3 ms against 14.3 ms: about 0.7 ms faster to send and 1.0 ms faster to receive.
- Read past one outlier: `h264 1080p60` shows `Delay max 233.9 ms` and 59 discarded against the
  usual ~28, a single stall in that run, not the steady state the rest of the table shows.
- The alpha stream decoded as `h264 (d3d11va)`. The taller 1920x2176 picture still takes D3D11
  hardware. These alpha rows are stacked; see 2026-10-20 for VMX alpha.

Not measured here: `h264_nvenc` (this PC has no NVIDIA GPU), and picture quality by eye, so the
1.4 bitrate factor for H.264 is still a starting value.
