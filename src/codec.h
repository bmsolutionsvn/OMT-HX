#pragma once
// Internal to omthx. Apps include omthx/omthx.h only.
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "omthx/omthx.h"

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace omthx {

inline constexpr int kCodecHevc = 0x43564548;  // 'HEVC' as OMT writes FourCCs
inline constexpr int kCodecH264 = 0x34363248;  // 'H264'
inline constexpr char kKeyframeRequest[] = "<OMTHX Keyframe=\"true\" />";

// ---- pure functions (src/pure.cpp) ----
int codecTag(Codec codec);                 // the tag an Hx frame of this codec carries
bool codecFromTag(int tag, Codec& out);    // false for a tag that is not an Hx codec
const char* codecName(Codec codec);        // "hevc" or "h264"
int64_t targetBitrate(const VideoFormat& format, Quality quality, Codec codec);
bool hasNalType(Codec codec, std::span<const uint8_t> annexB, int type);
bool isKeyframe(Codec codec, std::span<const uint8_t> annexB);
int alphaTop(int height);                       // row where the alpha picture starts
int codedHeight(const VideoFormat& format);     // height of the picture the encoder sees
bool isGap(int64_t previous, int64_t current, int fpsN, int fpsD);

// Decides when the sender must force a keyframe (spec section 8). Time is passed in, so it is testable.
class KeyframePolicy {
 public:
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
class Packer {
 public:
  Packer() = default;
  ~Packer();
  Packer(const Packer&) = delete;
  Packer& operator=(const Packer&) = delete;
  void pack(const VideoFormat& format, std::span<const uint8_t> bgra, bool premultiplied, Nv12& out);
  void unpack(const VideoFormat& format, const Nv12& in, std::vector<uint8_t>& bgra);  // premultiplied when alpha

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

// Hardware HEVC through FFmpeg (src/encoder.cpp). Added in Task 4.
class Encoder {
 public:
  Encoder(int width, int height, int fpsN, int fpsD, int64_t bitrate);
  ~Encoder();
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;
  static const std::vector<std::string>& order();
  const std::string& name() const { return name_; }
  // Usually one packet per picture. An encoder may hold one picture back; packets come out in order.
  std::vector<Packet> encode(const Nv12& picture, bool forceKeyframe);

 private:
  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  std::string name_;
  int64_t index_ = 0;
};

enum class Decoded { Picture, NeedMore, Error };

// HEVC through FFmpeg, D3D11 when the GPU offers it (src/decoder.cpp). Added in Task 4.
class Decoder {
 public:
  Decoder();
  ~Decoder();
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  const std::string& name() const { return name_; }
  // timestamp travels with the access unit and comes back in outTimestamp with its picture.
  Decoded decode(std::span<const uint8_t> accessUnit, int64_t timestamp, Nv12& out, int64_t& outTimestamp);

 private:
  AVCodecContext* ctx_ = nullptr;
  AVBufferRef* hw_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVFrame* sw_ = nullptr;
  AVPacket* packet_ = nullptr;
  std::string name_;
};

}  // namespace omthx
