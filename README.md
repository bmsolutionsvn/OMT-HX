# OMT-HX

A second video mode for [Open Media Transport](https://github.com/openmediatransport), in the way
NDI|HX is a second mode of NDI: a hardware-encoded video codec inside ordinary OMT connections.
Version 1 codes HEVC (H.265) or H.264 (AVC) from the GPU's hardware encoder. Discovery, audio and
tally are OMT's, unchanged.

`omthx` is the library BreadCamera (sender) and BreadStudio (receiver) share. One `Sender` sends
plain OMT or OMT-HX; one `Receiver` reads both.

- Design: [docs/superpowers/specs/2026-10-12-omt-hx-design.md](docs/superpowers/specs/2026-10-12-omt-hx-design.md)
- Measured figures: [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md), reproduced with `scripts\measure.ps1`

## Binary release

Each `v*` tag publishes `OMT-HX.Binaries.Release.<tag>.zip` on the
[releases page](https://github.com/bmsolutionsvn/OMT-HX/releases), laid out like Open Media
Transport's own release. To use it from C++ (Visual Studio 2022 or newer, `/MD`): add
`Libraries\Winx64\include` to the include path, `#include "omthx/omthx.h"`, link `omthx.lib`, and put
every DLL in `Libraries\Winx64` beside your executable. The zip also holds `libomt.h`/`libomt.lib` for
OMT's C API, the .NET `libomtnet.dll`, the test tools and the licences. Every push is built and tested
by `.github/workflows/build.yml`. A build from source that compiles omthx into your program defines
`OMTHX_STATIC`; the CMake target `omthx` does it for you.

## Build and test

Needs Windows x64, Visual Studio 2022 or newer with the C++ desktop workload, the .NET 8 SDK and git.

```powershell
git clone --recurse-submodules https://github.com/bmsolutionsvn/OMT-HX
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test.ps1
```

Open Media Transport is built from source, from three submodules in `omt/`: forks of
[libomtnet](https://github.com/bmsolutionsvn/libomtnet), [libomt](https://github.com/bmsolutionsvn/libomt)
and [libvmx](https://github.com/bmsolutionsvn/libvmx). OMT-HX's changes to OMT are commits in the
libomtnet fork, on top of upstream. One CMake run builds everything: `libvmx.dll` from C++,
`libomt.dll` from C# with .NET Native AOT, then omthx. The first run also installs FFmpeg through vcpkg.

## Try it

```powershell
cd build\release
.\omthx-send.exe --alpha                 # window 1
.\omthx-recv.exe --list                  # window 2
.\omthx-recv.exe --source "<PC> (OMT-HX Test)" --dump frame.bmp
```

Add `--codec h264` to `omthx-send` to send H.264 instead of HEVC.

## Use it from an app

The app's `vcpkg.json` needs FFmpeg with `avcodec`, `swscale`, `nvcodec` and `amf`, and the OMT-HX checkout
needs its submodules (`git submodule update --init`). An app with its own FFmpeg can instead
`include(<OMT-HX>/cmake/omt.cmake)` for libomt and libvmx alone, as BreadStudio does.

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

An app that passes video on (a bridge, a router) can stay in 4:2:2 YUV, the format VMX and NDI carry:
`Sender::sendVideoUyvy` takes UYVY or UYVA (alpha plane after the UYVY plane, as OMT and NDI lay it out),
and `Receiver(address, omthx::PixelFormat::Uyvy)` hands frames back in `VideoFrame::uyvy`. Plain OMT then
needs no colour conversion at all, and HX converts 4:2:2 to 4:2:0 without a colour matrix.

Two more receiver kinds are for apps that republish a source. `PixelFormat::Gpu` keeps a decoded HX
picture in video memory (`VideoFrame::gpu`), and `Sender::sendVideo(frame)` re-codes it there, for
example HEVC in and H.264 out, with no copy to memory when the GPU encoder takes D3D11 pictures
(`h264_amf` does on the test PC; otherwise the picture goes through memory, and `omthx::download`
copies one out when the app needs it).
`PixelFormat::Coded` decodes nothing: `receiveCoded` hands back the VMX or HEVC/H.264 frames as the
source sent them and `Sender::sendCoded` sends them on, with keyframe requests passed back through
`Sender::keyframeRequested` and `Receiver::requestKeyframe`.

A fourth `Sender` argument picks the HX codec: `omthx::Codec::Hevc` (the default) or
`omthx::Codec::H264`. The receiver needs no setting; it reads whichever arrives, and follows a
source that comes back with the other codec.

Timestamps are in OMT units, one frame duration apart. A frame the app skips leaves a larger step,
and the receiver plays on: the encoder never saw that frame, so the stream has no hole. Each HX
frame carries its number in OMT's per-frame metadata (`<OMTHX Seq="n" />`); a jump in that number
means a frame was lost on the way, and the receiver holds the last picture until the next keyframe.
A source from before the numbering is judged by its timestamps instead.

## How it compares

OMT-HX is to OMT what NDI|HX is to NDI: the same transport, with a compressed video mode for
links that cannot carry the full picture. Compressed modes spend latency to save bandwidth; full
modes spend bandwidth to save latency.

| Mode | Codec | 1080p60 bandwidth | Latency | Alpha | Codec cost |
|------|-------|-------------------|---------|-------|------------|
| OMT (full) | VMX, intra, visually lossless | ~200 Mbps | ~9 ms | yes, in the BGRA frame | libomt owns it, no GPU codec |
| OMT-HX, HEVC | HEVC (H.265), GPU hardware | 15 / 30 / 50 Mbps | 10-15 ms | optional, stacked plane | ~4-7 ms to send, ~4-7 ms to receive, CPU colour conversion included |
| OMT-HX, H.264 | H.264, GPU hardware | 21 / 42 / 70 Mbps | 10-16 ms | optional, stacked plane | ~5-8 ms to send, ~4-7 ms to receive, CPU colour conversion included |
| NDI (High Bandwidth) | SpeedHQ, intra | ~150 Mbps | ~16 ms | yes, RGBA 4:4:4:4 | CPU |
| NDI\|HX | H.264 / HEVC | 1-50 Mbps | 80-200 ms | limited, 4:2:0 | CPU or GPU |

- The OMT and OMT-HX rows are measured on one PC over `localhost` ([docs/MEASUREMENTS.md](docs/MEASUREMENTS.md)),
  so they are codec and library figures with no network in the path. The NDI and NDI|HX rows are
  vendor-published typicals, not measured here. A real link adds its one-way time to every mode.
- OMT-HX's delay is the encode/decode round trip, with no frame held at either end: it measured
  10 ms at 720p60 and 15 ms at 1080p60 (29 and 42 ms before 2026-10-19, when every packet left the
  sender one frame late). The sender waits the few ms the hardware encoder needs for each packet.
- The GPU codec is now the larger part of the cost. At 1080p60 the hardware encode takes about
  5 ms including that wait, and the decode with its copy to memory about 4 to 7 ms; converting
  between BGRA and NV12 on swscale's threads takes about 1 ms at each end. OMT full spends no GPU
  codec time but carries the whole picture.
- Both compressed modes are lossy (OMT-HX is 8-bit 4:2:0 HEVC or H.264); OMT full and NDI are intra and
  visually lossless. OMT-HX keeps OMT discovery, audio and tally; NDI|HX keeps NDI's.
- Resolution and frame rate are bounded by the time each end needs per frame, not by the
  transport: on the test PC, with sender and receiver sharing one CPU and GPU, HX holds 60 fps up to
  1440p (13 Mbps at 720p, 30 at 1080p, 53 at 1440p) and 1080p at 120 fps; 4K reaches 45 fps. The
  optional alpha mode doubles the coded picture; 720p60, 1080p30 and 1080p60 alpha hold real time.
  The full sweeps are in [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md).

## Technical detail

**Transport.** OMT-HX is not a new protocol. In version 1 it sends HEVC or H.264 as ordinary OMT
video frames tagged `HEVC` or `H264` (a small change to OMT, in the libomtnet fork, lets it carry
rather than reject a foreign tag).
Discovery, connection, audio, tally and metadata stay OMT's, so an OMT-HX source is found and
connected to exactly like a plain OMT one.

**Codec.** Version 1 codes HEVC (H.265) or H.264 (AVC), 8-bit, 4:2:0, progressive, no B-frames,
full-range BT.709. HEVC stays the default and is Main profile; the H.264 profile is the encoder's
choice. Encoding runs on the GPU's hardware encoder through FFmpeg (`hevc_amf` and `h264_amf` on
AMD, `hevc_nvenc` and `h264_nvenc` on NVIDIA); decoding uses D3D11 hardware (`hevc (d3d11va)` or
`h264 (d3d11va)`). Plain OMT instead uses libomt's VMX codec, which is intra and has no GPU codec
step.

**Colour.** Buffers cross the API as BGRA or UYVY and are converted to NV12 (BT.709, full range) on
the CPU before the encoder. NV12 is not a choice: it is the input of every hardware encoder and the
output of D3D11 decoding for 8-bit 4:2:0, and NDI's own HX codecs use it the same way (NVDEC's NV12
is turned into UYVY by a CUDA kernel inside the NDI runtime). HX is 4:2:0, so chroma is halved and the picture is lossy, which is the point of
the mode. Full OMT keeps the whole BGRA picture, so it is visually lossless.

**Alpha (optional).** Alpha is off by default: it is a flag on the frame format
(`VideoFormat::alpha`), and a run without it codes only the display picture at no extra cost. Turned
on, the alpha plane is stacked under the colour in the same coded picture (a 1080p alpha source codes
as 1920x2176), so it rides in the one stream with no second connection. Alpha is lossy and doubles
the coded size. With sender and receiver on one PC, 1080p60 alpha holds 60 fps at 27 ms delay. NDI|HX
packs alpha the same way: NDI documents HX alpha as YUVA 4:2:0:4 "vertically stacking the video and
alpha frames into a single frame", and its runtime has a `codec_h264_hevc_alpha` codec for it.
Full-bandwidth NDI carries alpha as 4:2:2:4 instead.

**Measured ceiling.** On the test PC (Radeon RX 6600, Ryzen 7 5700X), HX holds 60 fps up to 1440p
(about 53 Mbps at medium) and 1080p120, and reaches 45 fps at 4K. Alone, the hardware codec encodes
the 1080p alpha picture at 195 fps and decodes it at 305 fps. See
[docs/MEASUREMENTS.md](docs/MEASUREMENTS.md) for the resolution, frame-rate, quality and alpha
sweeps.

## Limits of version 1

8-bit 4:2:0 only. Colour conversion runs on the CPU, except on the Gpu receiver to Sender path. A program that speaks only stock OMT sees an
OMT-HX source with audio and no picture. An H.264 source needs both ends on this build of
libomt.dll: an older OMT-HX receiver, like a stock OMT program, sees it with audio and no picture.
See sections 3 and 17 of the design.

## Licence

MIT. `libomt.dll` and `libvmx.dll` are MIT, built from forks of the Open Media Transport project;
only libomtnet carries changes.