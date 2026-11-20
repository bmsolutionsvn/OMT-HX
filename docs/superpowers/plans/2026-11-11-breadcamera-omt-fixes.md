# BreadCamera: OMT input and output fixes (app review C1–C5)

Repo: `F:\Coding\BreadMedia\BreadCamera`. Base branch: `main`.
Review: `F:\Coding\BreadMedia\OMT-HX\docs\reviews\2026-11-11-app-review.md`, findings C1–C5.
Files: `src/io/omt_input.cpp`, `src/io/omt_input.h`, `src/io/omt_output.cpp`, `src/io/omt_output.h`,
`tests/omt_input_test.cpp`, `tests/omt_output_test.cpp`. Find every symbol with `git grep` first.

## Spec (binding)

**C1, mono audio.** `OmtInput` turns an `omthx::AudioFrame` (planar float: channel 0's samples, then channel
1's) into an interleaved stereo block. With `a.channels == 1` it reads `a.planar[a.samples + i]`, past the
buffer. Rule: with one channel, both output channels take channel 0; with two or more, the first two, as now.
Move the copy into a function a test can call without a network source.

**C2, zero channels.** `OmthOutput::deliver` divides by `f.channels`. Audio with `f.channels <= 0` is skipped.

**C3, the GPU refusal.** `gpuRefused_` latches for the object's life. New rule: the refusal and its warning are
cleared (a) in `open()`, and (b) when a frame arrives whose width or height differs from the frame that was
refused. No timer: omthx already retries its own encoder fallbacks, and what reaches the app is a property of
the picture or the device. First read the `throw` sites reachable from `Sender::sendVideoD3D11` in
`F:\Coding\BreadMedia\OMT-HX\src\link.cpp` and list them in the ledger; if one of them is a passing condition
(not a property of the picture or device), record a `Ruling:` and keep to the rule above.

**C4, status.** `OmthOutput::status()` fills `OutputStatus::note` (it exists, see `src/io/output.h`) from the
sender's stats: `lastFallback` when not empty, and `"bitrate lowered to N %"` when `bitratePercent < 100`; both
when both apply, joined with `"; "`. `deliver()` reads `send_->stats()` at most once a second and stores the two
values under the existing `warnMutex_`; `status()` only reads the stored copy. `close()` clears them. An existing
`note` set by the base class is kept in front.

**C5.** The comment in `omt_input.h` or `.cpp` that says only the newest audio block is kept is out of date:
make it say what the code does.

Anti-goals: no change to the video paths' order, no new setting, no UI change, no change in `../OMT-HX` or
`../BreadReplay`.

Tests:

- C1: one channel of 4 samples gives 8 interleaved samples L == R; two channels give L and R from their planes;
  three channels give the first two. A buffer shorter than `samples * channels` is rejected as now.
- C2: an `OutFrame` with audio and `channels == 0` is delivered without a crash (if `deliver` cannot be reached
  from a test without a live sender, test the guard through whatever seam `omt_output_test.cpp` already uses).
- C3: through the seam `omt_output_test.cpp` already uses for the refusal: refused at an odd size, then a frame
  at an even size clears it; refused, `close()`, `open()` clears it.
- C4: only if the test seam reaches it without a network; otherwise a `Ruling:`.

Done when: the tests pass, the BreadCamera suite passes, BreadReplay still compiles against this checkout.

## Tasks

### Task 1: mono audio and zero channels (C1, C2, C5)

TDD. Commit: `fix: a mono OMT source fills both channels instead of reading past its buffer`.

### Task 2: the GPU refusal clears on a new size and on open (C3)

TDD. Commit: `fix: a refused OMT GPU path is tried again after open and after a size change`.

### Task 3: the status note carries the sender's fallback and lowered bitrate (C4)

Commit: `feat: the OMT output's status shows the sender's fallback and lowered bitrate`.

## Review Focus

- No read past `a.planar` for any `channels` and `samples`.
- `gpuRefused_` is still an atomic read by `needsCpu()` on another thread; the warning string stays under
  `warnMutex_`.
- `stats()` is not called from `status()` and not more than once a second from `deliver()`.
- BreadReplay compiles with no change of its own.
