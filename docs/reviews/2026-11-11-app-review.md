# How the apps use OMT-HX: review, 2026-11-11

Scope: BreadCamera, BreadStudio, BreadReplay, OMT Camera (`opencamera-omthx`) and Bread OMT Screen
(`openscreenshare-omthx`), read against OMT-HX at `fix/core-review-1-4` (3b55d2c). The code was read, not run:
no finding here was reproduced, and the phone findings were not checked on a device.

Status column: **queued** = in tonight's OpenCode queue (`.superpowers/night-2026-11-11/`), **deferred** = not
queued, with the reason.

## Findings

| Id | App | Severity | Finding | Status |
|---|---|---|---|---|
| S1 | BreadStudio | High | An OMT-HX exception ends the program | queued |
| C1 | BreadCamera, BreadReplay | High | A mono source is read past its audio buffer | queued |
| H1 | OMT-HX, phones | High (unverified) | Limited-range phone video is shown as full range | receiver done; phones queued |
| P1 | OMT Camera | Medium | Keyframe requests are never answered | queued |
| C2 | BreadCamera | Low | Audio with zero channels divides by zero | queued |
| C3 | BreadCamera | Medium | A refused GPU path never comes back | queued |
| C4 | BreadCamera | Low | The library's fallbacks and lowered bitrate are not shown | queued |
| C5 | BreadCamera | Low | A comment about audio blocks is out of date | queued |
| S2 | BreadStudio | Medium | The clock offset is set once and never corrected | queued |
| S3 | BreadStudio | Low | The output converts pictures with nobody connected | queued |
| S4 | BreadStudio | Low | The audio callback allocates on every call | queued |
| S5 | BreadStudio | Low | The BGRA path assumes rows of width × 4 bytes | queued |
| S6 | BreadStudio | Low | Fallbacks are not logged | queued |
| P2 | Both phone apps | Low | Every keyframe request is answered at once | queued |
| P3 | Both phone apps | Low | A buffer is allocated for every keyframe | queued |
| R1 | BreadReplay | Medium | A recorded HX source is asked for a keyframe every 0.5 s | deferred |
| S7 | BreadStudio | Idea | Sources off program could take the preview stream | deferred |
| S8 | BreadStudio | Idea | Quality is fixed at High; NV12 receive would be cheaper | deferred |
| C6 | BreadCamera | Idea | Decoded pictures go to memory and back to the GPU | deferred |
| R2 | BreadReplay | Note | HX with alpha is recorded as fill only | deferred |
| P4 | Both phone apps | Idea | No preview stream, no encoder name in the metadata | deferred |

## Details

**S1.** `omtOutputRawVideo` and `omtOutputRawAudio` (`breadstudio/omt-hx/omt-output.cpp`) call
`sendVideo`, `sendVideoNv12`, `sendVideoUyvy` and `sendAudio` with no `try`. `worker_main`
(`omt-hx.cpp`) calls `receiveVideo` and `receiveAudio` the same way, on a `std::thread`. omthx throws
`std::runtime_error` when no encoder or decoder can be opened. The first case crosses OBS's C code, the second
leaves a thread: both end the process. BreadCamera wraps the same calls.

**C1.** `omt_input.cpp` accepts audio with `channels >= 1` and then always reads the second channel at
`a.planar[a.samples + i]`. With one channel that is past the end of the buffer. BreadReplay records through this
input.

**H1.** `OMTHXEncoder.java` in both phone apps sets no `KEY_COLOR_RANGE`, `KEY_COLOR_STANDARD` or
`KEY_COLOR_TRANSFER`; Android encoders then code limited range (16–235). The OMT-HX receiver converts every HX
picture as full-range BT.709 and never reads the range the stream signals. Expected result: grey blacks and dull
whites from a phone. The fix is in two places: the receiver honours the stream's range, and the phones signal
BT.709 limited range explicitly. The phones' frame header also says BT.601 below 720p; the receiver ignores it.
The receiver half is done on branch `opencode/receiver-colour-range`: a picture is full range only when its
stream says so, and Bgra, Nv12, Gpu and `FrameDecoder` outputs all come out full range. It has not been checked
on a phone. The matrix stays BT.709 for every stream, so a stream whose header says BT.601 is still converted as
BT.709.

**P1.** OMT Camera's `libomt/cpp` has no `omt_send_take_keyframe_request` and its encoder has no
`requestKeyframe`: a receiver that joins or loses a frame waits for the next periodic keyframe (1 s). Bread OMT
Screen has both (its commit 9757e6d).

**C2.** `omt_output.cpp`: `n = size / ch` with `ch == 0`.

**C3.** `OmthOutput::gpuRefused_` is set by the first failed `sendVideoD3D11` and never cleared, even after
the output is closed and opened again or the picture changes size. The library now retries its own encoder
fallbacks; what still reaches the app is a property of the picture or the device, so the right moments to try
again are a new size and a new `open()`.

**C4.** `SenderStats::lastFallback` and `bitratePercent` are not read. An operator cannot see that the sender
went to a slower path or lowered its bitrate. `OutputStatus::note` exists for this.

**S2.** `worker_main` computes `base` from the first frame and keeps it. A sender that restarts with a new time
base (BreadCamera counts ticks from zero) leaves every later timestamp off by the difference.

**S3–S5.** `omtOutputRawVideo` runs `video_scaler_scale` before the sender can skip the frame;
`omtOutputRawAudio` builds a `std::vector` per call; the BGRA span is `w * h * 4` bytes from `data[0]`
whatever `linesize[0]` is.

**P2.** Bread OMT Screen calls `enc.requestKeyframe()` for every request it takes. Several receivers losing
frames together cause a run of keyframes. The Windows sender allows one per 200 ms.

**P3.** `OMTHXEncoder` allocates a direct `ByteBuffer` for each keyframe when it prepends the headers.

## Deferred, and why

- **R1** changes how finely a replay can seek. That is a product choice for BreadReplay, not a bug fix: decide
  the interval, or decode from the keyframe before the seek point.
- **S7** would cut to a 640-wide picture for a moment when a source goes on program, and show a low picture in
  projectors of scenes that are not on program. It needs a design and a setting.
- **S8, C6, P4** are features, each with its own design. P4's preview stream needs a second encoder on the phone.
- **R2** looks deliberate.

## What held up

- BreadCamera's input reconnects, reports errors and switches between coded and decoded modes soundly.
- Bread OMT Screen numbers its frames the way the receiver expects.
- The phones' native bridge checks every buffer length before reading.

## Not verified

- H1 on a phone: send from each app, `omthx-recv --dump`, and compare black and white levels.
- S1 in OBS: start the OMT output with no encoder available (or a source whose decoder cannot open).
