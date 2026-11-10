# OMT-HX core review, 2026-11-10

Scope: `src/` (the omthx library), `include/omthx/omthx.h`, and the preview routing in the libomtnet fork
(`omt/libomtnet/src/OMTSend.cs`). Upstream libomt and libvmx were not reviewed.

Method: reading the code at commit `10b84ae`. Nothing was built or run for this review, so each finding is
a reading of the code, not a reproduced failure.

## Summary

| # | Finding | Kind | Size of fix | Status |
|---|---------|------|-------------|--------|
| 1 | A failed encode leaves timestamps and frame numbers out of step (no-alpha path) | Bug | Small | Fixed on `fix/core-review-1-4`, uncommitted; no test (needs a failing encoder) |
| 2 | The full stream is coded when every receiver is a preview receiver | Wasted work | Medium (touches the libomtnet fork) | Open |
| 3 | A preview receiver forces a full-size readback of GPU pictures | Wasted work | Medium | Open |
| 4 | `Packer` builds both converters, 8 swscale threads each | Wasted work | Small | Fixed on `fix/core-review-1-4`, uncommitted; full suite passed (98 test cases) |
| 5 | One slow receiver forces keyframes for every receiver | Behaviour under load | Small (back-off) to large (bitrate) | Back-off done (200 ms doubling to 2 s, joins and app requests exempt), uncommitted; bitrate half open |
| 6 | Software decode is single-threaded when a GPU exists but cannot decode the codec | Behaviour under load | Small to medium | Fallback to a software `Decoder` done, uncommitted; may not add threads, see note in section 6 |
| 7 | Fallbacks are silent and permanent | Operability | Small | Reason now in `lastFallback` (`fix/core-review-1-4`, uncommitted); flags still latch, no retry |
| 8 | The UYVY GPU path uses the default adapter, the decoder the high-performance one | Operability | Small | Fixed (high-performance adapter, default as fallback); not verified on a hybrid-GPU PC |
| 9 | Smaller points (stats, double gate call, preview switching, audio) | Minor | Small each | Timing stats fixed; gate steps once on Hx paths, still twice on two Full paths; preview switching and audio open |

## 1. A failed encode leaves the stream out of step (bug)

`Encoder::send` (`src/encoder.cpp:186-191`) first collects the previous picture's late packet into a local
vector, then calls `avcodec_send_frame`. If that call fails, `send` throws and the collected packet is lost.

In `Sender::Impl::sendPicture` (`src/link.cpp:244`) the alpha path catches the throw and resets both
encoders. The plain path (`src/link.cpp:287`) does not, so afterwards:

- The lost packet's timestamp stays in `stamps`. Every later packet is sent with the previous picture's
  timestamp, until the format changes.
- The lost access unit never received a `seq` number. Receivers see consecutive numbers, do not ask for a
  keyframe, and decode against a missing reference.
- The encoder stays open in whatever state the failure left it.

Fix: one `try/catch` around the encode in both branches that resets `encoder` and `alphaEncoder` and
rethrows. `openEncoder` then reopens on the next frame, clears `stamps` and `alphas`, and starts with a
keyframe.

## 2. The full stream is coded when nobody receives it

libomtnet sends a full HX frame only to non-preview channels while a preview stream runs
(`omt/libomtnet/src/OMTSend.cs:532`). `sendPicture` codes the full stream whenever any connection exists.
With a multiview where every receiver asked for preview, each camera spends one GPU encode session and one
colour conversion on frames that go nowhere.

Fix: add a count of full video connections to the fork beside `PreviewConnections`, export it through
libomt, and skip the full encode when it is zero. `omt_send_connections` cannot serve: it also counts audio
connections. The first full receiver to connect must get a keyframe, as after an idle spell.

## 3. A preview receiver breaks the zero-copy GPU path

For a GPU picture, `sendPreview` copies the full-size picture to memory to shrink it to 640 wide
(`src/link.cpp:355`). `docs/MEASUREMENTS.md` and the comments in `link.cpp` put a 4K readback at about 30 ms
on the GTX 1650, so one multiview viewer can take a 4K `sendVideoD3D11` or relay sender below 60 fps.

Fix: let the D3D11 video processor write a second, 640-wide NV12 target. It scales in the same blit, and
only the small picture is read back, or it is coded where it is.

## 4. `Packer` builds both converters

`Packer::ensure` (`src/pure.cpp:419-428`) creates BGRA to NV12 and NV12 to BGRA together, each with 8
swscale slice threads. A sender uses only the first and a receiver only the second.

Fix: create each converter on first use, keyed on the size. Idle threads per object halve.

## 5. One slow receiver costs every receiver quality

`KeyframePolicy::next` (`src/pure.cpp:146-157`) forces a keyframe, at most every 200 ms, whenever libomt's
`FramesDropped` rises. The count covers all receivers, so one receiver on a slow link gives everyone up to
five keyframes a second, each a quality dip on the two-frame rate buffer. The slow receiver also asks for a
keyframe itself when it sees the `seq` gap, so the sender-side trigger is a second request for the same
event.

Fix, small: back the drop-triggered keyframes off (200 ms, 400 ms, up to about 2 s) while drops continue,
and reset the interval after a quiet spell. Receiver requests keep the 200 ms limit.

Fix, larger: treat repeated drops as congestion and reopen the encoder at a lower bitrate, recovering
slowly. This is a design change and needs measurements.

## 6. Single-threaded software decode on a GPU without the codec

`Decoder::Decoder` (`src/decoder.cpp:77-88`) sets `thread_count = 4` only when no D3D11 device can be
created. On a GPU that has D3D11 but no hardware decoder for the codec, FFmpeg falls back to software inside
the same context with the default thread count, which is unlikely to hold 1080p60 HEVC.

Fix: when the first picture is not `AV_PIX_FMT_D3D11`, reopen the decoder as software with threads and wait
for the next keyframe.

Note after the fix: `Decoder` sets `AV_CODEC_FLAG_LOW_DELAY`, and FFmpeg turns frame threading off under
that flag (`libavcodec/pthread.c`, FFmpeg n9.0.2). `thread_count = 4` then gives only slice threads, which
help only streams coded in several slices or, for HEVC, with WPP. The software decoder, old and new, may
therefore decode a single-slice hardware stream on one thread. The code comment about "four frame threads"
is likely wrong. Needs a measurement; the real choice is frame threads (several frames of delay) against
low delay. Decided: the software decoder drops `LOW_DELAY` and runs four frame threads (commit `2b8d06e`);
the hardware decoder keeps it. The extra delay and the throughput are not measured yet.

## 7. Fallbacks are silent and permanent

`gpuRefused`, `uyvyRefused`, `readbackFailed`, `gpuBgraFailed` and `previewRefused` each latch for the
object's lifetime after one failure. The reason is discarded, including the encoder's `tried` string, which
names every encoder attempted and why it refused.

Fix: a `lastFallback` string in `SenderStats` and `ReceiverStats`, set where each flag is set. Retrying
after a delay is optional.

## 8. Two different GPUs on a hybrid PC

`UyvyToNv12` makes its device with `D3D11CreateDevice(nullptr, ...)` (`src/d3d11.cpp:240`), the default
adapter. `Decoder` picks the adapter Windows ranks first for performance (`src/decoder.cpp:37`). On a laptop
with an integrated and a discrete GPU the UYVY path may open its encoder on the integrated GPU, or on a
weaker encoder, where the memory path would have picked NVENC.

Fix: share `highPerformanceAdapter` and pass that adapter to `D3D11CreateDevice`. Needs a hybrid-GPU PC to
confirm the problem and the fix.

## 9. Smaller points

- `lastEncodeMs` and `roundTripMs` leave out the conversion in `sendVideoUyvy` (GPU path) and
  `sendVideoD3D11`: `t0` is taken in `sendVideo(frame)`, after it.
- `lastDecodeMs` reads about 0 for a picture `Readback` delivers one call late (`src/link.cpp:885`).
- `sendVideoUyvy` (GPU path) and `sendVideoD3D11` call `skipVideo()`, and `sendVideo(frame)` calls it again.
  Harmless today; the idle gate steps twice per frame.
- Preview is fixed when a `Receiver` is made. libomt has `omt_receive_setflags`, so a `setPreview(bool)`
  would let a multiview tile go to program without reconnecting.
- FPA1 audio is uncompressed float: about 3 Mbps for stereo at 48 kHz, beside a 13 Mbps 720p HX stream.

## What held up

Loss handling (`seq` numbering, the keyframe wait, alpha queued by timestamp), the checks of decoded
pictures against their frame headers, and libvmx's own size limits on the alpha plane were traced and no
fault was found.

## Suggested order

1. Findings 1 and 4: small, local, covered by the existing tests.
2. Findings 7 and 9 (stats and the double gate call).
3. Findings 2, 5 (back-off) and 6.
4. Findings 3 and 8, and the bitrate half of 5: each needs measurements or other hardware.
