#include "codec.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

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

namespace {
SwsContext* makeConverter(int w, int h, AVPixelFormat from, AVPixelFormat to) {
  SwsContext* c = sws_getContext(w, h, from, w, h, to, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!c) throw std::runtime_error("cannot create the colour converter");
  const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
  sws_setColorspaceDetails(c, bt709, 1, bt709, 1, 0, 1 << 16, 1 << 16);  // full range on both sides
  return c;
}

void checkSize(const VideoFormat& f) {
  if (f.width < 2 || f.height < 2 || f.width % 2 || f.height % 2)
    throw std::invalid_argument("width and height must be even and at least 2");
}

uint8_t scale(unsigned colour, unsigned alpha) { return static_cast<uint8_t>((colour * alpha + 127) / 255); }
}  // namespace

Packer::~Packer() {
  sws_freeContext(toNv12_);
  sws_freeContext(toBgra_);
}

void Packer::ensure(int w, int h) {
  if (toNv12_ && w == width_ && h == height_) return;
  sws_freeContext(toNv12_);
  sws_freeContext(toBgra_);
  toNv12_ = toBgra_ = nullptr;
  toNv12_ = makeConverter(w, h, AV_PIX_FMT_BGRA, AV_PIX_FMT_NV12);
  toBgra_ = makeConverter(w, h, AV_PIX_FMT_NV12, AV_PIX_FMT_BGRA);
  width_ = w;
  height_ = h;
}

// ponytail: colour conversion and premultiplying run on the CPU, a few ms per 1080p frame.
// Upgrade path: hand the encoder a D3D11 texture and do both in a shader.
void Packer::pack(const VideoFormat& f, std::span<const uint8_t> bgra, bool premultiplied, Nv12& out) {
  checkSize(f);
  const int w = f.width, h = f.height;
  if (bgra.size() != static_cast<size_t>(w) * h * 4)
    throw std::invalid_argument("bgra must be width * height * 4 bytes");
  ensure(w, h);
  const int coded = codedHeight(f);
  out.width = w;
  out.height = coded;
  out.y.resize(static_cast<size_t>(w) * coded);
  out.uv.resize(static_cast<size_t>(w) * coded / 2);

  const uint8_t* src = bgra.data();
  if (f.alpha && !premultiplied) {
    scratch_.resize(bgra.size());
    for (size_t i = 0; i < bgra.size(); i += 4) {
      const unsigned a = bgra[i + 3];
      scratch_[i] = scale(bgra[i], a);
      scratch_[i + 1] = scale(bgra[i + 1], a);
      scratch_[i + 2] = scale(bgra[i + 2], a);
      scratch_[i + 3] = static_cast<uint8_t>(a);
    }
    src = scratch_.data();
  }

  const uint8_t* srcPlanes[4] = {src, nullptr, nullptr, nullptr};
  const int srcStride[4] = {w * 4, 0, 0, 0};
  uint8_t* dstPlanes[4] = {out.y.data(), out.uv.data(), nullptr, nullptr};
  const int dstStride[4] = {w, w, 0, 0};
  sws_scale(toNv12_, srcPlanes, srcStride, 0, h, dstPlanes, dstStride);
  if (!f.alpha) return;

  const size_t row = static_cast<size_t>(w);
  const int top = alphaTop(h);
  uint8_t* y = out.y.data();
  uint8_t* uv = out.uv.data();
  for (int r = h; r < top; ++r) std::memcpy(y + r * row, y + (h - 1) * row, row);                   // under the fill
  for (int r = h / 2; r < top / 2; ++r) std::memcpy(uv + r * row, uv + (h / 2 - 1) * row, row);
  for (int r = 0; r < h; ++r) {
    uint8_t* dst = y + (top + r) * row;
    const uint8_t* a = src + r * row * 4 + 3;
    for (int x = 0; x < w; ++x) dst[x] = a[static_cast<size_t>(x) * 4];
  }
  for (int r = h; r < top; ++r) std::memcpy(y + (top + r) * row, y + (top + h - 1) * row, row);     // under the alpha
  std::memset(uv + (top / 2) * row, 128, (top / 2) * row);                                           // neutral colour
}

void Packer::unpack(const VideoFormat& f, const Nv12& in, std::vector<uint8_t>& bgra) {
  checkSize(f);
  const int w = f.width, h = f.height;
  const size_t rows = static_cast<size_t>(codedHeight(f));
  if (in.width != w || in.height < codedHeight(f) || in.y.size() < rows * w || in.uv.size() < rows * w / 2)
    throw std::invalid_argument("the picture does not match the frame header");
  ensure(w, h);
  bgra.resize(static_cast<size_t>(w) * h * 4);
  const uint8_t* srcPlanes[4] = {in.y.data(), in.uv.data(), nullptr, nullptr};
  const int srcStride[4] = {w, w, 0, 0};
  uint8_t* dstPlanes[4] = {bgra.data(), nullptr, nullptr, nullptr};
  const int dstStride[4] = {w * 4, 0, 0, 0};
  sws_scale(toBgra_, srcPlanes, srcStride, 0, h, dstPlanes, dstStride);
  if (!f.alpha) return;  // the converter wrote alpha 255
  const uint8_t* a = in.y.data() + static_cast<size_t>(alphaTop(h)) * w;
  for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) bgra[i * 4 + 3] = a[i];
}

}  // namespace omthx
