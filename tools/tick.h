// The A/V tick: once a second omthx-send sends a white picture and a short beep stamped with the same timestamp,
// and omthx-recv --tick finds both and reports how far apart they arrive. Header-only and pure, so the tests
// pin the rules both tools share.
#pragma once

#include <cmath>
#include <cstdint>

namespace omthx::tick {

inline constexpr int kRate = 48000;           // samples per second, per channel
inline constexpr int kBeepSamples = 2400;     // 50 ms of 1 kHz at the start of each second
inline constexpr float kBeepLevel = 0.5f;     // -6 dBFS
inline constexpr float kOnsetLevel = 0.25f;   // a sample at least this loud starts a beep

// True for the first frame of each second: frame i starts at i * fpsD / fpsN seconds.
inline bool flashFrame(int64_t frame, int fpsN, int fpsD) {
  return fpsN > 0 && fpsD > 0 && frame >= 0 && (frame * fpsD) % fpsN < fpsD;
}

// Sample k of the tick sound (k counts from the first sample sent): a beep in the first 50 ms of each second.
inline float beepSample(int64_t k) {
  if (k < 0 || k % kRate >= kBeepSamples) return 0.0f;
  return kBeepLevel * static_cast<float>(std::sin(2 * 3.141592653589793 * 1000 * static_cast<double>(k) / kRate));
}

// The first sample at least kOnsetLevel loud, or -1 when the block is quiet.
inline int onset(const float* samples, int count) {
  for (int i = 0; i < count; ++i)
    if (std::fabs(samples[i]) >= kOnsetLevel) return i;
  return -1;
}

// True when a picture is the white flash: the mean of 64 pixels spread over it is near white. `step` is the
// bytes between pixels in a row (4 for BGRA, 1 for an NV12 Y plane), `stride` the bytes between rows; the
// value read is the byte at each pixel (BGRA: blue, which is 255 only in white or blue-heavy pictures).
inline bool isFlash(const uint8_t* picture, int width, int height, int step, int stride) {
  if (!picture || width <= 0 || height <= 0) return false;
  int sum = 0;
  for (int gy = 0; gy < 8; ++gy) {
    for (int gx = 0; gx < 8; ++gx) {
      const int x = (2 * gx + 1) * width / 16, y = (2 * gy + 1) * height / 16;
      sum += picture[static_cast<size_t>(y) * stride + static_cast<size_t>(x) * step];
    }
  }
  return sum / 64 >= 230;
}

}  // namespace omthx::tick
