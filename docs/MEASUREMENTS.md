# OMT-HX measurements

Measured with `omthx-send` and `omthx-recv`, driven by `scripts\measure.ps1`, on one PC over
`localhost`, test picture with grain. One PC means no network in the path: these are codec and
library figures, not link figures. It also means sender and receiver share one CPU and one GPU, so
each runs slower than it would alone. Each row is the median of a 20 s run after a 2 s warm-up.

- Date: 2026-10-13
- GPU: AMD Radeon RX 6600
- CPU: AMD Ryzen 7 5700X 8-Core Processor
- Encoder: hevc_amf   Decoder: hevc (d3d11va)
- Codec: HEVC (H.265) in version 1; H.264 is under research
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

| Stage | 1080p60 | 1080p60 alpha |
|-------|---------|---------------|
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
- In the table at the top the same stages read higher (15.7 and 16.8 ms for 1080p60 alpha) because
  sender and receiver run at once on one PC.

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
- **Alpha (optional).** Alpha is off by default, so the rows without "alpha" run without it. Turned
  on, it codes a picture of double height. 720p60 alpha holds 60 fps (14.3 Mbps, 34 ms) and 1080p30
  alpha holds 30 fps (16.1 Mbps). 1080p60 alpha falls just short on one PC: over three 20 s runs it
  read 60 sent / 58 received, 60 / 60 (48 ms delay) and 58 / 55, and over ten minutes the receiver's
  median was 57 fps. The receiver is the slower end, at 17 to 18 ms. Each end alone needs about
  13 ms, so two PCs should hold 60 fps; that is not measured here. See the ten-minute run below.
- **Full mode** has no codec step of ours: 71 Mbps at 720p60 and 201.6 Mbps at 1080p60, at about 5
  to 10 ms delay. `lastEncodeMs`/`lastDecodeMs` are n/a as libomt owns both.

## From the test suite

- Round trip at 1280x720 with alpha: mean alpha error 0.030, mean colour error 0.303 (0 to 255).
- A receiver joining a running HX source got its first picture after 93 to 128 ms (two runs).

## Ten-minute run (1080p60 alpha)

Sender and receiver on one PC, as in the table at the top.

- It does not hold 60 fps on one PC. The sender's median was 60 fps (68 of 600 seconds below 58);
  the receiver's median was 57 fps (311 of 599 seconds below 58, low 20).
- Frames discarded: 27 at the start (the wait for the first keyframe), 1855 by the end. The receiver
  is the slower end (17.2 ms median against 15.3 ms for the sender), so libomt drops frames and the
  receiver waits for the next keyframe each time.
- Delay: 49 ms in the last minute. It read 1354 ms in the first minute and peaked at 3415 ms, which
  is the test sender's timestamps falling behind while it was late (see the notes above).
- The picture never stopped: the slowest second still showed 20 frames.

## Not measured here

- `hevc_nvenc`: this PC has no NVIDIA GPU. Run the HX rows on an NVIDIA PC before relying on it.
- Two PCs over a real network. This matters most for 1080p60 with alpha, where one PC is the limit.
- An idle PC. Repeat the table with nothing else running before quoting a figure to the millisecond.
- Picture quality by eye on real camera footage.

## H.264 beside HEVC

Measured 2026-10-13 with `hevc_amf` and `h264_amf` in one session, on the same PC as above, by
`scripts\measure.ps1 -Only "60"` (the H.264 rows use `--codec h264`). Each HEVC row sits directly
above its H.264 partner.

| Case | Sender fps | Recv fps | Mbps | Send ms | Receive ms | Delay avg ms | Delay max ms | Discarded |
|------|-----------|----------|------|---------|------------|--------------|--------------|-----------|
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
- The alpha stream decoded as `h264 (d3d11va)`. The taller 1920x2176 picture still takes D3D11
  hardware.

Not measured here: `h264_nvenc` (this PC has no NVIDIA GPU), and picture quality by eye, so the
1.4 bitrate factor for H.264 is still a starting value.
