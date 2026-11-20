# Fallback retry (core review finding 7, second half)

Base branch: `fix/core-review-1-4`. Review: `docs/reviews/2026-11-10-core-review.md`, finding 7.
Names are stable, line numbers are not: find every symbol with `git grep` before editing.

## Spec (binding)

Today each fallback flag latches for the object's life after one failure: a GPU encoder session that was busy for
a moment (another app held the last one) leaves the sender on the slow path until it is destroyed. Each flag
becomes a gate that lets the fast path be tried again after a delay that doubles on each failure.

Pure logic, in `src/codec.h` / `src/pure.cpp` beside `KeyframePolicy`:

```cpp
// A fast path that failed is tried again after a delay, which doubles with each failure in a row.
class RetryGate {
 public:
  static constexpr int64_t kFirstMs = 30'000;
  static constexpr int64_t kMaxMs = 600'000;
  bool closed(int64_t nowMs) const;  // true while the fast path must not be tried
  void fail(int64_t nowMs);          // the path failed: closed for the current delay, and the delay doubles
  void succeed();                    // the path worked: the delay goes back to kFirstMs
  void latch();                      // closed for good (test hooks)
};
```

Rules:
- New gate: open. `fail` at t closes it until `t + delay`; delay starts at `kFirstMs` and doubles per `fail` up to
  `kMaxMs`. `succeed` resets the delay to `kFirstMs` (and is cheap to call every frame). `latch` closes it for good.

Wiring, `src/link.cpp`. Replace these `bool` flags with a `RetryGate` each; where the flag was set, call `fail(nowMs)`
(keep the `fellBack(...)` call); where the fast path completes a frame, call `succeed()`; where the flag was read,
call `closed(nowMs)`. `nowMs` is milliseconds since the object's `start` clock, as the sender already computes.

- Sender: `gpuRefused`, `uyvyRefused`, `previewRefused`, `previewGpuRefused`.
- Receiver: `readbackFailed`, `gpuBgraFailed`.
- Not changed: `noGpu[2]` in `HxStream` (a GPU without a codec's decoder does not grow one), and the `uyvyGpu`
  setting.
- Test hooks keep their meaning: `OMTHX_TEST_GPU_REFUSED` calls `latch()`. `OMTHX_TEST_GPU_DECODE_REFUSED` is
  untouched.
- A retry must cost the stream nothing when it fails again: every one of these paths already falls back inside the
  same frame, and that must stay true. Check each one; if a path loses the frame on failure today, keep that
  behaviour and record a `Ruling:`.
- When a retry succeeds, clear `stats.lastFallback`? No: leave it; it says "the latest path this sender gave up
  on". Do not add a stats field.
- Update the `ponytail: no retry` comments and the header comment for `lastFallback` if it says the paths are
  given up for good.

Tests:
- `tests/pure_test.cpp`: open when new; closed for 30 s after one failure and open after; delay doubles and caps
  at 10 min; `succeed` resets the delay; `latch` never opens.
- `tests/link_test.cpp`: the existing tests that use `OMTHX_TEST_GPU_REFUSED` and check `lastFallback` must still
  pass unchanged. No new end-to-end test (no PC here can make a GPU path fail and then recover) and no new
  environment-variable hook.

Anti-goals: no public API or C API change, no change to the submodules under `omt/`, no retry for `noGpu`, no
background thread or timer (the gate is only read when a frame comes).

Done when: the pure tests pass, the full suite passes, and the finding 7 row and section 7 of the review say what
was done and that a real recovery is untested.

## Tasks

### Task 1: RetryGate (pure)

1. Write the pure tests; build; watch them fail.
2. Add the class; run the touched tests until they pass.
3. Full suite once. Commit: `feat: a RetryGate lets a failed fast path be tried again later`.

### Task 2: the sender's and receiver's fallbacks retry

1. Replace the six flags as the spec says, one at a time, building after each.
2. Run the tests that mention `lastFallback`, `REFUSED`, `preview`, `Uyvy`, `D3D11`; then the full suite once.
3. Update the review. Commit: `fix: a fallback is tried again after 30 s, doubling to 10 min`.

## Review Focus

- Every former read of a flag now reads `closed(nowMs)` with a current `nowMs`; none was missed (`git grep` the
  old names: zero hits besides comments).
- A failed retry falls back inside the same frame, as the first failure did.
- `succeed()` is called only after the fast path really delivered, so a path that fails every time still doubles.
- `OMTHX_TEST_GPU_REFUSED` still forces the memory path for the whole test.
- Nothing staged from `build/`, `dist/`, `.superpowers/`, `graphify-out/`.
