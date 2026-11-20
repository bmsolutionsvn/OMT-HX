# BreadStudio: OMT plugin fixes (app review S1–S6)

Repo: `F:\Coding\BreadMedia\BreadStudio`. Base branch: `main`.
Review: `F:\Coding\BreadMedia\OMT-HX\docs\reviews\2026-11-11-app-review.md`, findings S1–S6.
Files: `breadstudio/omt-hx/omt-hx.cpp`, `omt-output.cpp`, `omt-output.hpp`, `CMakeLists.txt`, `test/`.
Find every symbol with `git grep` first.

## Task 0: baseline

The plugin compiles OMT-HX's sources from `../OMT-HX` as checked out, and that checkout has moved on. Before any
change, build `omt-hx` and `bread-omt-test` on `main` and run `bread-omt-test`. If the build fails because of
OMT-HX (a new source file, a new definition), fix `breadstudio/omt-hx/CMakeLists.txt` only, and commit it as
`build(omt-hx): follow the OMT-HX checkout`. Record what was needed. If it fails for another reason, stop and
report.

## Spec (binding)

**S1, exceptions.** omthx throws `std::exception` from `sendVideo*`, `sendAudio`, `receiveVideo` and
`receiveAudio`. None may leave `omtOutputRawVideo`, `omtOutputRawAudio` or `worker_main`.
- Output callbacks: catch, drop the frame, log with `blog(LOG_WARNING, ...)`. The same message is logged once,
  not per frame: keep the last logged text and log again only when it differs.
- `worker_main`: the same around one pass of the loop, then wait 100 ms before the next pass. The receiver
  object is kept (it reconnects by itself).
- Also catch `...` in each place, logged as an unknown error.

**S2, the clock.** Today `base` is set from the first frame only. New rule, in a small header
`breadstudio/omt-hx/Anchor.hpp` with no OBS include so the test can use it:

```cpp
// Maps a sender's timestamps (100 ns units) to this PC's clock (ns). The first one sets the offset; one that
// lands more than a second from now sets it again (the sender restarted, or its clock drifted that far).
struct Anchor {
  uint64_t toLocal(int64_t omt, uint64_t nowNs);
};
```

Video and audio share one `Anchor`, as they share `base` now.

**S3, nobody connected.** `omtOutputRawVideo` and `omtOutputRawAudio` return at once when
`sender->hasReceivers()` is false, before any conversion or copy.

**S4.** The audio callback's planar buffer is a member of `OmtOutput`, not a new vector per call.

**S5.** In the BGRA path, when `frame->linesize[0] != width * 4` the rows are copied into `o->converted` and
that is sent; when they are equal, today's no-copy span stays.

**S6, fallbacks in the log.** About once a second, the output reads `sender->stats().lastFallback` and the
source reads `rx->stats().lastFallback`; a value that differs from the last one seen is logged with
`blog(LOG_INFO, ...)`. In the source this happens under `rxMutex` or on the worker thread, wherever `rx` is
already safe to use.

Anti-goals: no new setting or property, no change to tally, discovery or the catalog, no use of `setPreview`,
no change in `../OMT-HX`.

Tests (`bread-omt-test`, add `test/AnchorTest.cpp` or extend the existing test's `main`):
- `Anchor`: the first timestamp maps to now; a later one keeps the offset; one that maps more than 1 s into the
  past or the future maps to now again; 0.9 s off keeps the offset.
- S1, S3–S6 touch OBS callbacks and have no unit test: they are checked by review and by the owner in OBS. Say so
  in the report.

Done when: `omt-hx` and `bread-omt-test` build, `bread-omt-test` passes, and `bread-onair-test` passes if it
could be run on `main` before the change (try once; if it cannot start there, record it and skip it).

## Tasks

### Task 1: no exception leaves the plugin (S1)

Commit: `fix(omt-hx): an OMT-HX error is logged instead of ending the program`.

### Task 2: the clock offset follows a restarted sender (S2)

TDD. Commit: `fix(omt-hx): a source's clock offset is set again after a jump`.

### Task 3: the output does nothing with nobody connected, and copies less (S3, S4, S5)

Commit: `perf(omt-hx): the output skips its work with no receiver; odd row sizes are handled`.

### Task 4: fallbacks are logged (S6)

Commit: `feat(omt-hx): sender and receiver fallbacks go to the log`.

## Review Focus

- No call into omthx from an OBS callback or the worker thread is outside a `try`.
- A repeating error logs once, not sixty times a second.
- `Anchor` is used for both video and audio, and the 1 s test uses the mapped time against now, both ways.
- `hasReceivers()` is checked before `video_scaler_scale`.
- GitNexus `impact` was run for each edited symbol and `detect-changes` before each commit (see the brief).
