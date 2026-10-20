#pragma once
// Internal to omthx. Apps include omthx/omthx.h only.
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "omthx/omthx.h"

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct VMX_INSTANCE;

namespace omthx {

inline constexpr int kCodecHevc = 0x43564548;  // 'HEVC' as OMT writes FourCCs
inline constexpr int kCodecH264 = 0x34363248;  // 'H264'
inline constexpr char kKeyframeRequest[] = "<OMTHX Keyframe=\"true\" />";

// ---- pure functions (src/pure.cpp) ----
uint8_t mulDiv255(unsigned value, unsigned alpha);  // value * alpha / 255, rounded; both 0 to 255
int codecTag(Codec codec);                 // the tag an Hx frame of this codec carries
bool codecFromTag(int tag, Codec& out);    // false for a tag that is not an Hx codec
const char* codecName(Codec codec);        // "hevc" or "h264"
int64_t targetBitrate(const VideoFormat& format, Quality quality, Codec codec);  // one stream, alpha stacked
// With VMX alpha the GPU codes the fill alone, at the bitrate of the same source without alpha.
int64_t fillBitrate(const VideoFormat& format, Quality quality, Codec codec);
bool hasNalType(Codec codec, std::span<const uint8_t> annexB, int type);
bool isKeyframe(Codec codec, std::span<const uint8_t> annexB);
// Alpha travels one of two ways. Stacked (sources before 2026-10-21): one picture, the alpha picture under the fill.
// VMX: the GPU codes the fill alone, at the display size, and the CPU codes the alpha plane as one grey VMX plane;
// each OMT frame carries the fill's access unit, then the VMX alpha, and its metadata says how long the VMX alpha is,
// or that the frame repeats the previous frame's alpha and carries none.
int alphaTop(int height);                       // stacked: row where the alpha picture starts
int codedHeight(const VideoFormat& format);     // stacked: height of the picture the encoder sees
bool vmxAlphaFits(const VideoFormat& format);   // VMX takes 16x16 to 7680x4320; smaller or larger alpha is stacked
bool isGap(int64_t previous, int64_t current, int fpsN, int fpsD);
// Per-frame metadata that numbers each coded frame, so a receiver can tell a frame lost on the way
// (the number jumps) from a frame the app never sent (only the timestamp jumps).
// What an HX frame's metadata says about VMX alpha.
struct AlphaPart {
  size_t bytes = 0;      // the frame ends with this many bytes of VMX alpha
  bool repeats = false;  // the frame carries no alpha: the previous frame's alpha holds
  bool any() const { return bytes > 0 || repeats; }
};
std::string seqMetadata(uint64_t seq, AlphaPart alpha = {});
bool parseSeq(std::string_view metadata, uint64_t& out);  // false when the metadata carries no number
AlphaPart parseAlpha(std::string_view metadata);           // nothing set for a frame without VMX alpha

// Decides when the sender must force a keyframe (spec section 8). Time is passed in, so it is testable.
class KeyframePolicy {
 public:
  // A backstop only: receivers ask for a keyframe when they join or lose a frame, within 500 ms. Each periodic
  // keyframe is a bitrate spike or a quality dip on a two-frame rate buffer, so they are rare.
  static constexpr int64_t kPeriodicMs = 10'000;
  bool next(int64_t nowMs, uint64_t dropped, bool requested);

 private:
  bool first_ = true;
  bool pending_ = false;
  uint64_t dropped_ = 0;
  int64_t lastKeyMs_ = 0;
};

struct Nv12 {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> y;   // width * height
  std::vector<uint8_t> uv;  // width * height / 2, U and V interleaved
};

// BGRA to the packed NV12 picture and back (spec section 7). Added in Task 3.
// An NV12 picture with any row strides into `out` (full range). Limited range is stretched to full.
void copyNv12(const VideoFormat& format, const uint8_t* y, int yStride, const uint8_t* uv, int uvStride, bool fullRange,
              Nv12& out);
// The pack functions stack alpha under the fill when `alphaOut` is null, and otherwise put the fill alone in `out`
// and the width * height alpha plane in `alphaOut`. The unpack functions read alpha from the width * height plane
// `alpha` when set, and from under the fill otherwise.

// UYVY (limited range) and, with alpha, its alpha plane to the packed NV12 picture, with no colour matrix:
// values are re-ranged by table and chroma rows averaged 4:2:2 to 4:2:0. Straight colour is premultiplied in YUV.
void packUyvy(const VideoFormat& format, const uint8_t* uyvy, int stride, const uint8_t* alpha, int alphaStride,
              bool premultiplied, Nv12& out, std::vector<uint8_t>* alphaOut = nullptr);
// Back to UYVY rows of width * 2 bytes, followed with alpha by the width * height alpha plane: the UYVA layout
// OMT and NDI share. Chroma rows repeat; colour stays premultiplied when there is alpha.
void unpackUyvy(const VideoFormat& format, const Nv12& in, std::vector<uint8_t>& uyva,
                const uint8_t* alpha = nullptr);

class Packer {
 public:
  Packer() = default;
  ~Packer();
  Packer(const Packer&) = delete;
  Packer& operator=(const Packer&) = delete;
  void pack(const VideoFormat& format, std::span<const uint8_t> bgra, bool premultiplied, Nv12& out,
            std::vector<uint8_t>* alphaOut = nullptr);
  void unpack(const VideoFormat& format, const Nv12& in, std::vector<uint8_t>& bgra,
              const uint8_t* alpha = nullptr);  // premultiplied when alpha

 private:
  void ensure(int width, int height);
  SwsContext* toNv12_ = nullptr;
  SwsContext* toBgra_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  std::vector<uint8_t> scratch_;
};

struct Packet {
  std::vector<uint8_t> data;  // one access unit, Annex B
  bool keyframe = false;
};

// Hardware HEVC or H.264 through FFmpeg (src/encoder.cpp).
class Encoder {
 public:
  // gpuFrames: null for NV12 pictures in memory; else a D3D11 frames context, whose pictures encode() then takes
  // with encodeGpu. Throws std::runtime_error when no hardware encoder of this codec opens.
  Encoder(Codec codec, int width, int height, int fpsN, int fpsD, int64_t bitrate, AVBufferRef* gpuFrames = nullptr);
  ~Encoder();
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;
  static const std::vector<std::string>& order(Codec codec);
  Codec codec() const { return codec_; }
  const std::string& name() const { return name_; }
  // One packet per picture, normally this picture's: the call waits up to one frame time for it. A picture
  // that takes longer comes out with a later call; packets come out in order.
  std::vector<Packet> encode(const Nv12& picture, bool forceKeyframe);
  std::vector<Packet> encodeGpu(const AVFrame* picture, bool forceKeyframe);  // a frame of gpuFrames

 private:
  std::vector<Packet> send(bool forceKeyframe);  // frame_ is set up
  std::vector<Packet> drain();

  Codec codec_ = Codec::Hevc;
  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  std::string name_;
  int64_t index_ = 0;
  int64_t owed_ = 0;         // pictures sent whose packet has not come out yet
  int64_t frameUs_ = 16'667;  // how long encode waits for its packet
  void* timer_ = nullptr;     // a high-resolution waitable timer for the short waits
};

// The alpha plane as one grey VMX plane, on the CPU (src/alpha.cpp). VMX is intra only: each frame stands alone.
class AlphaEncoder {
 public:
  AlphaEncoder(int width, int height, Quality quality);  // throws std::runtime_error when VMX refuses the size
  ~AlphaEncoder();
  AlphaEncoder(const AlphaEncoder&) = delete;
  AlphaEncoder& operator=(const AlphaEncoder&) = delete;
  static int vmxQuality(Quality quality);  // a fixed VMX quality per Quality, no bitrate targeting
  void encode(const uint8_t* plane, int stride, std::vector<uint8_t>& out);

 private:
  ::VMX_INSTANCE* vmx_ = nullptr;
  int width_ = 0;
  int height_ = 0;
};

class AlphaDecoder {
 public:
  AlphaDecoder() = default;
  ~AlphaDecoder();
  AlphaDecoder(const AlphaDecoder&) = delete;
  AlphaDecoder& operator=(const AlphaDecoder&) = delete;
  // False for data VMX cannot read at this size. `out` becomes the width * height plane.
  bool decode(std::span<const uint8_t> data, int width, int height, std::vector<uint8_t>& out);

 private:
  ::VMX_INSTANCE* vmx_ = nullptr;
  int width_ = 0;
  int height_ = 0;
};

enum class Decoded { Picture, NeedMore, Error };

// HEVC or H.264 through FFmpeg, D3D11 when the GPU offers it (src/decoder.cpp).
class Decoder {
 public:
  explicit Decoder(Codec codec);
  ~Decoder();
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  Codec codec() const { return codec_; }
  const std::string& name() const { return name_; }
  // timestamp travels with the access unit and comes back in outTimestamp with its picture.
  Decoded decode(std::span<const uint8_t> accessUnit, int64_t timestamp, Nv12& out, int64_t& outTimestamp);
  // The same, leaving the picture where the decoder put it: `out` gets a reference to a D3D11 frame, or to a
  // frame in memory when decoding runs in software.
  Decoded decodeFrame(std::span<const uint8_t> accessUnit, int64_t timestamp, AVFrame* out, int64_t& outTimestamp);

 private:
  Codec codec_ = Codec::Hevc;
  AVCodecContext* ctx_ = nullptr;
  AVBufferRef* hw_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  std::string name_;
};

// A decoded frame, from video or system memory, copied into an NV12 picture. False for a frame omthx cannot
// read (10-bit, 4:2:2, odd sizes) or a failed copy from video memory.
bool downloadNv12(const AVFrame* frame, Nv12& out);

// What a Gpu receiver hands out: a reference to one decoded frame, freed with the last copy of the pointer.
struct GpuPicture {
  AVFrame* frame = nullptr;  // the fill, or with a stacked source the whole picture
  std::shared_ptr<const std::vector<uint8_t>> alpha;  // VMX alpha, decoded: the width * height plane; or null
  GpuPicture();
  ~GpuPicture();
  GpuPicture(const GpuPicture&) = delete;
  GpuPicture& operator=(const GpuPicture&) = delete;
  bool inVideoMemory() const;  // a D3D11 frame, rather than a software decoder's frame in memory
};

}  // namespace omthx
