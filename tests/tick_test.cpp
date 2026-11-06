#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "pattern.h"
#include "tick.h"

using namespace omthx;

TEST_CASE("tick: the flash is the first frame of each second, at any rate") {
  int flashes = 0;
  for (int64_t i = 0; i < 600; ++i) flashes += tick::flashFrame(i, 60, 1);
  CHECK(flashes == 10);
  CHECK(tick::flashFrame(0, 60, 1));
  CHECK(tick::flashFrame(60, 60, 1));
  CHECK_FALSE(tick::flashFrame(59, 60, 1));
  // 29.97: frame 30 starts at 1.001 s, the first frame of second 1; frame 29 is still in second 0.
  CHECK(tick::flashFrame(30, 30000, 1001));
  CHECK_FALSE(tick::flashFrame(29, 30000, 1001));
  CHECK_FALSE(tick::flashFrame(5, 0, 1));
}

TEST_CASE("tick: the beep is 50 ms of 1 kHz at the start of each second, silence after") {
  CHECK(tick::beepSample(0) == 0.0f);  // sin(0)
  CHECK(tick::beepSample(12) == doctest::Approx(0.5f));  // a quarter of a 1 kHz cycle
  CHECK(tick::beepSample(48000 + 12) == doctest::Approx(0.5f));
  CHECK(tick::beepSample(2400) == 0.0f);
  CHECK(tick::beepSample(47999) == 0.0f);
  CHECK(tick::beepSample(-1) == 0.0f);
}

TEST_CASE("tick: onset finds the first loud sample") {
  std::vector<float> block(800, 0.0f);
  CHECK(tick::onset(block.data(), 800) == -1);
  for (int k = 0; k < 400; ++k) block[400 + static_cast<size_t>(k)] = tick::beepSample(k);
  const int at = tick::onset(block.data(), 800);
  CHECK(at >= 400);
  CHECK(at <= 404);  // within the first quarter cycle of the beep
}

TEST_CASE("tick: a white picture is the flash and the test pattern is not") {
  const int w = 320, h = 180;
  std::vector<uint8_t> white(static_cast<size_t>(w) * h * 4, 255);
  CHECK(tick::isFlash(white.data(), w, h, 4, w * 4));
  std::vector<uint8_t> y(static_cast<size_t>(w) * h, 250);  // an NV12 Y plane, slightly below white
  CHECK(tick::isFlash(y.data(), w, h, 1, w));
  for (int frame = 0; frame < 64; frame += 4) {
    std::vector<uint8_t> pattern;
    drawPattern(pattern, w, h, frame, false);
    CHECK_FALSE(tick::isFlash(pattern.data(), w, h, 4, w * 4));
  }
  CHECK_FALSE(tick::isFlash(nullptr, w, h, 4, w * 4));
}
