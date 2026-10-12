#include "codec.h"

#include <cmath>

namespace omthx {

int64_t targetBitrate(const VideoFormat& f, Quality q) {
  static constexpr double kBitsPerPixel[] = {0.12, 0.24, 0.40};
  double bits = kBitsPerPixel[static_cast<int>(q)] * f.width * f.height * f.fpsN / f.fpsD;
  if (f.alpha) bits *= 1.25;
  return std::llround(bits);
}

namespace {
// Calls fn(type) for each NAL unit and stops at the first true.
template <typename F>
bool anyNal(std::span<const uint8_t> d, F fn) {
  for (size_t i = 0; i + 3 < d.size(); ++i) {
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
      if (fn((d[i + 3] >> 1) & 0x3F)) return true;
      i += 2;
    }
  }
  return false;
}
}  // namespace

bool hasNalType(std::span<const uint8_t> d, int type) {
  return anyNal(d, [type](int t) { return t == type; });
}

// HEVC types 16 to 23 are the pictures a decoder can start from.
bool isKeyframe(std::span<const uint8_t> d) {
  return anyNal(d, [](int t) { return t >= 16 && t <= 23; });
}

int alphaTop(int height) { return (height + 15) / 16 * 16; }

int codedHeight(const VideoFormat& f) { return f.alpha ? 2 * alphaTop(f.height) : f.height; }

bool isGap(int64_t previous, int64_t current, int fpsN, int fpsD) {
  if (fpsN <= 0 || fpsD <= 0) return false;
  const int64_t frame = 10'000'000LL * fpsD / fpsN;
  const int64_t step = current - previous;
  return step <= 0 || step > frame + frame / 2;
}

bool KeyframePolicy::next(int64_t nowMs, uint64_t dropped, bool requested) {
  if (dropped > dropped_ || requested) pending_ = true;
  dropped_ = dropped;
  const int64_t since = nowMs - lastKeyMs_;
  const bool key = first_ || since >= 2000 || (pending_ && since >= 500);
  if (key) {
    first_ = false;
    pending_ = false;
    lastKeyMs_ = nowMs;
  }
  return key;
}

}  // namespace omthx
