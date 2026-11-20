# The receiver honours a stream's colour range (app review H1, OMT-HX half)

Repo: `F:\Coding\BreadMedia\OMT-HX`. Base branch: `fix/core-review-1-4`.
Review: `docs/reviews/2026-11-11-app-review.md`, finding H1.
Names are stable, line numbers are not: find every symbol with `git grep` before editing.

## Spec (binding)

Today every decoded HX picture is converted as full-range BT.709. A phone's MediaCodec stream is limited range
(luma 16–235), so its picture comes out with grey blacks. Our own sender signals full range and must look exactly
as it does now.

Rule: a decoded picture is **full range** when its `AVFrame::color_range == AVCOL_RANGE_JPEG`, and **limited
range** otherwise (`AVCOL_RANGE_MPEG` or unspecified: H.264 and HEVC define unspecified as limited).

What each output does with a limited-range picture:

- `PixelFormat::Bgra`, CPU path (`Packer::unpack`, swscale): source range limited, so black is 0 and white 255.
- `PixelFormat::Bgra`, D3D11 path (`Readback`, the video processor in `src/d3d11.cpp`): the stream colour space
  is the studio-range BT.709 one (`DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709`). The processor is rebuilt or its
  colour space set again when the range changes between pictures.
- `PixelFormat::Nv12`: the header promises full-range planes. A limited-range picture is copied and stretched
  to full (`copyNv12` with `fullRange == false` already does this). A full-range picture keeps today's no-copy
  path. The planes stay valid until the next `receiveVideo` call, as now.
- `PixelFormat::Gpu`: the surface is the decoder's own and is not converted. `VideoFrame` gains
  `bool fullRange = true;` (comment: false when a Gpu picture holds limited-range values). `download()` honours
  it for both targets.
- `PixelFormat::Coded`: untouched.
- `FrameDecoder` follows the same rule for its outputs.
- The preview stream follows the same rule (it is decoded by the same code).

Not changed: the matrix stays BT.709 for every stream (record in the review that a BT.601 stream is still
converted as BT.709); plain OMT (VMX) paths; the sender; the C API (if the C API cannot carry the new field,
leave it out and record a `Ruling:`); the submodules under `omt/`.

Tests:

- The stream for the test must be flagged limited. `Encoder` (internal, `src/codec.h`) gets a way to signal
  limited range, default full, used by tests only: it sets `AVCOL_RANGE_MPEG` on the context and frames and
  passes the NV12 it is given through unchanged. No environment variable, no public API.
- `tests/`: a picture whose left half is luma 16 and right half luma 235 (chroma 128), flagged limited, decoded:
  - as Bgra: left within 3 of 0, right within 3 of 255, on every Bgra path this PC can run;
  - as Nv12: Y within 3 of 0 and of 255;
  - as Gpu: `fullRange == false`, and `download()` to Bgra gives the same as above.
  The same picture flagged full: Bgra left within 3 of 16, right within 3 of 235, and Gpu `fullRange == true`.
- If the hardware encoder on this PC does not write the flag (the decoded frame of the limited stream does not
  report `AVCOL_RANGE_MPEG`), record a `Ruling:` and test the conversions by setting the range on a decoded
  frame directly instead.
- Every existing test passes unchanged: they pin today's full-range result.

Anti-goals: no change to what a full-range stream looks like, no new public function, no colour-matrix work, no
copy on the full-range Nv12 path.

Done when: the new tests pass, the full suite passes, `include/omthx/omthx.h` says what `fullRange` means and
that Nv12 and Bgra outputs are always full range, and the review's H1 row says the receiver half is done and
that a phone has not been checked.

## Tasks

### Task 1: the decoder reports each picture's range; the memory paths honour it

1. Write the Bgra (CPU) and Nv12 tests; build; watch them fail.
2. Add the limited-range switch to `Encoder`, carry the range with the decoded picture, fix `Packer::unpack`'s
   use and the Nv12 output.
3. Full suite. Commit: `fix: a limited-range HX stream is decoded to full-range Bgra and Nv12`.

### Task 2: the D3D11 path and Gpu pictures

1. Write the Gpu and D3D11-Bgra tests; watch them fail.
2. `Readback` honours the range; `VideoFrame::fullRange`; `download()` honours it.
3. Full suite. Update the header comments and the review. Commit:
   `fix: Gpu pictures say their range, and the D3D11 readback honours it`.

## Review Focus

- A full-range stream takes exactly the code path it took before (no copy added, same pixels).
- The range is read per picture, not once per stream: a stream that changes range mid-way is followed.
- Unspecified range is treated as limited.
- No path converts twice (a stretched Nv12 picture is not stretched again by a later Bgra conversion).
- Nothing staged from `build/`, `dist/`, `.superpowers/`, `graphify-out/`, `docs/superpowers/`.
