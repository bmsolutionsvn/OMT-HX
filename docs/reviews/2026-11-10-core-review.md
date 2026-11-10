# OMT-HX core review, 2026-11-10

Scope: `src/` (the omthx library), `include/omthx/omthx.h`, and the preview routing in the libomtnet fork
(`omt/libomtnet/src/OMTSend.cs`). Upstream libomt and libvmx were not reviewed.

Method: reading the code at commit `10b84ae`. Nothing was built or run for this review, so each finding is
a reading of the code, not a reproduced failure.

## Summary

| # | Finding | Kind | Size of fix | Status |
|---|---------|------|-------------|--------|
| 1 | A failed encode leaves timestamps and frame numbers out of step (no-alpha path) | Bug | Small | Fixed on `fix/core-review-1-4`; no test (needs a failing encoder) |
| 2 | The full stream is coded when every receiver is a preview receiver | Wasted work | Medium (touches the libomtnet fork) | Fixed: `omt_send_fullconnections`; the full encoder closes while only preview receivers watch |
| 3 | A preview receiver forces a full-size readback of GPU pictures | Wasted work | Medium | Fixed: shrunk by the D3D11 video processor and coded in video memory; 4K send 9.5 to 5.5 ms on the RX 6600 |
| 4 | `Packer` builds both converters, 8 swscale threads each | Wasted work | Small | Fixed on `fix/core-review-1-4`; full suite passed (98 test cases) |
| 5 | One slow receiver forces keyframes for every receiver | Behaviour under load | Small (back-off) to large (bitrate) | Back-off done (200 ms doubling to 2 s, joins and app requests exempt); and a `BitrateGovernor` steps the full stream down to 40 % under sustained drops and back up, see section 5 |
| 6 | Software decode is single-threaded when a GPU exists but cannot decode the codec | Behaviour under load | Small to medium | Fallback to a software `Decoder` done; measured: four frame threads hold 1080p60 at 60 fps and 2160p30 at 30 fps, three frames (46 ms) of extra delay; `LOW_DELAY` kept gives 6 fps, see section 6 and `docs/MEASUREMENTS.md` |
| 7 | Fallbacks are silent and permanent | Operability | Small | Reason now in `lastFallback` (`fix/core-review-1-4`); each path is retried later, after a 30 s delay that doubles to 10 min (`opencode/fallback-retry`). A real recovery is untested |
| 8 | The UYVY GPU path uses the default adapter, the decoder the high-performance one | Operability | Small | Fixed (high-performance adapter, default as fallback); not verified on a hybrid-GPU PC |
| 9 | Smaller points (stats, double gate call, preview switching, audio) | Minor | Small each | Timing stats fixed; the gate steps once per frame on every path; `Receiver::setPreview` / `omthx_c_receiver_preview` switch without reconnecting; compressed audio dropped (OMT defines FPA1 only, so plain receivers would break) |

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

Done (`fix/core-review-1-4`), coded where it is:

- `GpuShrinker` (`src/d3d11.cpp`, declared in `src/codec.h`) scales the top-left fill of an NV12 D3D11 frame
  (a decoder's array slice, or a `BgraToNv12` / `UyvyToNv12` frame) to the preview size into an NV12 frame
  pool on the same FFmpeg device, so it takes that device's lock, the one the decoder and encoders take. It
  reuses `VideoConverter`, which now takes an output size, an input array slice and an optional shared FFmpeg
  device; it is rebuilt when the device or either size changes. Both sides are full-range BT.709 YCbCr.
- `sendPreview` codes that frame with a GPU `Encoder` on the pool (reopened when the pool changes) and names
  it `<encoder> (d3d11)` in the preview metadata. It works with no full encoder open (preview receivers
  only). Any failure latches `previewGpuRefused`, sets `lastFallback` to `preview on the gpu: <reason>`, and
  the frame and later ones take the old path (full readback, `Shrinker`, CPU-memory encoder). A failure there
  still only stops the preview, as before. No middle path that reads back only the small picture: a GPU
  whose encoder refuses D3D11 frames already has the picture in memory (`gpuRefused`).
- Measured on the RX 6600 (hevc_amf), 3840x2160 `sendVideoD3D11` with one preview receiver and no full
  receiver, 60 frames, three runs each: old path 9.2 to 9.7 ms per call (5 to 6 ms CPU), new path 5.4 to
  5.7 ms (2.6 to 3.4 ms CPU; thread CPU times are coarse). Most of what remains is the encoder's wait for
  its own packet. The RX 6600 reads back far faster than the GTX 1650's 30 ms, so the gain there should be
  larger; not measured on the GTX 1650.
- Tests (`tests/link_test.cpp`): "a 4K texture's preview is shrunk and coded in video memory" (preview size,
  `(d3d11)` encoder, no fallback, mean colour error 0.64 of 255 against the gradient) and "a Gpu receiver's
  pictures get their preview shrunk in video memory" (decoder array slices). The fallback path is not
  exercised by a test: nothing on this PC makes the video processor or the GPU encoder refuse.

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

Done (`opencode/core-review-remaining`): `BitrateGovernor` (`src/codec.h`, `src/pure.cpp`) takes libomt's
cumulative `FramesDropped` once per coded frame while the full encoder is open. A single burst changes
nothing: drops must go on for a second before the first step. It then steps the bitrate down by a
quarter, at most once a second, to 40 %, and reopens the encoder on the next frame at the new rate; the
reopen starts with a keyframe, which the slow receiver needs anyway. Two seconds without a drop end the
spell, and ten seconds with no drop and no change step the bitrate back up by 20 %. A drop count that
goes down is a new baseline. `SenderStats::bitratePercent` reports the current percentage as its last
field; a resumed sender starts at 100 % again. The preview encoder and the VMX alpha encoder are not
changed. Six pure tests (`tests/pure_test.cpp`) cover the rules. Only a healthy link is checked
end-to-end (`tests/link_test.cpp`, `bitratePercent` stays 100 over 2 s): a receiver that connects and
never calls `receiveVideo` makes libomt count no send-side drops, 0 after 20 s, because its receive
thread keeps draining the socket, and the drops it counts are the receiver's own statistics. So the
reopen-under-drops path has no end-to-end test.

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
the hardware decoder keeps it.

Measured (`docs/MEASUREMENTS.md`, 2026-11-10, one PC, RX 6600, 25 s runs): the hardware decoder gives a
median video delay of 14.7 ms at 1080p60 HEVC; the software decoder with four frame threads gives 60.7 ms
and holds 60 fps (1.6 cores). Keeping `LOW_DELAY` instead, slice threads only, drops the same stream to
6 to 7 fps with 117 pictures discarded: the AMF stream is single-slice, so slice threads cannot help, and
the "four frame threads" comment was indeed wrong about the mechanism but right about the need. The frame
threads cost three frames, about 46 ms, and are the only setting that keeps up, so the decoder is left
alone. The software path also holds 2160p30 HEVC (30 fps, 163 ms, 3.5 cores) and collapses at 2160p60
(4 fps, 1845 ms).

## 7. Fallbacks are silent and permanent

`gpuRefused`, `uyvyRefused`, `previewRefused`, `previewGpuRefused`, `readbackFailed` and `gpuBgraFailed` once each
latched for the object's lifetime after one failure. The reason is discarded, including the encoder's `tried`
string, which names every encoder attempted and why it refused.

Fix (done): a `lastFallback` string in `SenderStats` and `ReceiverStats`, set where each gate fails. Each fallback
is also retried: a `RetryGate` (`src/codec.h`) closes the fast path for a delay that starts at 30 s and doubles with
every failure in a row, up to 10 min, and the delay goes back to 30 s when the path next delivers a frame. The
fallback still runs in the same frame, so a failed retry loses nothing, with one exception: while every receiver
is a preview receiver, a failed preview retry drops one frame, as the full encoder was closed for it.

A real recovery is untested: no test PC can make a GPU path fail and then recover, and there is no environment hook
for it. The gate itself is unit-tested (`tests/pure_test.cpp`), and `OMTHX_TEST_GPU_REFUSED` still latches.

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
