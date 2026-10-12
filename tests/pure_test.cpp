#include <doctest/doctest.h>

#include <vector>

#include "codec.h"

using namespace omthx;

TEST_CASE("targetBitrate follows bits per pixel, quality and alpha") {
  VideoFormat f{1920, 1080, 60, 1, false};
  CHECK(targetBitrate(f, Quality::Low) == 14'929'920);
  CHECK(targetBitrate(f, Quality::Medium) == 29'859'840);
  CHECK(targetBitrate(f, Quality::High) == 49'766'400);
  f.alpha = true;
  CHECK(targetBitrate(f, Quality::Medium) == 37'324'800);
  CHECK(targetBitrate(VideoFormat{1280, 720, 30000, 1001, false}, Quality::Medium) == 6'628'891);
}

TEST_CASE("isKeyframe and hasNalType read NAL unit types from Annex B data") {
  const std::vector<uint8_t> idr{0, 0, 0, 1, 0x40, 1, 0xAA, 0, 0, 1, 0x26, 1, 0xBB};  // VPS (32), then IDR (19)
  const std::vector<uint8_t> trail{0, 0, 0, 1, 0x02, 1, 0xCC};                         // TRAIL_R (1)
  const std::vector<uint8_t> cra{0, 0, 1, 0x2A, 1};                                    // CRA (21)
  const std::vector<uint8_t> cut{0, 0, 1};                                             // start code, nothing after
  CHECK(isKeyframe(idr));
  CHECK(isKeyframe(cra));
  CHECK_FALSE(isKeyframe(trail));
  CHECK_FALSE(isKeyframe(cut));
  CHECK_FALSE(isKeyframe(std::vector<uint8_t>{}));
  CHECK(hasNalType(idr, 32));
  CHECK(hasNalType(idr, 19));
  CHECK_FALSE(hasNalType(trail, 32));
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

TEST_CASE("KeyframePolicy: first frame, requests limited to one per 500 ms, new drops, every 2 s") {
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
  CHECK(p.next(3000, 1, false));       // 2 seconds since the last keyframe
}
