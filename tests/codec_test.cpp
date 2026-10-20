#include <doctest/doctest.h>

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "codec.h"
#include "pattern.h"

using namespace omthx;

namespace {
// Null, with a visible message, on a PC that has no hardware encoder for this codec.
std::unique_ptr<Encoder> openEncoder(Codec codec, const VideoFormat& f) {
  try {
    return std::make_unique<Encoder>(codec, f.width, codedHeight(f), f.fpsN, f.fpsD,
                                     targetBitrate(f, Quality::Medium, codec));
  } catch (const std::runtime_error& e) {
    MESSAGE("SKIPPED, no hardware " << codecName(codec) << " encoder on this PC: " << e.what());
    return nullptr;
  }
}

std::vector<Packet> encodePattern(Encoder& enc, const VideoFormat& f, int frames, int keyAt) {
  Packer packer;
  Nv12 pic;
  std::vector<uint8_t> bgra;
  std::vector<Packet> all;
  for (int i = 0; i < frames; ++i) {
    drawPattern(bgra, f.width, f.height, i, f.alpha);
    packer.pack(f, bgra, false, pic);
    for (Packet& p : enc.encode(pic, i == keyAt)) all.push_back(std::move(p));
  }
  return all;
}

// The NAL types a keyframe must carry so a late joiner can start from it.
std::vector<int> parameterSets(Codec codec) {
  return codec == Codec::H264 ? std::vector<int>{7, 8} : std::vector<int>{32, 33, 34};  // SPS, PPS; VPS, SPS, PPS
}

bool startsWith(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }
}  // namespace

TEST_CASE("Encoder::order lists four hardware encoders of the chosen codec, NVIDIA first") {
  CHECK(Encoder::order(Codec::Hevc) == std::vector<std::string>{"hevc_nvenc", "hevc_amf", "hevc_qsv", "hevc_mf"});
  CHECK(Encoder::order(Codec::H264) == std::vector<std::string>{"h264_nvenc", "h264_amf", "h264_qsv", "h264_mf"});
}

TEST_CASE("Encoder: a picture no hardware takes throws and names every encoder of that codec") {
  Codec codec = Codec::Hevc;
  SUBCASE("hevc") { codec = Codec::Hevc; }
  SUBCASE("h264") { codec = Codec::H264; }
  std::string what;
  try {
    Encoder enc(codec, 16384, 16384, 60, 1, 1'000'000);  // larger than any level of either codec
  } catch (const std::runtime_error& e) {
    what = e.what();
  }
  REQUIRE_FALSE(what.empty());
  for (const std::string& name : Encoder::order(codec)) CHECK(what.find(name) != std::string::npos);
  const Codec other = codec == Codec::Hevc ? Codec::H264 : Codec::Hevc;
  for (const std::string& name : Encoder::order(other)) CHECK(what.find(name) == std::string::npos);  // no fallback
}

TEST_CASE("Encoder: first packet is a self-contained keyframe, then no keyframes until one is forced") {
  Codec codec = Codec::Hevc;
  SUBCASE("hevc") { codec = Codec::Hevc; }
  SUBCASE("h264") { codec = Codec::H264; }
  const VideoFormat f{1280, 720, 60, 1, false};
  auto enc = openEncoder(codec, f);
  if (!enc) return;
  MESSAGE("encoder: " << enc->name());
  CHECK(enc->codec() == codec);
  CHECK(startsWith(enc->name(), codecName(codec)));
  const std::vector<Packet> all = encodePattern(*enc, f, 30, 20);
  REQUIRE(all.size() >= 29);  // low delay: the encoder holds back one picture at most
  CHECK(all[0].keyframe);
  for (int type : parameterSets(codec)) CHECK(hasNalType(codec, all[0].data, type));
  for (size_t i = 1; i < 20; ++i) CHECK_FALSE(all[i].keyframe);
  CHECK(all[20].keyframe);
  for (int type : parameterSets(codec)) CHECK(hasNalType(codec, all[20].data, type));  // a late joiner can start here
}

TEST_CASE("a picture with alpha survives pack, encode, decode and unpack") {
  Codec codec = Codec::Hevc;
  SUBCASE("hevc") { codec = Codec::Hevc; }
  SUBCASE("h264") { codec = Codec::H264; }
  const VideoFormat f{1280, 720, 60, 1, true};
  auto enc = openEncoder(codec, f);
  if (!enc) return;
  Decoder dec(codec);
  Packer packer;
  Nv12 pic, decoded;
  std::vector<uint8_t> src, back;
  drawPattern(src, f.width, f.height, 0, true);  // a still picture, so any decoded frame can be compared with it
  int pictures = 0;
  for (int i = 0; i < 30; ++i) {
    packer.pack(f, src, false, pic);
    for (const Packet& p : enc->encode(pic, false)) {
      int64_t ts = 0;
      if (dec.decode(p.data, i, decoded, ts) != Decoded::Picture) continue;
      packer.unpack(f, decoded, back);
      ++pictures;
    }
  }
  MESSAGE("decoder: " << dec.name());
  CHECK(dec.codec() == codec);
  CHECK(startsWith(dec.name(), codecName(codec)));
  REQUIRE(pictures >= 20);
  REQUIRE(back.size() == src.size());
  double alphaError = 0, colourError = 0;
  const size_t pixels = static_cast<size_t>(f.width) * f.height;
  for (size_t i = 0; i < pixels; ++i) {
    const int a = src[i * 4 + 3];
    alphaError += std::abs(back[i * 4 + 3] - a);
    for (int k = 0; k < 3; ++k) colourError += std::abs(back[i * 4 + k] - (src[i * 4 + k] * a + 127) / 255);
  }
  alphaError /= pixels;
  colourError /= pixels * 3.0;
  MESSAGE("mean alpha error " << alphaError << ", mean colour error " << colourError << " (0 to 255)");
  CHECK(alphaError < 3.0);
  CHECK(colourError < 6.0);
}

TEST_CASE("a picture with alpha survives as two streams: the fill and alpha, each with its own encoder") {
  Codec codec = Codec::Hevc;
  SUBCASE("hevc") { codec = Codec::Hevc; }
  SUBCASE("h264") { codec = Codec::H264; }
  const VideoFormat f{1280, 720, 60, 1, true};
  std::unique_ptr<Encoder> fillEnc, alphaEnc;
  try {
    fillEnc = std::make_unique<Encoder>(codec, f.width, f.height, f.fpsN, f.fpsD, fillBitrate(f, Quality::Medium, codec));
    alphaEnc = std::make_unique<Encoder>(codec, f.width, f.height, f.fpsN, f.fpsD, alphaBitrate(f, Quality::Medium, codec));
  } catch (const std::runtime_error& e) {
    MESSAGE("SKIPPED, no two hardware " << codecName(codec) << " encoders on this PC: " << e.what());
    return;
  }
  Decoder fillDec(codec), alphaDec(codec);
  Packer packer;
  Nv12 fill, alpha, fillOut, alphaOut;
  std::vector<uint8_t> src, back;
  drawPattern(src, f.width, f.height, 0, true);
  int pictures = 0;
  size_t fillBytes = 0, alphaBytes = 0;
  for (int i = 0; i < 30; ++i) {
    packer.pack(f, src, false, fill, &alpha);
    REQUIRE(fill.height == 720);
    const std::vector<Packet> fp = fillEnc->encode(fill, i == 0);
    const std::vector<Packet> ap = alphaEnc->encode(alpha, i == 0);
    REQUIRE(fp.size() == ap.size());
    for (size_t k = 0; k < fp.size(); ++k) {
      if (i == 0) CHECK(fp[k].keyframe == ap[k].keyframe);
      fillBytes += fp[k].data.size();
      alphaBytes += ap[k].data.size();
      int64_t ts = 0, ats = 0;
      const Decoded d = fillDec.decode(fp[k].data, i, fillOut, ts);
      const Decoded da = alphaDec.decode(ap[k].data, i, alphaOut, ats);
      CHECK(d == da);
      if (d != Decoded::Picture || da != Decoded::Picture) continue;
      CHECK(ts == ats);
      packer.unpack(f, fillOut, back, &alphaOut);
      ++pictures;
    }
  }
  MESSAGE("fill " << fillBytes / 30 << " bytes a frame, alpha " << alphaBytes / 30);
  REQUIRE(pictures >= 20);
  REQUIRE(back.size() == src.size());
  double alphaError = 0, colourError = 0;
  const size_t pixels = static_cast<size_t>(f.width) * f.height;
  for (size_t i = 0; i < pixels; ++i) {
    const int a = src[i * 4 + 3];
    alphaError += std::abs(back[i * 4 + 3] - a);
    for (int k = 0; k < 3; ++k) colourError += std::abs(back[i * 4 + k] - (src[i * 4 + k] * a + 127) / 255);
  }
  alphaError /= pixels;
  colourError /= pixels * 3.0;
  MESSAGE("mean alpha error " << alphaError << ", mean colour error " << colourError << " (0 to 255)");
  CHECK(alphaError < 3.0);
  CHECK(colourError < 6.0);
}

TEST_CASE("Decoder: garbage is an error, not a crash, and a keyframe decodes afterwards") {
  Codec codec = Codec::Hevc;
  SUBCASE("hevc") { codec = Codec::Hevc; }
  SUBCASE("h264") { codec = Codec::H264; }
  const VideoFormat f{1280, 720, 60, 1, false};
  auto enc = openEncoder(codec, f);
  if (!enc) return;
  const std::vector<Packet> all = encodePattern(*enc, f, 12, -1);
  Decoder dec(codec);
  Nv12 out;
  int64_t ts = 0;
  const std::vector<uint8_t> junk(500, 0x5A);
  CHECK(dec.decode(junk, 0, out, ts) != Decoded::Picture);
  CHECK(dec.decode({}, 0, out, ts) == Decoded::Error);
  bool picture = false;
  for (size_t i = 0; i < all.size(); ++i) {
    if (dec.decode(all[i].data, static_cast<int64_t>(i), out, ts) == Decoded::Picture) picture = true;
  }
  CHECK(picture);
  CHECK(out.width == 1280);
  CHECK(out.height == 720);
}

TEST_CASE("Decoder: a stream of the other codec never becomes a picture") {
  Codec stream = Codec::Hevc;
  SUBCASE("hevc stream, h264 decoder") { stream = Codec::Hevc; }
  SUBCASE("h264 stream, hevc decoder") { stream = Codec::H264; }
  const VideoFormat f{1280, 720, 60, 1, false};
  auto enc = openEncoder(stream, f);
  if (!enc) return;
  const std::vector<Packet> all = encodePattern(*enc, f, 12, -1);
  Decoder dec(stream == Codec::Hevc ? Codec::H264 : Codec::Hevc);
  Nv12 out;
  int64_t ts = 0;
  for (size_t i = 0; i < all.size(); ++i) {
    CHECK(dec.decode(all[i].data, static_cast<int64_t>(i), out, ts) != Decoded::Picture);
  }
}