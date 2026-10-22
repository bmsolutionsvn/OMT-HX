# OMT-HX on Linux (x64 and arm64) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** OMT-HX builds and passes its test suite on Linux x64 and Linux arm64, so the encoder box (an RK3588 running Linux) can send OMT and OMT-HX with it.

**Architecture:** Upstream OMT already runs on Linux: libvmx has an ARM NEON source (`vmxcodec_arm.cpp`, via sse2neon), libomtnet has Linux discovery through Avahi, and .NET Native AOT publishes `libomt.so` for `linux-x64` and `linux-arm64`. What is Windows-only is OMT-HX's own build glue (`cmake/omt.cmake`, `CMakeLists.txt`, presets, PowerShell scripts) and three spots in the C++ (the export macro, the encoder's waitable timer, the send tool's `timeBeginPeriod`). This plan makes those portable, picks libvmx sources per CPU, takes FFmpeg from the system on Linux, and adds the one test the box depends on: an HEVC stream coded outside omthx, sent with `Sender::sendCoded`, reaches a receiver. Hardware encoder backends for Linux desktops (VAAPI, Rockchip) are not in this plan; on Linux the Hx tests that need a hardware encoder say SKIPPED, as they already do on a PC without one.

**Tech Stack:** C++20, CMake ≥ 3.28 + Ninja, GCC 13 (Ubuntu 24.04) or Clang 17+, FFmpeg from the distribution (pkg-config), doctest (`doctest-dev`), .NET 8 SDK with Native AOT prerequisites (`clang`, `zlib1g-dev`), Avahi (`avahi-daemon`, `libavahi-client3`). Windows build unchanged (MSVC, vcpkg).

**Spec:** `F:\Coding\BreadMedia\docs\superpowers\specs\2026-10-20-omt-encoder-box-design.md` — §11 "OMT-HX Linux port" and D12 (the box needs `sendCoded`, keyframe requests, tally and the Full sender).

**Before starting:** the working tree has uncommitted changes in `docs/superpowers/plans/2026-10-13-h264-codec.md` and `tests/link_test.cpp` that are the owner's. Work on a new branch `linux-port` and do not touch those two files.

## Status (2026-10-22)

- **Task 1: done** in `db9c768` ("feat: build and run on Linux, and a C API for the OBS plugin"), with one
  difference: the encoder also falls back to software `libx265`/`libx264`, so Hx tests run without a GPU.
- **Task 2: covered differently.** With the software fallback, the existing Hx tests run on Linux: the relay
  test pins `sendCoded` and keyframe requests, the HX source test pins tally. The one case they missed, a
  stream starting mid-GOP, is now `tests/link_test.cpp` "a coded stream that starts mid-GOP shows nothing
  until its next keyframe". `tests/coded_source_test.cpp` and `x265Stream` are not needed.
- **Task 3: open.** CI jobs exist (`build.yml`) but have not passed yet: the run on `e94ca7a` failed at
  checkout (the `omt/libomt` commit was not pushed yet; it is now), and the Windows job before it failed in
  vcpkg on an `amd-amf` download hash. Steps 2-5 remain: green CI, the RK3588 run, the Full-mode figures.

## Global Constraints

- The Windows build and its tests must keep passing unchanged: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test.ps1` after every task.
- No new dependency on Windows. On Linux, only distribution packages: `build-essential cmake ninja-build pkg-config libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev doctest-dev clang zlib1g-dev avahi-daemon libavahi-client3`, plus the .NET 8 SDK.
- FFmpeg linked dynamically (LGPL), from the system on Linux (spec §5).
- `libomt` and `libvmx` keep their file names: `libomt.so` / `libvmx.so` on Linux (`DllImport("libvmx")` in libomtnet resolves `libvmx.so`).
- Binaries find `libomt.so` and `libvmx.so` beside themselves (RPATH `$ORIGIN`), from any working directory.
- No changes to the `omt/` submodules.
- Commit subjects follow the repo: `build:`, `feat:`, `test:`, `ci:`, `docs:`; body ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

- **No Avahi daemon running** (a fresh Linux box): `discover()` must return (empty) within 5 s and senders must still accept connections by address. Pinned in Task 1 (`discover returns promptly`).
- **Run from another directory** (the box starts the daemon from systemd, not from the build folder): the tests must load `libomt.so`/`libvmx.so` via RPATH. Pinned in Task 1 Step 6 (run from the repo root).
- **An x86 CPU without AVX2** (older mini PCs): only `vmxcodec_avx2.cpp` may be compiled with AVX2; everything else targets x86-64-v2. Pinned in Task 1 Step 3 and checked in review: libvmx must choose the AVX2 path at run time, not at build time.
- **FFmpeg without libx265** (some distribution builds): the coded-source test says SKIPPED with a message, never fails. Pinned in Task 2.
- **A coded stream that starts mid-GOP** (a box restarted, a receiver joining late): frames before the first keyframe must not crash the receiver or show garbage; the first picture comes from the first keyframe. Pinned in Task 2 (`coded frames before the first keyframe are skipped`).

---

### Task 1: Build and test on Linux x64

**Files:**
- Modify: `cmake/omt.cmake` (libvmx sources and flags per platform/CPU; libomt RID, output name and runtime list per OS)
- Modify: `CMakeLists.txt` (FFmpeg via pkg-config off Windows; warnings per compiler; `winmm` only on Windows; RPATH; TBB if found)
- Modify: `include/omthx/omthx.h:11-17` (export macro)
- Modify: `src/encoder.cpp:12,105-111,175-180` (wait without the Win32 timer off Windows)
- Modify: `tools/omthx_send.cpp:2,78` (`timeBeginPeriod` only on Windows)
- Modify: `CMakePresets.json` (Linux presets)
- Create: `scripts/test.sh`
- Modify: `tests/libomt_test.cpp` (the discovery test, plus `#include "omthx/omthx.h"`)
- Modify: `README.md` ("Build and test": a Linux subsection)

**Interfaces:**
- Produces: configure presets `linux-release` and `linux-debug` (binary dirs `build/linux-release`, `build/linux-debug`), no vcpkg toolchain, `CMAKE_BUILD_TYPE` `RelWithDebInfo` / `Debug`; build presets of the same names.
- Produces: `scripts/test.sh [--debug] [doctest-filter]` — inits submodules, configures, builds, runs `omthx_tests` (with `--test-case=<filter>` when given); exit code is the tests'.
- Produces: CMake variable `OMTHX_RID` (`win-x64`, `linux-x64`, `linux-arm64`, `osx-arm64`), from `CMAKE_SYSTEM_NAME` and `CMAKE_SYSTEM_PROCESSOR` (`x86_64`/`AMD64` → `x64`, `aarch64`/`arm64` → `arm64`); fatal error for anything else.
- Produces: `OMTHX_RUNTIME` lists `libomt.so` + `libvmx.so` on Linux (`.dll` on Windows as today).

- [x] **Step 1: Write the failing test** in `tests/libomt_test.cpp`

```cpp
TEST_CASE("discover returns promptly") {
  const auto start = std::chrono::steady_clock::now();
  const auto names = omthx::discover();
  const auto took = std::chrono::steady_clock::now() - start;
  CHECK(took < std::chrono::seconds(5));
  (void)names;  // empty without a discovery daemon; that is fine
}
```

- [x] **Step 2: Run it on Linux to see it fail to build**

Run (Ubuntu 24.04 x64, packages from Global Constraints): `bash scripts/test.sh`
Expected: FAIL — `scripts/test.sh` does not exist yet; with a hand-run `cmake -S . -B build/x -G Ninja`, configure fails in `cmake/omt.cmake` on MSVC-only flags (`/O2`) or `find_package(FFMPEG)`.

- [x] **Step 3: Make libvmx build per CPU in `cmake/omt.cmake`**

Sources: always `vmxcodec.cpp`. On x64: plus `vmxcodec_x86.cpp` and `vmxcodec_avx2.cpp`. On arm64: plus `vmxcodec_arm.cpp`. `exports.def` and the `_WINDOWS _USRDLL` definitions only on Windows. Flags off MSVC: `-O2 -w -fPIC`; `vmxcodec_x86.cpp` gets `-march=x86-64-v2`, `vmxcodec_avx2.cpp` gets `-march=x86-64-v3`, the arm64 sources nothing extra (NEON is baseline on ARMv8). MSVC keeps today's `/O2 /W0 /WX-` and `/arch:AVX2`. Keep `PREFIX ""` and `OUTPUT_NAME libvmx`.

- [x] **Step 4: Make libomt build per OS in `cmake/omt.cmake`**

`dotnet publish ... -r ${OMTHX_RID}`; native folder `libomt/bin/Release/net8.0/${OMTHX_RID}/native`. `-p:IlcUseEnvironmentalTools=true` only on Windows. Outputs and link: Windows `libomt.dll` + `libomt.lib` (as today); Linux `libomt.so`, linked directly. `OMTHX_RUNTIME` = the native library + `$<TARGET_FILE:libvmx>`.

- [x] **Step 5: Port the rest**

- `CMakeLists.txt`: off Windows, `find_package(PkgConfig REQUIRED)` and `pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale libswresample)`, which sets the same `FFMPEG_INCLUDE_DIRS`, `FFMPEG_LIBRARY_DIRS`, `FFMPEG_LIBRARIES` the targets already use. `omthx_warnings`: MSVC flags as today; otherwise `-Wall -Wextra`. Off Windows set `CMAKE_BUILD_RPATH` and `CMAKE_INSTALL_RPATH` to `$ORIGIN`. `find_package(TBB QUIET)` and link `TBB::tbb` to the library when found (libstdc++'s `std::execution::par` in `src/pure.cpp` runs serially without it). `winmm` only on Windows.
- `include/omthx/omthx.h`: on non-Windows, `OMTHX_BUILD_DLL` → `__attribute__((visibility("default")))`, the import case → empty.
- `src/encoder.cpp`: `<windows.h>`, `CreateWaitableTimerExW`, `CloseHandle`, `SetWaitableTimer` under `#ifdef _WIN32`; elsewhere `timer_` stays null and the wait in `Encoder::drain()` is `std::this_thread::sleep_for(std::chrono::microseconds(200))` (replacing `SwitchToThread()` on that branch only off Windows).
- `tools/omthx_send.cpp`: `<windows.h>` and `timeBeginPeriod(1)` under `#ifdef _WIN32`.
- `CMakePresets.json`: add `linux-release` / `linux-debug` (Ninja, no toolchain file) and their build presets; keep `release` / `debug` as they are.
- `scripts/test.sh`: as in Interfaces; `set -euo pipefail`.
- `README.md`: a "Linux (x64, arm64)" subsection under "Build and test": the package list from Global Constraints, `bash scripts/test.sh`, and that discovery needs `avahi-daemon` running (senders still accept connections by address without it).

- [x] **Step 6: Run the tests on Linux x64, from the repo root**

Run: `bash scripts/test.sh` then, from the repo root, `build/linux-release/omthx_tests`
Expected: both PASS. Hx tests that need a hardware encoder print `SKIPPED, no hardware ...` and pass; `discover returns promptly` passes; `ldd build/linux-release/omthx_tests` shows `libomt.so` resolved from `build/linux-release`.

- [x] **Step 7: Run the Windows suite**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test.ps1`
Expected: PASS, same counts as before plus the new test.

- [x] **Step 8: Commit**

```bash
git add cmake/omt.cmake CMakeLists.txt CMakePresets.json include/omthx/omthx.h src/encoder.cpp tools/omthx_send.cpp scripts/test.sh tests/libomt_test.cpp README.md
git commit -F - <<'EOF'
build: build and test OMT-HX on Linux x64

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

---

### Task 2: An HEVC stream coded outside omthx reaches a receiver

The box codes HEVC on its own hardware encoder and sends it with `sendCoded` (spec D12). This pins that contract on every platform, with no GPU, using FFmpeg's `libx265`.

**Files:**
- Create: `tests/coded_source_test.cpp`
- Modify: `CMakeLists.txt` (add it to `omthx_tests`)

**Interfaces:**
- Consumes: `Sender(name, Mode::Hx, Quality::Medium, Codec::Hevc)`, `Sender::sendCoded(const CodedFrame&)`, `Sender::address()`, `Sender::keyframeRequested()`, `Sender::tally()`, `Receiver(address, PixelFormat::Bgra)`, `Receiver::receiveVideo`, `Receiver::requestKeyframe`, `Receiver::setTally`; `drawPattern` from `tools/pattern.h`.
- Produces: test helper (file-local) `std::vector<CodedFrame> x265Stream(int w, int h, int frames, int keyint)` — returns empty, after `MESSAGE("SKIPPED, FFmpeg has no libx265: ...")`, when `avcodec_find_encoder_by_name("libx265")` is null. Each frame: `mode = Mode::Hx`, `codec = Codec::Hevc`, `format = {w, h, 60, 1, false}`, `keyframe` from the packet's `AV_PKT_FLAG_KEY`, `seq` = 0, 1, 2…, `timestamp = i * 166'667`, Annex B data with VPS/SPS/PPS in every keyframe (x265 params `repeat-headers=1:bframes=0:keyint=<keyint>:min-keyint=<keyint>:scenecut=0`, `tune=zerolatency`, `preset=ultrafast`, input converted from the BGRA pattern to `yuv420p` with swscale).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("an HEVC stream coded outside omthx arrives as pictures") {
  const auto stream = x265Stream(320, 180, 120, 30);
  if (stream.empty()) return;
  Sender sender("coded-source-test", Mode::Hx, Quality::Medium, Codec::Hevc);
  Receiver receiver(sender.address(), PixelFormat::Bgra);
  // send at 60 fps on a thread, repeating the stream, until the receiver has 30 pictures or 10 s pass
  VideoFrame frame;
  CHECK(receiveSome(receiver, frame, 30, 10) == 30);
  CHECK(frame.format.width == 320);
  CHECK(frame.format.height == 180);
  CHECK(frame.bgra.size() == 320u * 180u * 4u);
  receiver.setTally({false, true});
  // within 2 s: CHECK(sender.tally() == Tally{false, true})
}

TEST_CASE("coded frames before the first keyframe are skipped") {
  const auto stream = x265Stream(320, 180, 90, 30);
  if (stream.empty()) return;
  // send frames 10..89 (starts mid-GOP; the next keyframe is frame 30) once, at 60 fps
  // expect: no crash, the first picture's timestamp >= stream[30].timestamp, and at least 30 pictures in total
}

TEST_CASE("a receiver's keyframe request reaches sendCoded's caller") {
  const auto stream = x265Stream(320, 180, 60, 600);
  if (stream.empty()) return;
  // while sending, receiver.requestKeyframe(); CHECK(sender.keyframeRequested()) becomes true within 2 s
}
```

Write the two sketched bodies with the same send-thread pattern as the first test (a `Pump`-style thread over `stream`; a local `receiveSome` like `tests/link_test.cpp`'s).

- [ ] **Step 2: Run them to verify they fail**

Run: `bash scripts/test.sh "*coded*"` (and on Windows `scripts\test.ps1 -Filter "*coded*"`)
Expected: FAIL to build — `x265Stream` not defined. (If the platform's FFmpeg lacks libx265, the finished tests print SKIPPED; Ubuntu 24.04's FFmpeg has it, vcpkg's does not unless the `x265` feature is added — do not add it: the Windows run may SKIP.)

- [ ] **Step 3: Implement `x265Stream` in `tests/coded_source_test.cpp` and add the file to `omthx_tests` in `CMakeLists.txt`**

- [ ] **Step 4: Run the tests to verify they pass**

Run: `bash scripts/test.sh "*coded*"`
Expected on Linux x64: 3 PASS, none SKIPPED. On Windows: PASS or SKIPPED with the libx265 message.

- [ ] **Step 5: Run both full suites** (`bash scripts/test.sh`, `scripts\test.ps1`)

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add tests/coded_source_test.cpp CMakeLists.txt
git commit -F - <<'EOF'
test: an HEVC stream coded outside omthx reaches a receiver

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

---

### Task 3: Linux arm64 and CI

**Files:**
- Modify: `.github/workflows/build.yml` (two jobs: `linux-x64` on `ubuntu-24.04`, `linux-arm64` on `ubuntu-24.04-arm`)
- Modify: `docs/MEASUREMENTS.md` (a section "Linux arm64 (RK3588)": Full-mode rows only)

**Interfaces:**
- Consumes: `scripts/test.sh`, presets `linux-release` (Task 1); `tests/coded_source_test.cpp` (Task 2).

- [x] **Step 1: Add the CI jobs**

Each job: checkout with submodules, `actions/setup-dotnet@v4` (8.0.x), `apt-get install` the Global Constraints package list, `bash scripts/test.sh`. No release packaging for Linux in this plan. The Windows job is untouched.

- [ ] **Step 2: Push the branch and check CI**

Run: `git push -u origin linux-port`, then watch the run.
Expected: `windows-x64`, `linux-x64` and `linux-arm64` all green; the arm64 log shows libvmx built from `vmxcodec_arm.cpp`, and the three coded-source tests PASS.

- [ ] **Step 3: Run on an RK3588 board** (Orange Pi 5 Plus or Radxa Rock 5B+, Ubuntu/Debian arm64, `avahi-daemon` running)

Run: `bash scripts/test.sh`; then on the board `build/linux-release/omthx-send --mode full --name Board`; on a Windows PC on the same LAN `omthx-recv --list`, then `omthx-recv --source "<board host> (Board)" --seconds 10 --tally program`.
Expected: tests PASS; `--list` shows the board's source; the receiver reports about 600 frames in 10 s; the board's sender reports program tally.

- [ ] **Step 4: Measure Full mode on the board**

Run: `omthx-send --mode full --size 1920x1080 --fps 60 --seconds 60`, one instance and then two at once (different `--name`), with one `omthx-recv` connected to each; record the frames each receiver got and the senders' CPU from `pidstat -u 5`.
Expected: a table in `docs/MEASUREMENTS.md` under "Linux arm64 (RK3588)": fps held and CPU % for one and two 1080p60 Full senders. This answers the encoder box's risk R2; no pass/fail here, the numbers go to the box design.

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/build.yml docs/MEASUREMENTS.md
git commit -F - <<'EOF'
ci: build and test on Linux x64 and arm64; RK3588 Full-mode figures

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

---

## Not in this plan

- Linux hardware encoders and decoders for omthx's own `Encoder`/`Decoder` (VAAPI, NVENC on Linux, Rockchip MPP) and GPU-resident pictures on Linux — a later plan, for Linux desktops.
- macOS (VideoToolbox) — later.
- The encoder box daemon (capture, crop, MPP encode, web page, updates) — the next plan; it uses `sendCoded` as pinned in Task 2.
