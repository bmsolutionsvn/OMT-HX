# OMT-HX

A second video mode for [Open Media Transport](https://github.com/openmediatransport), in the way
NDI|HX is a second mode of NDI: HEVC from the GPU's hardware encoder inside ordinary OMT
connections. Discovery, audio and tally are OMT's, unchanged.

`omthx` is the library BreadCamera (sender) and BreadStudio (receiver) share. One `Sender` sends
plain OMT or OMT-HX; one `Receiver` reads both.

- Design: [docs/superpowers/specs/2026-10-12-omt-hx-design.md](docs/superpowers/specs/2026-10-12-omt-hx-design.md)
- Measured figures: [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md)

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

Timestamps must be one frame duration apart. A larger step tells the receiver a frame was
skipped, and it holds the last picture until the next keyframe.

## Limits of version 1

8-bit 4:2:0 only. Colour conversion runs on the CPU. A program that speaks only OMT sees an
OMT-HX source with audio and no picture. See section 13 of the design.

## Licence

MIT. The bundled `libomt.dll` and `libvmx.dll` are MIT, from the Open Media Transport project,
with one patch.