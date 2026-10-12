#pragma once
// A moving test picture with grain, for tests and tools. Straight (not premultiplied) alpha.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

inline void drawPattern(std::vector<uint8_t>& bgra, int w, int h, int frame, bool alpha) {
  bgra.resize(static_cast<size_t>(w) * h * 4);
  uint32_t rng = 0x9E3779B9u * static_cast<uint32_t>(frame + 1);
  const int cx = w / 2 + static_cast<int>(w / 4 * std::sin(frame * 0.05));
  const int cy = h / 2;
  const int radius = h / 3;
  const int box = (frame * 8) % std::max(1, w - 64);
  auto clamp8 = [](int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); };
  for (int y = 0; y < h; ++y) {
    uint8_t* row = bgra.data() + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      const int grain = static_cast<int>(rng & 15) - 8;
      int b = ((x + frame * 2) * 255 / w) % 256;
      int g = y * 255 / h;
      int r = 128;
      if (x >= box && x < box + 64 && y >= h / 4 && y < h / 4 + 64) b = g = r = 235;
      row[x * 4] = clamp8(b + grain);
      row[x * 4 + 1] = clamp8(g + grain);
      row[x * 4 + 2] = clamp8(r + grain);
      int a = 255;
      if (alpha) {
        const int dx = x - cx, dy = y - cy;
        a = (radius * radius - dx * dx - dy * dy) * 4 / radius + 128;  // soft edge, about 30 pixels wide
      }
      row[x * 4 + 3] = clamp8(a);
    }
  }
}
