# Core review, remaining items: software decoder measurement and bitrate adaptation

Base branch: `fix/core-review-1-4`. Review: `docs/reviews/2026-11-10-core-review.md` (findings 5 and 6).
Names are stable, line numbers are not: find every symbol with `git grep` before editing.

## Spec (binding)

### A. Measure the software decoder (finding 6)

The software `Decoder` (`src/decoder.cpp`, the `else` branch after `av_hwdevice_ctx_create`) drops
`AV_CODEC_FLAG_LOW_DELAY` and sets `thread_count = 4`, so FFmpeg runs four frame threads. Frame threads add delay.
Nobody has measured how much, or whether the old setting (LOW_DELAY kept, so slice threads only) kept up.

Measure on this PC (AMD RX 6600, Windows), with `build/release/omthx-send.exe` and `omthx-recv.exe` on the same PC:

- 1080p60 HEVC and 1080p60 H.264, Quality Medium, `--tick` on both tools (see each tool's usage text).
- For each: the hardware decoder (default), and the software decoder forced with the environment variable
  `OMTHX_TEST_GPU_DECODE_REFUSED=1` set for `omthx-recv` only (the receiver then falls back to the software
  decoder at the next keyframe; its stats name the decoder "... (software)": check that it did).
- Also once with 2160p30 HEVC, software decoder, to find where it stops keeping up.
- Record: median video delay from `--tick`, pictures received per second, the decoder name, and CPU use of
  `omthx-recv` (Task Manager is not available; use `Get-Process omthx-recv | Select CPU` before and after, or
  `typeperf`), each run at least 20 s.
- Comparison run, not committed: temporarily keep `AV_CODEC_FLAG_LOW_DELAY` in the software branch (delete the
  `ctx_->flags &= ~AV_CODEC_FLAG_LOW_DELAY;` line), rebuild, measure 1080p60 HEVC software again, then restore the
  file with `git checkout -- src/decoder.cpp` and rebuild. Never commit the temporary change.

Write the results as a new section at the top of the dated sections of `docs/MEASUREMENTS.md`
("## 2026-11-10: the software decoder"), as a table plus two or three plain sentences on what it means. Update
section 6 and the finding 6 row of the review with the measured numbers. If the measurements show frame threads
are worse than LOW_DELAY (more delay and no throughput gain), say so in the docs and record a `Ruling:`, but do not
change the decoder: that decision goes back to the user.

Done when: the table has every run above, the review's section 6 no longer says "not measured", and the decoder
source is unchanged.

### B. Bitrate adaptation (second half of finding 5)

When libomt keeps dropping frames for a slow receiver (`OMTStatistics::FramesDropped` from
`omt_send_getvideostatistics`, cumulative over all receivers), an Hx sender lowers its full stream's bitrate in
steps, and raises it again slowly once the drops stop.

Pure logic, in `src/codec.h` / `src/pure.cpp` beside `KeyframePolicy`:

```cpp
// Lowers an Hx sender's bitrate while libomt keeps dropping frames, and raises it again once they stop.
class BitrateGovernor {
 public:
  static constexpr int kFloorPercent = 40;
  static constexpr int64_t kTroubleMs = 1'000;   // drops must go on this long before the first step down
  static constexpr int64_t kStepMs = 1'000;      // at most one step down per second
  static constexpr int64_t kQuietMs = 2'000;     // no drops this long ends a spell of trouble
  static constexpr int64_t kRecoverMs = 10'000;  // no drops and no change this long: one step up
  // Call once per coded frame with libomt's cumulative drop count. True when percent() changed.
  bool next(int64_t nowMs, uint64_t dropped);
  int percent() const { return percent_; }
  ...
};
```

Rules:
- The first call only records the drop count; it never changes the percent.
- Drops rising starts a spell of trouble (if none is running) at that time. The spell ends after `kQuietMs`
  without a rise.
- Step down: drops rose on this call, the spell has run at least `kTroubleMs`, and the last change was at least
  `kStepMs` ago: `percent = max(kFloorPercent, percent * 3 / 4)`. A single burst of drops (shorter than
  `kTroubleMs`) changes nothing.
- Step up: no rise for `kRecoverMs` and no change for `kRecoverMs`: `percent = min(100, percent + 20)`.
- A drop count that goes down (libomt restarted its count) is taken as the new baseline, with no step.

Wiring, in `src/link.cpp` (`Sender::Impl`):
- One `BitrateGovernor governor;` member. In `sendPicture`, after `omt_send_getvideostatistics`, call
  `governor.next(nowMs, FramesDropped)` while a full encoder is open. When it returns true, reset `encoder` and
  `alphaEncoder` so `openEncoder` reopens on the next frame (a reopened encoder starts with a keyframe, which the
  slow receiver needs anyway). Do not reopen in the middle of `sendPicture`: the change applies from the next frame.
- `openEncoder` codes the fill at `bitrate * governor.percent() / 100`, for both `fillBitrate` and
  `targetBitrate`. The VMX alpha encoder and the preview encoder are not changed.
- Reset the governor to a fresh one (100 %) where `skipVideo` handles `IdleGate::Step::Resume`.
- `SenderStats` gains `int bitratePercent = 100;` as its **last** field (after `lastFallback`), filled under
  `statsMutex` when it changes, so an app can show it.

Tests:
- `tests/pure_test.cpp`: one test per rule above (first call, burst, sustained trouble steps down once per second
  to the floor, quiet ends the spell, recovery steps up to 100, count going down).
- `tests/link_test.cpp`: a healthy Hx link keeps `stats().bitratePercent == 100` over 2 s. Then try to make real
  drops: a receiver that connects and never calls `receiveVideo` while a 1080p60 sender runs for 5 s. If libomt
  counts drops for it (`stats().framesDropped > 0`), check `bitratePercent < 100`. If it does not, keep only the
  healthy-link check and record a `Ruling:`. Do not add an environment-variable test hook for this.

Anti-goals: no dynamic bitrate reconfiguration inside FFmpeg (reopen only), no per-receiver bitrate, no change to
`KeyframePolicy`, no change to the C API (`omthx_c.h`), no change to the submodules under `omt/`.

Done when: the pure tests and the link test pass, the full suite passes, and the finding 5 row and section 5 of the
review say what was done.

## Tasks

### Task 1: software decoder measurement

1. `scripts/build.ps1 -Preset release` (see the test rules in the brief for how to call it).
2. Find the sender's source name with `omthx-recv --list` while `omthx-send` runs.
3. Run every measurement in spec A. Restore `src/decoder.cpp` after the comparison run and check `git status` shows
   it unchanged.
4. Write `docs/MEASUREMENTS.md` and the review update.
5. Commit: `docs: measure the software decoder's delay and throughput`.

### Task 2: BitrateGovernor (pure)

1. Write the pure tests in `tests/pure_test.cpp`; build; watch them fail to compile or fail.
2. Add the class to `src/codec.h` and `src/pure.cpp`; run the touched tests until they pass.
3. Full suite once. Commit: `feat: a BitrateGovernor steps the bitrate down under sustained drops`.

### Task 3: wire the governor into the sender

1. Write the link test(s); watch the healthy-link check fail to compile (no `bitratePercent` yet).
2. Add `bitratePercent` to `SenderStats` (last field), the member, the calls in `sendPicture`, `openEncoder` and
   `skipVideo`.
3. Run the touched tests, then the full suite once. Update the finding 5 row and section 5 of the review.
4. Commit: `feat: an HX sender lowers its bitrate while receivers keep dropping frames`.

## Review Focus

- The governor never changes the percent on a single burst, and never steps more than once per `kStepMs`.
- No reopen happens inside a `sendPicture` call after packets were taken; `stamps` and `alphas` stay consistent
  (they are cleared by `openEncoder` on reopen).
- `SenderStats` field order: `bitratePercent` is last.
- The decoder source is unchanged after Task 1.
- Nothing staged from `build/`, `dist/`, `.superpowers/`, `graphify-out/`.
