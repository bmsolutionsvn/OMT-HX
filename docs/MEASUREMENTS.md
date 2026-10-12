# OMT-HX measurements

Measured with `omthx-send` and `omthx-recv` on one PC over `localhost`, 1920x1080 at 60 fps,
test picture with grain. One PC means no network in the path: these are codec and library
figures, not link figures. Delay is from the frame's timestamp to the receiver holding the BGRA
picture.

- Date: 2026-10-13
- GPU: AMD Radeon RX 6600
- CPU: AMD Ryzen 7 5700X 8-Core Processor
- Encoder: hevc_amf   Decoder: hevc (d3d11va)
- Commit: 52b7c2f

| # | Mode | Alpha | Quality | Mbps | Encode ms | Decode ms | Delay avg ms | Delay max ms |
|---|------|-------|---------|------|-----------|-----------|--------------|--------------|
| 1 | hx | no | low | 14.95 | 9.2 | 11.7 | 38.5 | 42 |
| 2 | hx | no | medium | 29.9 | 9.0 | 12.7 | 40 | 44 |
| 3 | hx | yes | medium | 29.2 | 18.0 | 17.0 | 423 to 3118 | 3155 |
| 4 | hx | no | high | 49.9 | 9.5 | 13.0 | 40 | 44 |
| 5 | full | no | medium | 201.6 | n/a | n/a | 8.7 | 10.5 |
| 6 | full | yes | medium | 207.1 | n/a | n/a | 9.0 | 11.3 |

Notes:

- Rows 1, 2 and 4 are steady: the receiver holds a stable one-frame delay near 40 ms.
- Row 3 (alpha) runs below 60 fps: the coded picture is 1920x2176, so encode (about 18 ms) and
  decode (about 17 ms) cannot keep a 60 fps budget, and the receiver's queue grows without bound
  over the run (the delay figure climbs from 423 ms to 3118 ms across the 30 seconds). The Mbps,
  encode and decode figures are steady; only the delay drifts.
- Row 2 saw one transient stall mid-run (a few seconds near 20 fps, delay spiking to about
  1700 ms) before recovering; the figures above are the steady state after it.
- Full mode carries the whole BGRA picture (about 200 Mbps) because libomt encodes VMX with no
  lossy step; `lastEncodeMs`/`lastDecodeMs` are n/a as libomt owns both.

## From the test suite

- Round trip at 1280x720 with alpha: mean alpha error 0.030, mean colour error 0.303 (0 to 255).
- A receiver joining a running HX source got its first picture after 76 ms.

## Ten-minute run (row 3)

- Frames discarded after the first second: none. `discarded` stayed at 21 (the startup wait for
  the first keyframe) for the whole run and did not rise.
- Seconds below 58 fps: 599 of 600 (median 54 fps, low 7 fps). The alpha stream cannot hold
  60 fps on this PC, so the receiver backlog grows steadily.

## Not measured here

- `hevc_nvenc`: this PC has no NVIDIA GPU. Run rows 1 to 4 on an NVIDIA PC before relying on it.
- Two PCs over a real network.
- Picture quality by eye on real camera footage.