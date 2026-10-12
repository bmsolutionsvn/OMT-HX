#include <doctest/doctest.h>

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

#include "codec.h"
#include "pattern.h"

using namespace omthx;

namespace {
// Null, with a visible message, on a PC that has no hardware HEVC encoder.
std::unique_ptr<Encoder> openEncoder(const VideoFormat& f) {
  try {
    return std::make_unique<Encoder>(f.width, codedHeight(f), f.fpsN, f.fpsD, targetBitrate(f, Quality::Medium));
  } catch (const std::runtime_error& e) {
    MESSAGE("SKIPPED, no hardware HEVC encoder on this PC: " << e.what());
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
}  // namespace

TEST_CASE("Encoder: first packet is a self-contained keyframe, then no keyframes until one is forced") {
  const VideoFormat f{1280, 720, 60, 1, false};
  auto enc = openEncoder(f);
  if (!enc) return;
  MESSAGE("encoder: " << enc->name());
  const std::vector<Packet> all = encodePattern(*enc, f, 30, 20);
  REQUIRE(all.size() >= 29);  // low delay: the encoder holds back one picture at most
  CHECK(all[0].keyframe);
  CHECK(hasNalType(all[0].data, 32));  // VPS
  CHECK(hasNalType(all[0].data, 33));  // SPS
  CHECK(hasNalType(all[0].data, 34));  // PPS
  for (size_t i = 1; i < 20; ++i) CHECK_FALSE(all[i].keyframe);
  CHECK(all[20].keyframe);
  CHECK(hasNalType(all[20].data, 32));  // a late joiner can start here
}

TEST_CASE("a picture with alpha survives pack, encode, decode and unpack") {
  const VideoFormat f{1280, 720, 60, 1, true};
  auto enc = openEncoder(f);
  if (!enc) return;
  Decoder dec;
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
  const VideoFormat f{1280, 720, 60, 1, false};
  auto enc = openEncoder(f);
  if (!enc) return;
  const std::vector<Packet> all = encodePattern(*enc, f, 12, -1);
  Decoder dec;
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
