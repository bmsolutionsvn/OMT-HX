#include <doctest/doctest.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "codec.h"

using namespace omthx;

TEST_CASE("targetBitrate follows bits per pixel, quality, alpha and codec") {
  VideoFormat f{1920, 1080, 60, 1, false};
  CHECK(targetBitrate(f, Quality::Low, Codec::Hevc) == 14'929'920);
  CHECK(targetBitrate(f, Quality::Medium, Codec::Hevc) == 29'859'840);
  CHECK(targetBitrate(f, Quality::High, Codec::Hevc) == 49'766'400);
  CHECK(targetBitrate(f, Quality::Low, Codec::H264) == 20'901'888);  // 1.4 times HEVC
  CHECK(targetBitrate(f, Quality::Medium, Codec::H264) == 41'803'776);
  CHECK(targetBitrate(f, Quality::High, Codec::H264) == 69'672'960);
  f.alpha = true;
  CHECK(targetBitrate(f, Quality::Medium, Codec::Hevc) == 37'324'800);
  CHECK(targetBitrate(f, Quality::Medium, Codec::H264) == 52'254'720);
  CHECK(targetBitrate(VideoFormat{1280, 720, 30000, 1001, false}, Quality::Medium, Codec::Hevc) == 6'628'891);
}

TEST_CASE("a separate alpha stream costs a quarter of the fill, which gets the opaque bitrate") {
  const VideoFormat opaque{1920, 1080, 60, 1, false};
  const VideoFormat keyed{1920, 1080, 60, 1, true};
  for (Codec codec : {Codec::Hevc, Codec::H264}) {
    CHECK(fillBitrate(keyed, Quality::Medium, codec) == targetBitrate(opaque, Quality::Medium, codec));
    CHECK(alphaBitrate(keyed, Quality::Medium, codec) == targetBitrate(opaque, Quality::Medium, codec) / 4);
  }
  CHECK(fillBitrate(keyed, Quality::Medium, Codec::Hevc) + alphaBitrate(keyed, Quality::Medium, Codec::Hevc) ==
        targetBitrate(keyed, Quality::Medium, Codec::Hevc));  // the same total as the stacked picture
}

TEST_CASE("codec tags are the FourCCs HEVC and H264 and nothing else is a codec") {
  CHECK(codecTag(Codec::Hevc) == 0x43564548);
  CHECK(codecTag(Codec::H264) == 0x34363248);
  Codec c = Codec::Hevc;
  CHECK(codecFromTag(0x34363248, c));
  CHECK(c == Codec::H264);
  CHECK(codecFromTag(0x43564548, c));
  CHECK(c == Codec::Hevc);
  c = Codec::H264;
  CHECK_FALSE(codecFromTag(0x31584D56, c));  // VMX1
  CHECK_FALSE(codecFromTag(0, c));
  CHECK(c == Codec::H264);  // untouched by a tag that is not a codec
  CHECK(std::string(codecName(Codec::Hevc)) == "hevc");
  CHECK(std::string(codecName(Codec::H264)) == "h264");
}

TEST_CASE("isKeyframe and hasNalType read HEVC NAL unit types from Annex B data") {
  const std::vector<uint8_t> idr{0, 0, 0, 1, 0x40, 1, 0xAA, 0, 0, 1, 0x26, 1, 0xBB};  // VPS (32), then IDR (19)
  const std::vector<uint8_t> trail{0, 0, 0, 1, 0x02, 1, 0xCC};                         // TRAIL_R (1)
  const std::vector<uint8_t> cra{0, 0, 1, 0x2A, 1};                                    // CRA (21)
  const std::vector<uint8_t> cut{0, 0, 1};                                             // start code, nothing after
  CHECK(isKeyframe(Codec::Hevc, idr));
  CHECK(isKeyframe(Codec::Hevc, cra));
  CHECK_FALSE(isKeyframe(Codec::Hevc, trail));
  CHECK_FALSE(isKeyframe(Codec::Hevc, cut));
  CHECK_FALSE(isKeyframe(Codec::Hevc, std::vector<uint8_t>{}));
  CHECK(hasNalType(Codec::Hevc, idr, 32));
  CHECK(hasNalType(Codec::Hevc, idr, 19));
  CHECK_FALSE(hasNalType(Codec::Hevc, trail, 32));
}

TEST_CASE("isKeyframe and hasNalType read H.264 NAL unit types from Annex B data") {
  // What an AMD keyframe looks like: AUD (9), SPS (7), PPS (8), then the IDR (5) last.
  const std::vector<uint8_t> idr{0, 0, 0, 1, 0x09, 0x10, 0, 0, 0, 1, 0x67, 0xAA,
                                 0, 0, 1,    0x68, 0xBB, 0, 0, 1, 0x65, 0xCC};
  const std::vector<uint8_t> slice{0, 0, 0, 1, 0x09, 0x30, 0, 0, 1, 0x41, 0xDD};  // AUD, then a non-IDR slice (1)
  const std::vector<uint8_t> cut{0, 0, 1};
  CHECK(isKeyframe(Codec::H264, idr));
  CHECK_FALSE(isKeyframe(Codec::H264, slice));
  CHECK_FALSE(isKeyframe(Codec::H264, cut));
  CHECK_FALSE(isKeyframe(Codec::H264, std::vector<uint8_t>{}));
  CHECK(hasNalType(Codec::H264, idr, 7));
  CHECK(hasNalType(Codec::H264, idr, 8));
  CHECK(hasNalType(Codec::H264, idr, 5));
  CHECK_FALSE(hasNalType(Codec::H264, slice, 7));
}

TEST_CASE("a keyframe of one codec is not a keyframe when read as the other") {
  const std::vector<uint8_t> hevcIdr{0, 0, 0, 1, 0x26, 1, 0xBB};  // HEVC IDR (19); read as H.264 the byte is type 6
  const std::vector<uint8_t> h264Idr{0, 0, 0, 1, 0x65, 0xCC};     // H.264 IDR (5); read as HEVC the byte is type 50
  CHECK_FALSE(isKeyframe(Codec::H264, hevcIdr));
  CHECK_FALSE(isKeyframe(Codec::Hevc, h264Idr));
}

TEST_CASE("alphaTop rounds up to 16 and codedHeight doubles it only with alpha") {
  CHECK(alphaTop(1080) == 1088);
  CHECK(alphaTop(720) == 720);
  CHECK(alphaTop(1) == 16);
  CHECK(codedHeight(VideoFormat{1920, 1080, 60, 1, true}) == 2176);
  CHECK(codedHeight(VideoFormat{1920, 1080, 60, 1, false}) == 1080);
}

TEST_CASE("isGap flags a timestamp step of more than one and a half frames, or backwards") {
  CHECK_FALSE(isGap(0, 166'667, 60, 1));
  CHECK_FALSE(isGap(1'000'000, 1'166'666, 60, 1));
  CHECK(isGap(0, 333'334, 60, 1));   // one frame missing
  CHECK(isGap(500, 500, 60, 1));     // no progress
  CHECK(isGap(500, 100, 60, 1));     // the source restarted
  CHECK_FALSE(isGap(0, 333'667, 30000, 1001));
  CHECK_FALSE(isGap(0, 5, 0, 1));    // unknown frame rate: never a gap
}

TEST_CASE("frame numbers round-trip through the per-frame metadata") {
  uint64_t n = 0;
  CHECK(parseSeq(seqMetadata(0), n));
  CHECK(n == 0);
  CHECK(parseSeq(seqMetadata(18'446'744'073'709'551'615ULL), n));
  CHECK(n == 18'446'744'073'709'551'615ULL);
  CHECK(parseSeq("<other /><OMTHX Seq=\"42\" />", n));
  CHECK(n == 42);
  CHECK_FALSE(parseSeq("", n));
  CHECK_FALSE(parseSeq("<OMTHX Keyframe=\"true\" />", n));
  CHECK_FALSE(parseSeq("<OMTHX Seq=\"\" />", n));
  CHECK_FALSE(parseSeq("<OMTHX Seq=\"12", n));  // cut short
}

TEST_CASE("the length of a separate alpha stream rides in the same metadata") {
  CHECK(seqMetadata(7) == "<OMTHX Seq=\"7\" />");  // what sources before the alpha stream send
  CHECK(seqMetadata(7, 0) == seqMetadata(7));
  CHECK(seqMetadata(7, 1234) == "<OMTHX Seq=\"7\" Alpha=\"1234\" />");
  uint64_t n = 0;
  CHECK(parseSeq(seqMetadata(7, 1234), n));
  CHECK(n == 7);
  CHECK(parseAlphaBytes(seqMetadata(7, 1234)) == 1234);
  CHECK(parseAlphaBytes(seqMetadata(7)) == 0);
  CHECK(parseAlphaBytes("") == 0);
  CHECK(parseAlphaBytes("<OMTHX Seq=\"7\" Alpha=\"\" />") == 0);
  CHECK(parseAlphaBytes("<OMTHX Seq=\"7\" Alpha=\"12") == 0);                  // cut short
  CHECK(parseAlphaBytes("<OMTHX Seq=\"7\" /><Other Alpha=\"5\" />") == 0);   // not the OMTHX element's
}

TEST_CASE("copyNv12 drops row padding and stretches limited range to full") {
  const VideoFormat f{4, 2, 60, 1, false};
  const std::vector<uint8_t> y{16, 235, 126, 0, 9, 9, 16, 235, 126, 255, 9, 9};  // stride 6
  const std::vector<uint8_t> uv{16, 240, 128, 128, 9, 9};                          // stride 6
  Nv12 out;
  copyNv12(f, y.data(), 6, uv.data(), 6, false, out);
  CHECK(out.width == 4);
  CHECK(out.height == 2);
  CHECK(out.y == std::vector<uint8_t>{0, 255, 128, 0, 0, 255, 128, 255});
  CHECK(out.uv == std::vector<uint8_t>{1, 255, 128, 128});  // chroma 16 rounds to 1
  copyNv12(f, y.data(), 6, uv.data(), 6, true, out);
  CHECK(out.y == std::vector<uint8_t>{16, 235, 126, 0, 16, 235, 126, 255});
  CHECK_THROWS_AS(copyNv12(VideoFormat{3, 2, 60, 1, false}, y.data(), 6, uv.data(), 6, true, out),
                  std::invalid_argument);
}

TEST_CASE("KeyframePolicy: first frame, requests limited to one per 500 ms, new drops, every 10 s") {
  KeyframePolicy p;
  CHECK(p.next(0, 0, false));          // first frame
  CHECK_FALSE(p.next(16, 0, false));
  CHECK_FALSE(p.next(100, 0, true));   // asked, but 500 ms have not passed
  CHECK_FALSE(p.next(400, 0, false));
  CHECK(p.next(500, 0, false));        // the waiting request is served
  CHECK_FALSE(p.next(516, 0, false));
  CHECK_FALSE(p.next(600, 1, false));  // libomt dropped a frame
  CHECK(p.next(1000, 1, false));
  CHECK_FALSE(p.next(1016, 1, false)); // same drop count: nothing new
  CHECK_FALSE(p.next(3000, 1, false)); // 2 seconds: no longer a reason
  CHECK_FALSE(p.next(10999, 1, false));
  CHECK(p.next(11000, 1, false));      // 10 seconds since the last keyframe
}

#include <cstdlib>
#include <stdexcept>

namespace {
std::vector<uint8_t> flat(int w, int h, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
  std::vector<uint8_t> v(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < v.size(); i += 4) {
    v[i] = b;
    v[i + 1] = g;
    v[i + 2] = r;
    v[i + 3] = a;
  }
  return v;
}
bool near(int value, int want) { return std::abs(value - want) <= 4; }
}  // namespace

TEST_CASE("pack lays out fill, padding and alpha, and unpack reverses it") {
  const VideoFormat f{64, 40, 60, 1, true};  // alphaTop(40) is 48, so the coded height is 96
  std::vector<uint8_t> src = flat(64, 40, 200, 100, 50, 255);
  for (int y = 0; y < 40; ++y)
    for (int x = 0; x < 32; ++x) src[(static_cast<size_t>(y) * 64 + x) * 4 + 3] = 128;  // left half see-through

  Packer packer;
  Nv12 pic;
  packer.pack(f, src, false, pic);
  REQUIRE(pic.width == 64);
  REQUIRE(pic.height == 96);
  REQUIRE(pic.y.size() == 64u * 96);
  REQUIRE(pic.uv.size() == 64u * 48);

  // alpha picture: brightness is alpha, colour is neutral
  CHECK(pic.y[48 * 64 + 0] == 128);
  CHECK(pic.y[48 * 64 + 63] == 255);
  CHECK(pic.y[(48 + 39) * 64 + 0] == 128);
  CHECK(pic.uv[24 * 64 + 10] == 128);
  CHECK(pic.uv[47 * 64 + 63] == 128);
  // padding rows repeat the row above them, in both halves
  CHECK(pic.y[40 * 64 + 5] == pic.y[39 * 64 + 5]);
  CHECK(pic.y[47 * 64 + 60] == pic.y[39 * 64 + 60]);
  CHECK(pic.y[(48 + 40) * 64 + 0] == 128);
  CHECK(pic.y[95 * 64 + 63] == 255);

  std::vector<uint8_t> back;
  packer.unpack(f, pic, back);
  REQUIRE(back.size() == src.size());
  const size_t solid = (20 * 64 + 48) * 4;  // right half: colour as sent, alpha exact
  CHECK(near(back[solid], 200));
  CHECK(near(back[solid + 1], 100));
  CHECK(near(back[solid + 2], 50));
  CHECK(back[solid + 3] == 255);
  const size_t half = (20 * 64 + 8) * 4;  // left half: colour was multiplied by 128/255
  CHECK(near(back[half], 100));
  CHECK(near(back[half + 1], 50));
  CHECK(near(back[half + 2], 25));
  CHECK(back[half + 3] == 128);
}

TEST_CASE("pack without alpha keeps the display size and uses full range") {
  const VideoFormat f{32, 18, 60, 1, false};
  Packer packer;
  Nv12 pic;
  packer.pack(f, flat(32, 18, 255, 255, 255, 7), false, pic);  // the alpha byte is ignored
  CHECK(pic.height == 18);
  CHECK(pic.y[0] >= 254);  // limited range would give 235
  CHECK(near(pic.uv[0], 128));
  packer.pack(f, flat(32, 18, 0, 0, 0, 255), false, pic);
  CHECK(pic.y[0] <= 1);  // limited range would give 16
  std::vector<uint8_t> back;
  packer.unpack(f, pic, back);
  CHECK(back[3] == 255);
}

TEST_CASE("pack trusts a premultiplied source and rejects sizes it cannot pack") {
  const VideoFormat f{32, 18, 60, 1, true};
  Packer packer;
  Nv12 a, b;
  const std::vector<uint8_t> src = flat(32, 18, 60, 60, 60, 128);
  packer.pack(f, src, true, a);   // already premultiplied: colour untouched
  packer.pack(f, src, false, b);  // straight: colour scaled by alpha
  CHECK(a.y[0] > b.y[0]);
  CHECK_THROWS_AS(packer.pack(VideoFormat{31, 18, 60, 1, false}, src, false, a), std::invalid_argument);
  CHECK_THROWS_AS(packer.pack(f, std::vector<uint8_t>(10), false, a), std::invalid_argument);
  Nv12 small;
  small.width = 32;
  small.height = 18;  // too short for a picture with alpha
  std::vector<uint8_t> out;
  CHECK_THROWS_AS(packer.unpack(f, small, out), std::invalid_argument);
}

TEST_CASE("pack leaves opaque pixels as sent and turns fully transparent ones black") {
  const VideoFormat f{32, 18, 60, 1, true};
  Packer packer;
  Nv12 straight, premultiplied;
  const std::vector<uint8_t> opaque = flat(32, 18, 200, 100, 50, 255);
  packer.pack(f, opaque, false, straight);
  packer.pack(f, opaque, true, premultiplied);
  CHECK(straight.y == premultiplied.y);
  CHECK(straight.uv == premultiplied.uv);

  packer.pack(f, flat(32, 18, 200, 100, 50, 0), false, straight);
  CHECK(straight.y[0] <= 1);              // fill: black
  CHECK(near(straight.uv[0], 128));
  CHECK(straight.y[32 * 32] == 0);        // alphaTop(18) is 32: first row of the alpha picture

  // Premultiplying runs in bands of rows: every row of a flat half-transparent picture must come out the same.
  packer.pack(f, flat(32, 18, 200, 100, 50, 128), false, straight);
  packer.pack(f, flat(32, 18, 100, 50, 25, 128), true, premultiplied);
  for (int r = 0; r < 18; ++r) {
    CAPTURE(r);
    CHECK(straight.y[static_cast<size_t>(r) * 32] == straight.y[0]);
    CHECK(near(straight.y[static_cast<size_t>(r) * 32 + 31], premultiplied.y[0]));
  }
}

TEST_CASE("packUyvy re-ranges without a colour matrix and unpackUyvy reverses it") {
  const VideoFormat f{4, 2, 60, 1, false};
  // Rows 10 bytes apart: white then black. Chroma 64 over 192 on the left pair, neutral on the right.
  const std::vector<uint8_t> src{64,  235, 64,  235, 128, 16, 128, 16, 9, 9,
                                 192, 235, 192, 235, 128, 16, 128, 16, 9, 9};
  Nv12 pic;
  packUyvy(f, src.data(), 10, nullptr, 0, false, pic);
  CHECK(pic.height == 2);
  CHECK(pic.y == std::vector<uint8_t>{255, 255, 0, 0, 255, 255, 0, 0});
  CHECK(pic.uv == std::vector<uint8_t>{128, 128, 128, 128});  // 64 and 192 are 55 and 201 in full range: mean 128

  std::vector<uint8_t> back;
  unpackUyvy(f, pic, back);
  CHECK(back == std::vector<uint8_t>{128, 235, 128, 235, 128, 16, 128, 16, 128, 235, 128, 235, 128, 16, 128, 16});
  CHECK_THROWS_AS(packUyvy(VideoFormat{3, 2, 60, 1, false}, src.data(), 10, nullptr, 0, false, pic),
                  std::invalid_argument);
  CHECK_THROWS_AS(packUyvy(f, src.data(), 7, nullptr, 0, false, pic), std::invalid_argument);
}

TEST_CASE("packUyvy stacks the alpha plane and premultiplies straight colour in YUV") {
  const VideoFormat f{64, 40, 60, 1, true};  // alphaTop(40) is 48, so the coded height is 96
  const int stride = 140, alphaStride = 70;  // both padded
  std::vector<uint8_t> uyvy(static_cast<size_t>(stride) * 40), alpha(static_cast<size_t>(alphaStride) * 40);
  for (int r = 0; r < 40; ++r) {
    for (int x = 0; x < 64; ++x) {
      uint8_t* p = &uyvy[static_cast<size_t>(r) * stride + 2 * x];
      p[0] = x % 2 ? 200 : 60;  // U before an even pixel's luma, V before an odd one's
      p[1] = 235;
      alpha[static_cast<size_t>(r) * alphaStride + x] = x < 32 ? 128 : 255;  // left half see-through
    }
  }
  Nv12 straight, premultiplied;
  packUyvy(f, uyvy.data(), stride, alpha.data(), alphaStride, false, straight);
  packUyvy(f, uyvy.data(), stride, alpha.data(), alphaStride, true, premultiplied);
  REQUIRE(straight.height == 96);
  // alpha picture: brightness is alpha, colour is neutral, padding rows repeat the row above
  CHECK(straight.y[48 * 64 + 0] == 128);
  CHECK(straight.y[48 * 64 + 63] == 255);
  CHECK(straight.y[95 * 64 + 63] == 255);
  CHECK(straight.uv[24 * 64 + 10] == 128);
  CHECK(straight.y[47 * 64 + 5] == straight.y[39 * 64 + 5]);
  // opaque half as sent; see-through half: luma and the distance from neutral chroma scaled by 128/255
  CHECK(straight.y[20 * 64 + 40] == premultiplied.y[20 * 64 + 40]);
  CHECK(premultiplied.y[20 * 64 + 8] == 255);
  CHECK(straight.y[20 * 64 + 8] == 128);
  const int u = premultiplied.uv[10 * 64 + 8], v = premultiplied.uv[10 * 64 + 9];
  CHECK(near(straight.uv[10 * 64 + 8], 128 + (u - 128) * 128 / 255));
  CHECK(near(straight.uv[10 * 64 + 9], 128 + (v - 128) * 128 / 255));

  std::vector<uint8_t> back;
  unpackUyvy(f, straight, back);
  const size_t plane = 64u * 2 * 40;
  REQUIRE(back.size() == plane + 64u * 40);
  CHECK(back[plane + 20 * 64 + 8] == 128);  // the alpha plane follows the UYVY plane
  CHECK(back[plane + 20 * 64 + 40] == 255);
  CHECK(back[20 * 128 + 2 * 40 + 1] == 235);                       // opaque luma
  CHECK(near(back[20 * 128 + 2 * 8 + 1], 16 + 219 * 128 / 255));  // see-through luma stays premultiplied
  CHECK_THROWS_AS(packUyvy(f, uyvy.data(), stride, nullptr, 0, false, straight), std::invalid_argument);
}

TEST_CASE("pack with a separate alpha picture keeps the fill at display size") {
  const VideoFormat f{64, 40, 60, 1, true};
  std::vector<uint8_t> src = flat(64, 40, 200, 100, 50, 255);
  for (int y = 0; y < 40; ++y)
    for (int x = 0; x < 32; ++x) src[(static_cast<size_t>(y) * 64 + x) * 4 + 3] = 128;  // left half see-through

  Packer packer;
  Nv12 fill, alpha, stacked;
  packer.pack(f, src, false, fill, &alpha);
  packer.pack(f, src, false, stacked);
  REQUIRE(fill.height == 40);
  REQUIRE(fill.y.size() == 64u * 40);
  REQUIRE(fill.uv.size() == 64u * 20);
  REQUIRE(alpha.width == 64);
  REQUIRE(alpha.height == 40);
  REQUIRE(alpha.y.size() == 64u * 40);
  REQUIRE(alpha.uv.size() == 64u * 20);
  // the fill is the top of the stacked picture, and the alpha picture is its alpha half
  CHECK(std::equal(fill.y.begin(), fill.y.end(), stacked.y.begin()));
  CHECK(std::equal(fill.uv.begin(), fill.uv.end(), stacked.uv.begin()));
  CHECK(std::equal(alpha.y.begin(), alpha.y.end(), stacked.y.begin() + 48 * 64));
  CHECK(alpha.y[20 * 64 + 8] == 128);
  CHECK(alpha.y[20 * 64 + 40] == 255);
  CHECK(std::all_of(alpha.uv.begin(), alpha.uv.end(), [](uint8_t c) { return c == 128; }));

  std::vector<uint8_t> back, backStacked;
  packer.unpack(f, fill, back, &alpha);
  packer.unpack(f, stacked, backStacked);
  CHECK(back == backStacked);
  CHECK(back[(20 * 64 + 8) * 4 + 3] == 128);

  Nv12 small = alpha;
  small.height = 20;
  CHECK_THROWS_AS(packer.unpack(f, fill, back, &small), std::invalid_argument);
  CHECK_THROWS_AS(packer.unpack(f, fill, back), std::invalid_argument);  // too short to hold stacked alpha

  // Without alpha in the format, the alpha picture is left alone.
  Nv12 untouched;
  packer.pack(VideoFormat{64, 40, 60, 1, false}, src, false, fill, &untouched);
  CHECK(fill.height == 40);
  CHECK(untouched.y.empty());
}

TEST_CASE("packUyvy with a separate alpha picture, and unpackUyvy back to UYVA") {
  const VideoFormat f{64, 40, 60, 1, true};
  const int stride = 140, alphaStride = 70;  // both padded
  std::vector<uint8_t> uyvy(static_cast<size_t>(stride) * 40), alpha(static_cast<size_t>(alphaStride) * 40);
  for (int r = 0; r < 40; ++r) {
    for (int x = 0; x < 64; ++x) {
      uint8_t* p = &uyvy[static_cast<size_t>(r) * stride + 2 * x];
      p[0] = x % 2 ? 200 : 60;
      p[1] = 235;
      alpha[static_cast<size_t>(r) * alphaStride + x] = x < 32 ? 128 : 255;
    }
  }
  Nv12 fill, alphaPic, stacked;
  packUyvy(f, uyvy.data(), stride, alpha.data(), alphaStride, false, fill, &alphaPic);
  packUyvy(f, uyvy.data(), stride, alpha.data(), alphaStride, false, stacked);
  REQUIRE(fill.height == 40);
  REQUIRE(alphaPic.height == 40);
  CHECK(std::equal(fill.y.begin(), fill.y.end(), stacked.y.begin()));
  CHECK(std::equal(fill.uv.begin(), fill.uv.end(), stacked.uv.begin()));
  CHECK(alphaPic.y[20 * 64 + 8] == 128);
  CHECK(alphaPic.y[39 * 64 + 63] == 255);
  CHECK(alphaPic.uv[10 * 64 + 9] == 128);

  std::vector<uint8_t> back, backStacked;
  unpackUyvy(f, fill, back, &alphaPic);
  unpackUyvy(f, stacked, backStacked);
  CHECK(back == backStacked);
  REQUIRE(back.size() == 64u * 2 * 40 + 64u * 40);
}

TEST_CASE("mulDiv255 equals the rounded divide it replaces, for every value and alpha") {
  int wrong = 0;
  for (unsigned v = 0; v < 256; ++v)
    for (unsigned a = 0; a < 256; ++a) wrong += mulDiv255(v, a) != (v * a + 127) / 255;
  CHECK(wrong == 0);
}
