# OMT-HX: design

**Status:** approved by the owner 2026-10-12, with the MIT licence. Section 5 was
proven by a throwaway build on 2026-10-13. Sections 1 to 17 are implemented, tested and measured; HX
ships HEVC and H.264, with the figures in `docs/MEASUREMENTS.md`. Section 17, added 2026-10-13,
makes H.264 a second HX codec; where sections 1 to 16 say HEVC, section 17 says what differs.

**Parent decisions:** broadcast roadmap R11 and R12
(`../../../../docs/superpowers/specs/2026-10-12-broadcast-roadmap-design.md`).

## 1. What this is

OMT-HX is a second video mode for Open Media Transport, in the way NDI|HX is a
second mode of NDI. Plain OMT sends every frame on its own with the VMX codec,
at about 200 Mbps for 1080p60. OMT-HX sends HEVC from the GPU's hardware
encoder, at about 30 Mbps for the same picture.

Everything except the video codec stays OMT: sources are found the same way,
audio and tally travel the same way, and one receiver reads both modes.

It exists for the link from BreadCamera to BreadStudio (roadmap R11). It is a
shared library in its own repository, used by both apps.

**Success means:**

- A BreadCamera output can be switched between OMT and OMT-HX per camera, and
  BreadStudio shows either one from the same input.
- Four 1080p60 cameras with alpha fit in a 1 GbE link with room to spare.
- The link adds no more delay than plain OMT does.
- Programs that only speak stock OMT, such as vMix, keep working with plain
  OMT sources from the same library build.

## 2. Decisions

| # | Topic | Decision |
|---|---|---|
| H1 | Relation to OMT | HX is a codec inside OMT, not a new protocol. Discovery, connection, audio, tally and metadata are OMT's, unchanged. |
| H2 | Carriage | HEVC frames travel as normal OMT video frames with the codec tag `HEVC`. This needs a small patch to the OMT library (section 5), because stock libomt refuses any tag but its own. |
| H3 | Codec | HEVC Main, 8-bit 4:2:0, full-range BT.709, progressive. No B-frames. H.264 (AVC) is a second codec, added in section 17. |
| H4 | Encoder | The GPU's hardware encoder through FFmpeg. The first of `hevc_nvenc`, `hevc_amf`, `hevc_qsv`, `hevc_mf` that opens is used. This is the pattern BreadCamera's preview encoder already uses. No software encoder. |
| H5 | Decoder | FFmpeg's HEVC decoder, with D3D11 hardware decoding when the GPU offers it. Software decoding is the fallback; it uses several threads and so adds about three frames of delay. |
| H6 | Alpha | Alpha is off by default (the format's `Alpha` flag). When on, fill and alpha are packed into one picture: fill on top, alpha below as a grey image (section 7). Fill is premultiplied on the wire. |
| H7 | Bitrate | Constant bitrate from bits per pixel, by quality level: Low 0.12, Medium 0.24, High 0.40. At 1080p60 that is 15, 30 and 50 Mbps. Alpha adds 25%. These are starting values, tuned by measurement in milestone H2. |
| H8 | Keyframes | A keyframe is sent at start, when a receiver asks for one, when the library reports a dropped frame, and every 2 seconds (section 8). |
| H9 | One library for both modes | `omthx` wraps libomt. Its sender sends plain OMT or OMT-HX; its receiver reads both and returns the same frame type. Apps use `omthx` and never call libomt directly. |
| H10 | Repository | `F:\Coding\BreadMedia\OMT-HX`, its own git repository, beside the apps. BreadCamera and BreadStudio both build against it (section 10). |
| H11 | Platform | Windows x64 only, like BreadCamera and BreadStudio S0. |
| H12 | Out of scope | The sliced and inter-frame VMX ideas from the research. They need a transport or codec of our own and save at most one frame on this link. |

## 3. How it fits

```
BreadCamera (Camera PC)                         BreadStudio (Studio PC)
  camera pipeline                                 input plugin
        |  BGRA + audio                                ^  BGRA + audio
        v                                              |
  omthx::Sender --- OMT connection (TCP) ---> omthx::Receiver
    mode Full: VMX frame                          VMX  -> libomt decodes
    mode HX:   HEVC frame, tag 'HEVC'             HEVC -> omthx decodes
        ^                                              |
        +------------- tally (preview, program) -------+

  discovery: OMT's DNS-SD, unchanged      audio: OMT's, unchanged
```

A stock OMT program that connects to an HX source gets audio and no picture,
and logs an unsupported codec. That is the same situation as an old NDI
receiver meeting an NDI|HX source. An operator who needs both adds two outputs
to the camera, one OMT and one OMT-HX; BreadCamera already allows several
outputs per camera.

## 4. Wire format, version 1

An HX video frame is an OMT Protocol 1.0 video frame. Only the codec tag and
the payload are new.

| Field | Value |
|---|---|
| Codec | `HEVC`, the 32-bit value `0x43564548` |
| Width, Height | The picture as displayed, for example 1920 x 1080. With alpha the coded picture is taller (section 7); the display size stays here. |
| FrameRateN / FrameRateD | As in OMT |
| Flags | `Alpha` (2) when alpha is present, together with `PreMultiplied` (4). `Interlaced` is never set. |
| ColorSpace | 709 |
| Timestamp | As in OMT: the time the frame was made at the source, 10,000,000 units per second |
| Data | One HEVC access unit in Annex B form (start codes). Every keyframe carries VPS, SPS and PPS in front of it. |

There is no separate version field. A later format, such as 10-bit or AV1,
gets its own codec tag.

## 5. The libomt patch

Stock libomt (libomtnet commit `7d29031`, libomt commit `bb4b67b`, release
v1.0.0.19) was read for this design:

- **Sending.** `OMTSend.SendVideo` accepts raw pixel formats and encodes them
  with VMX, or accepts ready-made `VMX1` data and passes it through. Any other
  tag is rejected with "Codec not supported".
- **Receiving.** `OMTReceive.ReceiveVideo` handles only `VMX1`. Any other tag
  is logged and the frame is discarded.
- **The C wrapper** passes the frame struct straight to the .NET library, so
  it needs no change. `omthx` defines the `HEVC` tag value itself.

The patch, 32 added lines of C# in three files:

1. Add `HEVC = 0x43564548` to `OMTCodec`.
2. In `SendVideo`, treat `HEVC` exactly like the existing `VMX1` pass-through,
   keeping the `HEVC` tag on the outgoing frame.
3. In `ReceiveVideo`, when the tag is `HEVC`, skip decoding and hand the bytes
   to the caller through the existing `CompressedData` and `CompressedLength`
   fields, with `Codec` set to `HEVC`. Allocate that buffer on first use.

Nothing changes for any codec stock OMT knows, so the patched library still
talks to vMix and every other OMT program.

The patch is kept as a file, `patches/libomtnet-hx.patch`, against the pinned
commit. A script clones the two upstream repositories, applies it, and builds
`libomt.dll` with `dotnet publish -r win-x64 -c Release` (.NET 8 SDK, already
on this PC). `libvmx.dll` comes from the pinned upstream release, checked by
SHA-256. No fork repository is needed.

**Proven on this PC, 2026-10-13.** A clean clone, patched and built by the
script, passed a throwaway test: 1000 bytes sent with the `HEVC` tag arrived
byte for byte with size and flags intact; a UYVY frame still went through VMX
and came back as UYVY; tally set on the receiver was read on the sender; a
metadata message from the receiver reached the sender; audio arrived unchanged.
The .NET 8 build does not find Visual Studio 2026 by itself, so the script
runs inside the Visual Studio developer shell.

## 6. The `omthx` library

C++20, CMake, vcpkg, doctest: the same toolchain as BreadCamera.

| Unit | What it does | Depends on |
|---|---|---|
| `bitrate` | Target bitrate from size, frame rate, quality and alpha. Pure function. | nothing |
| `nal` | Says whether an access unit is a keyframe, from its NAL unit types. Pure function. | nothing |
| `pack` | BGRA to the packed NV12 picture and back, including premultiplying. Pure functions. | nothing |
| `Encoder` | Opens the first hardware HEVC encoder that works, encodes one picture to one access unit, forces a keyframe on request. | FFmpeg |
| `Decoder` | Decodes one access unit to an NV12 picture. Hardware when available. | FFmpeg |
| `Sender` | One OMT source. Mode Full or HX. Takes BGRA, audio and a timestamp; applies the keyframe rules; reports tally and statistics. | libomt, `Encoder`, `pack`, `bitrate` |
| `Receiver` | Connects to one OMT source by name. Returns BGRA frames whichever mode the source uses; waits for a keyframe after connecting; sets tally; reports statistics. | libomt, `Decoder`, `pack`, `nal` |
| `omthx-send` | Test tool. Sends a moving test pattern with grain as an OMT or OMT-HX source. | `Sender` |
| `omthx-recv` | Test tool. Connects to a source and prints frame rate, Mbps, encode and decode time, drops and delay once a second. Can save a frame as an image. | `Receiver` |

Apps see `Sender` and `Receiver` only. That keeps roadmap R12's rule: one
output adapter in BreadCamera and one input plugin in BreadStudio depend on
the link, and nothing else does.

## 7. Alpha packing

Hardware HEVC encoders have no dependable alpha support across vendors, so
alpha rides as part of the picture.

- Let `A` be the display height rounded up to a multiple of 16 (1088 for 1080).
- The coded picture is `Width` x `2A`.
- Rows `0` to `Height-1` hold the fill. Rows `A` to `A+Height-1` hold alpha
  as brightness, with neutral colour. Rows between repeat the row above.
- The fill is multiplied by alpha before encoding. Transparent areas become
  black, which compresses well and avoids coloured fringes at edges.

Rounding to 16 keeps the seam between fill and alpha on a coding-block
boundary, so the two halves do not smear into each other.

Without the `Alpha` flag the coded picture is just the display picture.

## 8. Keyframes and recovery

HEVC frames depend on the frames before them, so a receiver that joins late or
misses a frame needs a keyframe.

The sender forces one when:

1. it sends its first frame;
2. a receiver asks for one, with the OMT metadata message
   `<OMTHX Keyframe="true" />`;
3. libomt's send statistics report a newly dropped frame, which happens when a
   receiver's connection is too slow to take a frame;
4. the app says it skipped a frame (BreadCamera drops the oldest frame when an
   output falls behind);
5. 2 seconds have passed since the last keyframe.

Forced keyframes are limited to one per 500 ms. Without the limit a saturated
link would drop a frame, get a larger keyframe, and drop again.

The receiver discards frames until the first keyframe after connecting, after
a decoder error, and after a jump in timestamps of more than one and a half
frames, which is how a dropped frame looks from its side. While it is
discarding it asks for a keyframe, at most once per 500 ms. OMT runs over TCP,
so frames are never lost in transit; they are only dropped at the sender.

Because of the timestamp rule, a sender must stamp frames one frame duration
apart. BreadCamera's tick clock already does.

## 9. Encoder settings

- Constant bitrate from H7, with a buffer of about two frames.
- No B-frames, one reference frame, each encoder's lowest-delay preset, in the
  same per-encoder table style as BreadCamera's `preview_encoder.cpp`.
- If no hardware encoder opens, the sender fails with a clear error naming
  what it tried. The operator then uses plain OMT for that camera.

## 10. Sharing across projects

OMT-HX sits beside the apps:

```
F:\Coding\BreadMedia\
  OMT-HX\          this repository
  BreadCamera\     output adapter built on omthx::Sender
  BreadStudio\     input plugin built on omthx::Receiver
```

- OMT-HX builds a static library `omthx` and exports a CMake target.
- Each app has one CMake variable, `OMTHX_ROOT`, which defaults to the sibling
  folder `../OMT-HX`, and adds it with `add_subdirectory`.
- When OMT-HX gets a remote, the apps pin a commit of it instead (a git
  submodule), so their CI can build without the sibling folder. BreadStudio
  will need this before its CI builds the input plugin.
- The patched `libomt.dll` and `libvmx.dll` ship next to each app's
  executable. Both apps use this build for plain OMT as well.

## 11. Milestones

| # | Milestone | Delivers | Done when |
|---|---|---|---|
| H0 | Patched libomt | The patch file, the build script, the pinned versions. | On one PC, a UYVY frame sent through VMX arrives as before, and a block of bytes sent with the `HEVC` tag arrives byte for byte. |
| H1 | Codec layer | `bitrate`, `nal`, `pack`, `Encoder`, `Decoder`, with tests. | A picture with alpha survives pack, encode, decode and unpack with the alpha plane within a small tolerance. |
| H2 | Link and tools | `Sender`, `Receiver`, both tools, and `docs/MEASUREMENTS.md`. | `omthx-send` to `omthx-recv` on this PC runs 1080p60 with alpha for 10 minutes with no decode error, recovers after a forced drop within one keyframe, and the measured bitrate, encode time, decode time and delay are written down. |

Not in this repository: the BreadCamera output adapter (BreadCamera M4) and
the BreadStudio input plugin (Studio S2). Each is a thin wrapper and belongs
to its app's own spec.

## 12. Testing

- **Pure units** (`bitrate`, `nal`, `pack`): doctest unit tests, written first.
- **Encoder and Decoder:** a round-trip test that is skipped, with a visible
  message, when the PC has no hardware HEVC encoder.
- **Link:** a loopback test on `localhost` through the patched libomt: frames
  arrive in order, the first frame delivered is a keyframe, tally set on the
  receiver is read on the sender, and a plain OMT source is received through
  the same `Receiver`.
- **Compatibility:** the H0 test proves the patch did not change plain OMT.

**This PC has an AMD Radeon RX 6600 and no NVIDIA GPU.** Every test here runs
on `hevc_amf`. The `hevc_nvenc` path, which is the one BreadCamera's target
PC will use, must be run by the owner on an NVIDIA machine before it is
called working.

## 13. Known limits of version 1

- Frames are converted between BGRA and NV12 on the CPU on both sides. The
  later step is to give the encoder and take from the decoder a D3D11 texture.
- Keyframes are whole frames, so bitrate spikes every 2 seconds. The later
  step is gradual intra refresh.
- 8-bit 4:2:0 only. Plain OMT stays the mode for 4:2:2 and 10-bit.
- One bitrate per source, shared by all its receivers.
- A slow receiver forces keyframes for every receiver of that source.

## 14. Risks

| Risk | Handling |
|---|---|
| The patched library drifts from upstream OMT. | The patch is one small file against a pinned commit, re-applied on each upgrade. Offering it upstream is an owner decision (section 15). |
| Low-delay behaviour differs between NVIDIA, AMD and Intel encoders. | Per-encoder settings table; measurements recorded per encoder in `MEASUREMENTS.md`. |
| Hardware encoder session limits. NVIDIA allows 8 at once per system on consumer cards, and BreadCamera also encodes previews and SRT outputs. | `Sender` reports a clear error when the encoder will not open. BreadCamera's spec states the budget. |
| HEVC is covered by patent pools. | OMT-HX ships no HEVC encoder of its own; it drives the one in the GPU driver. Decoding uses FFmpeg, as OBS itself does. The owner should confirm this is acceptable; AV1 is the royalty-free alternative but this PC's GPU cannot encode it. |
| The .NET runtime inside libomt pauses for garbage collection. | Measure frame-time jitter in H2, not only the average. HX frames are about 60 KB, far smaller than VMX frames, which lowers the pressure. |

## 15. Open decisions for the owner

1. **Licence of this repository.** Decided 2026-10-12: MIT.
2. **Carriage (H2).** Recommended: the patched libomt. The alternative is a
   second TCP connection beside an unpatched libomt; it avoids the patch but
   adds a connection, a port, address discovery and reconnect logic of our own.
3. **Premultiplied fill on the wire (H6).** Recommended as written.
4. **Offering the patch to the OMT project.** Not needed to ship. It is an
   outward-facing step and waits for the owner's word.
5. **No remote yet.** The repository is local only. Creating a GitHub
   repository and pushing are the owner's to trigger.

## 16. Alternatives considered

| Alternative | Why not |
|---|---|
| A second TCP connection for HEVC beside stock libomt | No patch, but libomt's C API does not expose the sender's IP address, so address discovery, reconnecting and frame queues would be written again. More new code than the patch. |
| Our own C++ implementation of the OMT protocol | Removes the .NET library, but rewrites discovery, tally, audio and reconnecting that already work. |
| Improving VMX (slices over UDP, block skipping, residual coding, NVENC motion vectors) | A transport and a codec of our own, no longer readable by any other OMT program, for at most one frame saved on this link. |
| NVENC only, through NVIDIA's SDK | Cannot be developed or tested on this PC, and BreadCamera already runs on non-NVIDIA GPUs with its ONNX fallback. |
| NDI\|HX | Sending HX needs NDI's Advanced SDK, and its closed runtime cannot ship inside a GPL BreadStudio. |

## 17. H.264 as a second HX codec

Added 2026-10-13. An HX source sends HEVC or H.264, chosen per source by the
sender, the way NDI HX3 offers both. HEVC stays the default and is unchanged.

| # | Topic | Decision |
|---|---|---|
| D1 | Who chooses | The sender, per source: `Codec::Hevc` (the default) or `Codec::H264`. The receiver has no setting; it reads the tag on each frame. |
| D2 | Carriage | A second codec tag, `H264`, the 32-bit value `0x34363248`. The libomt patch passes it through exactly as it passes `HEVC`; the patch is now 32 added lines. |
| D3 | H.264 on the wire | 8-bit 4:2:0, full-range BT.709, progressive, no B-frames, Annex B. Every keyframe is an IDR picture with SPS and PPS in the same access unit. The profile is the encoder's choice. |
| D4 | Encoder | The first of `h264_nvenc`, `h264_amf`, `h264_qsv`, `h264_mf` that opens, with the same per-vendor low-delay settings as HEVC. No software encoder. |
| D5 | No fallback between codecs | If the chosen codec has no hardware encoder, the sender fails with an error naming what it tried. |
| D6 | Bitrate | 1.4 times the HEVC bitrate of the same quality level: 21, 42 and 70 Mbps at 1080p60. A starting value; picture quality has not been compared by eye. |
| D7 | Codec change on a live source | When a source comes back with the other codec, the receiver rebuilds its decoder and waits for a keyframe. |

Why: H.264 has a hardware encoder on every GPU, including ones too old for
HEVC, and it is the simpler codec. Measured figures for both are in
`docs/MEASUREMENTS.md`.

What H.264 does not change:

- It is covered by a patent pool, as HEVC is (section 14). It is not the
  royalty-free option; AV1 still is.
- Alpha packing, keyframe rules, discovery, audio and tally are the same for
  both codecs.
- A receiver built before this section, like a stock OMT program, sees an
  H.264 source with audio and no picture.

One difference in behaviour: left alone, AMD's H.264 encoder sends very few
IDR pictures, so H.264 depends on the sender forcing a keyframe every
2 seconds (section 8). It already does.
