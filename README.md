# OMT-HX

A second video mode for [Open Media Transport](https://github.com/openmediatransport), in the way
NDI|HX is a second mode of NDI: a hardware-encoded video codec inside ordinary OMT connections.
Version 1 codes HEVC (H.265) or H.264 (AVC) from the GPU's hardware encoder. Discovery, audio and
tally are OMT's, unchanged.

`omthx` is the library BreadCamera (sender) and BreadStudio (receiver) share. One `Sender` sends
plain OMT or OMT-HX; one `Receiver` reads both.

- Design: [docs/superpowers/specs/2026-10-12-omt-hx-design.md](docs/superpowers/specs/2026-10-12-omt-hx-design.md)
- Measured figures: [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md), reproduced with `scripts\measure.ps1`

## Build and test

Needs Windows x64, Visual Studio 2026 with the C++ desktop workload, the .NET 8 SDK and git.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test.ps1
```

The first run builds the patched `libomt.dll` (see `patches/libomtnet-hx.patch`) and installs
FFmpeg through vcpkg.

## Try it

```powershell
cd build\release
.\omthx-send.exe --alpha                 # window 1
.\omthx-recv.exe --list                  # window 2
.\omthx-recv.exe --source "<PC> (OMT-HX Test)" --dump frame.bmp
```

Add `--codec h264` to `omthx-send` to send H.264 instead of HEVC.

## Use it from an app

The app's `vcpkg.json` needs FFmpeg with `avcodec`, `swscale`, `nvcodec` and `amf`.

```cmake
set(OMTHX_ROOT "${CMAKE_SOURCE_DIR}/../OMT-HX" CACHE PATH "OMT-HX checkout")
add_subdirectory("${OMTHX_ROOT}" omthx)
target_link_libraries(my_app PRIVATE omthx)
omthx_copy_runtime(my_app)   # puts libomt.dll and libvmx.dll next to the exe
```

```cpp
#include "omthx/omthx.h"

omthx::Sender sender("Camera 1", omthx::Mode::Hx, omthx::Quality::Medium);
sender.sendVideo(format, bgra, /*premultiplied*/ false, timestamp);
bool onAir = sender.tally().program;

omthx::Receiver receiver("CAMERA-PC (Camera 1)");
omthx::VideoFrame frame;
if (receiver.receiveVideo(frame, 100)) { /* frame.bgra, premultiplied when it has alpha */ }
receiver.setTally({.preview = false, .program = true});
```

A fourth `Sender` argument picks the HX codec: `omthx::Codec::Hevc` (the default) or
`omthx::Codec::H264`. The receiver needs no setting; it reads whichever arrives, and follows a
source that comes back with the other codec.

Timestamps must be one frame duration apart. A larger step tells the receiver a frame was
skipped, and it holds the last picture until the next keyframe.

## How it compares

OMT-HX is to OMT what NDI|HX is to NDI: the same transport, with a compressed video mode for
links that cannot carry the full picture. Compressed modes spend latency to save bandwidth; full
modes spend bandwidth to save latency.

| Mode | Codec | 1080p60 bandwidth | Latency | Alpha | Codec cost |
|------|-------|-------------------|---------|-------|------------|
| OMT (full) | VMX, intra, visually lossless | ~200 Mbps | ~9 ms | yes, in the BGRA frame | libomt owns it, no GPU codec |
| OMT-HX, HEVC | HEVC (H.265), GPU hardware | 15 / 30 / 50 Mbps | 29-42 ms | optional, stacked plane | ~5-11 ms to send, ~6-13 ms to receive, CPU colour conversion included |
| OMT-HX, H.264 | H.264, GPU hardware | 21 / 42 / 70 Mbps | 29-41 ms | optional, stacked plane | ~5-11 ms to send, ~6-13 ms to receive, CPU colour conversion included |
| NDI (High Bandwidth) | SpeedHQ, intra | ~150 Mbps | ~16 ms | yes, RGBA 4:4:4:4 | CPU |
| NDI\|HX | H.264 / HEVC | 1-50 Mbps | 80-200 ms | limited, 4:2:0 | CPU or GPU |

- The OMT and OMT-HX rows are measured on one PC over `localhost` ([docs/MEASUREMENTS.md](docs/MEASUREMENTS.md)),
  so they are codec and library figures with no network in the path. The NDI and NDI|HX rows are
  vendor-published typicals, not measured here. A real link adds its one-way time to every mode.
- OMT-HX's delay is a floor of one receiver-held frame (about 17 ms at 60 fps) plus the
  encode/decode round trip: it measured 29 ms at 720p60 and 42 ms at 1080p60. OMT full is
  intra-only, so it has no such hold.
- The GPU codec is the smaller part of the cost. At 1080p60 the hardware encode call takes about
  3 ms and the decode about 4 ms; converting between BGRA and NV12 on one CPU core takes about 5 ms
  at each end. OMT full spends no GPU codec time but carries the whole picture.
- Both compressed modes are lossy (OMT-HX is 8-bit 4:2:0 HEVC or H.264); OMT full and NDI are intra and
  visually lossless. OMT-HX keeps OMT discovery, audio and tally; NDI|HX keeps NDI's.
- Resolution and frame rate are bounded by the time each end needs per frame, most of it CPU colour
  conversion, and not by the transport or the GPU codec: on the test PC, HX holds 60 fps up to 1080p
  (13 Mbps at 720p, 30 Mbps at 1080p) and drops below real time at 1440p and 4K. The optional alpha
  mode doubles the coded picture. With alpha, 720p60 and 1080p30 hold; 1080p60 falls just short
  (about 57 fps received) when sender and receiver share one PC. The full sweeps are in [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md).

## Technical detail

**Transport.** OMT-HX is not a new protocol. In version 1 it sends HEVC or H.264 as ordinary OMT
video frames tagged `HEVC` or `H264` (a small patch lets stock libomt carry, rather than reject, a
foreign tag).
Discovery, connection, audio, tally and metadata stay OMT's, so an OMT-HX source is found and
connected to exactly like a plain OMT one.

**Codec.** Version 1 codes HEVC (H.265) or H.264 (AVC), 8-bit, 4:2:0, progressive, no B-frames,
full-range BT.709. HEVC stays the default and is Main profile; the H.264 profile is the encoder's
choice. Encoding runs on the GPU's hardware encoder through FFmpeg (`hevc_amf` and `h264_amf` on
AMD, `hevc_nvenc` and `h264_nvenc` on NVIDIA); decoding uses D3D11 hardware (`hevc (d3d11va)` or
`h264 (d3d11va)`). Plain OMT instead uses libomt's VMX codec, which is intra and has no GPU codec
step.

**Colour.** Buffers cross the API as BGRA and are converted to NV12 (BT.709, full range) on the CPU
before the encoder. HX is 4:2:0, so chroma is halved and the picture is lossy, which is the point of
the mode. Full OMT keeps the whole BGRA picture, so it is visually lossless.

**Alpha (optional).** Alpha is off by default: it is a flag on the frame format
(`VideoFormat::alpha`), and a run without it codes only the display picture at no extra cost. Turned
on, the alpha plane is stacked under the colour in the same coded picture (a 1080p alpha source codes
as 1920x2176), so it rides in the one stream with no second connection. Alpha is lossy and doubles
the coded size. With alpha on, each end needs about 13 ms per 1080p frame when it runs alone, inside
the 16.7 ms of a 60 fps frame. With sender and receiver on one PC they share the CPU and GPU, and
1080p60 falls just short: a median of 57 fps received over ten minutes (720p60 and 1080p30 hold).
Two PCs are not measured yet. For reference, full-bandwidth NDI carries RGBA 4:4:4:4 alpha cleanly, while NDI|HX is
H.264/HEVC 4:2:0 and its encoders typically drop or heavily compress alpha.

**Measured ceiling.** On the test PC (Radeon RX 6600, Ryzen 7 5700X), HX holds 60 fps up to 1080p
(about 30 Mbps at medium) and drops below real time at 1440p and 4K. The hardware codec is not the
limit: alone it encodes the 1080p alpha picture at 195 fps and decodes it at 305 fps. See
[docs/MEASUREMENTS.md](docs/MEASUREMENTS.md) for the resolution, frame-rate, quality and alpha
sweeps.

## Limits of version 1

8-bit 4:2:0 only. Colour conversion runs on the CPU. A program that speaks only stock OMT sees an
OMT-HX source with audio and no picture. Both ends must run this build of libomt.dll: an older
OMT-HX receiver, like a stock OMT program, sees an H.264 source with audio and no picture. See
sections 3 and 17 of the design.

## Licence

MIT. The bundled `libomt.dll` and `libvmx.dll` are MIT, from the Open Media Transport project,
with one patch.