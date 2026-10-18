#include "codec.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <execution>
#include <iterator>
#include <stdexcept>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace omthx {

int64_t targetBitrate(const VideoFormat& f, Quality q, Codec codec) {
  static constexpr double kBitsPerPixel[] = {0.12, 0.24, 0.40};
  double bits = kBitsPerPixel[static_cast<int>(q)] * f.width * f.height * f.fpsN / f.fpsD;
  if (f.alpha) bits *= 1.25;
  if (codec == Codec::H264) bits *= 1.4;  // H.264 needs more bits than HEVC for the same picture
  return std::llround(bits);
}

int codecTag(Codec codec) { return codec == Codec::H264 ? kCodecH264 : kCodecHevc; }

bool codecFromTag(int tag, Codec& out) {
  if (tag == kCodecHevc) {
    out = Codec::Hevc;
    return true;
  }
  if (tag == kCodecH264) {
    out = Codec::H264;
    return true;
  }
  return false;
}

const char* codecName(Codec codec) { return codec == Codec::H264 ? "h264" : "hevc"; }

namespace {
// Calls fn(type) for each NAL unit and stops at the first true.
// The type sits in a different place in each codec's NAL header byte.
template <typename F>
bool anyNal(Codec codec, std::span<const uint8_t> d, F fn) {
  for (size_t i = 0; i + 3 < d.size(); ++i) {
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
      const int header = d[i + 3];
      if (fn(codec == Codec::H264 ? (header & 0x1F) : ((header >> 1) & 0x3F))) return true;
      i += 2;
    }
  }
  return false;
}
}  // namespace

bool hasNalType(Codec codec, std::span<const uint8_t> d, int type) {
  return anyNal(codec, d, [type](int t) { return t == type; });
}

// The pictures a decoder can start from: HEVC types 16 to 23, H.264 type 5 (IDR).
bool isKeyframe(Codec codec, std::span<const uint8_t> d) {
  return anyNal(codec, d, [codec](int t) { return codec == Codec::H264 ? t == 5 : (t >= 16 && t <= 23); });
}

int alphaTop(int height) { return (height + 15) / 16 * 16; }

int codedHeight(const VideoFormat& f) { return f.alpha ? 2 * alphaTop(f.height) : f.height; }

bool isGap(int64_t previous, int64_t current, int fpsN, int fpsD) {
  if (fpsN <= 0 || fpsD <= 0) return false;
  const int64_t frame = 10'000'000LL * fpsD / fpsN;
  const int64_t step = current - previous;
  return step <= 0 || step > frame + frame / 2;
}

std::string seqMetadata(uint64_t seq) { return "<OMTHX Seq=\"" + std::to_string(seq) + "\" />"; }

bool parseSeq(std::string_view m, uint64_t& out) {
  static constexpr std::string_view kKey = "<OMTHX Seq=\"";
  const size_t at = m.find(kKey);
  if (at == std::string_view::npos) return false;
  const char* first = m.data() + at + kKey.size();
  const auto [end, ec] = std::from_chars(first, m.data() + m.size(), out);
  return ec == std::errc() && end != first && end < m.data() + m.size() && *end == '"';
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

// Chroma premultiplied in YUV: the distance from neutral is scaled, which equals scaling R, G and B.
uint8_t scaleChroma(int chroma, int alpha) {
  const int d = (chroma - 128) * alpha;
  return static_cast<uint8_t>(128 + (d + (d >= 0 ? 127 : -127)) / 255);
}

using Table = std::array<uint8_t, 256>;
Table makeTable(double offsetIn, double gain, double offsetOut) {
  Table t{};
  for (int i = 0; i < 256; ++i) t[i] = static_cast<uint8_t>(std::clamp((i - offsetIn) * gain + offsetOut + 0.5, 0.0, 255.0));
  return t;
}
// Limited range (luma 16-235, chroma 16-240) to the full range the stream carries, and back.
const Table kLumaToFull = makeTable(16, 255 / 219.0, 0);
const Table kChromaToFull = makeTable(128, 255 / 224.0, 128);
const Table kLumaToLimited = makeTable(0, 219 / 255.0, 16);
const Table kChromaToLimited = makeTable(128, 224 / 255.0, 128);

// The rows under the fill of an alpha picture repeat its last row, the alpha picture below holds `a` as
// brightness with neutral colour, and the rows under it repeat its last row (spec section 7).
// `a` is the first alpha value; values are `step` bytes apart and rows `rowStride` bytes apart.
void stackAlpha(const VideoFormat& f, const uint8_t* a, size_t rowStride, size_t step, Nv12& out) {
  const int w = f.width, h = f.height;
  const size_t row = static_cast<size_t>(w);
  const int top = alphaTop(h);
  uint8_t* y = out.y.data();
  uint8_t* uv = out.uv.data();
  for (int r = h; r < top; ++r) std::memcpy(y + r * row, y + (h - 1) * row, row);                   // under the fill
  for (int r = h / 2; r < top / 2; ++r) std::memcpy(uv + r * row, uv + (h / 2 - 1) * row, row);
  for (int r = 0; r < h; ++r) {
    uint8_t* dst = y + (top + r) * row;
    const uint8_t* src = a + r * rowStride;
    for (int x = 0; x < w; ++x) dst[x] = src[static_cast<size_t>(x) * step];
  }
  for (int r = h; r < top; ++r) std::memcpy(y + (top + r) * row, y + (top + h - 1) * row, row);     // under the alpha
  std::memset(uv + (top / 2) * row, 128, (top / 2) * row);                                           // neutral colour
}

void sizeFor(const VideoFormat& f, Nv12& out) {
  const int coded = codedHeight(f);
  out.width = f.width;
  out.height = coded;
  out.y.resize(static_cast<size_t>(f.width) * coded);
  out.uv.resize(static_cast<size_t>(f.width) * coded / 2);
}

void checkPicture(const VideoFormat& f, const Nv12& in) {
  const size_t rows = static_cast<size_t>(codedHeight(f));
  if (in.width != f.width || in.height < codedHeight(f) || in.y.size() < rows * f.width ||
      in.uv.size() < rows * f.width / 2)
    throw std::invalid_argument("the picture does not match the frame header");
}
}  // namespace

// ponytail: one core, a table lookup per byte (about 1 ms for 1080p). Upgrade path: hand over full range.
void copyNv12(const VideoFormat& f, const uint8_t* y, int yStride, const uint8_t* uv, int uvStride, bool fullRange,
              Nv12& out) {
  checkSize(f);
  const int w = f.width, h = f.height;
  out.width = w;
  out.height = h;
  out.y.resize(static_cast<size_t>(w) * h);
  out.uv.resize(static_cast<size_t>(w) * h / 2);
  const Table& kLuma = kLumaToFull;
  const Table& kChroma = kChromaToFull;
  auto copy = [&](uint8_t* dst, const uint8_t* src, int stride, int rows, const Table& t) {
    for (int r = 0; r < rows; ++r) {
      const uint8_t* s = src + static_cast<size_t>(r) * stride;
      uint8_t* d = dst + static_cast<size_t>(r) * w;
      if (fullRange) {
        std::memcpy(d, s, w);
      } else {
        for (int x = 0; x < w; ++x) d[x] = t[s[x]];
      }
    }
  };
  copy(out.y.data(), y, yStride, h, kLuma);
  copy(out.uv.data(), uv, uvStride, h / 2, kChroma);
}

Packer::~Packer() {
  sws_freeContext(toNv12_);
  sws_freeContext(toBgra_);
}

void Packer::ensure(int w, int h) {
  if (toNv12_ && toBgra_ && w == width_ && h == height_) return;
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
  sizeFor(f, out);

  const uint8_t* src = bgra.data();
  if (f.alpha && !premultiplied) {
    scratch_.resize(bgra.size());
    const uint8_t* in = bgra.data();
    uint8_t* dst = scratch_.data();
    // Bands of rows on the thread pool: a fade makes every pixel need the multiply, which one core
    // cannot do for 1080p inside a 60 fps frame.
    static constexpr int kBands[] = {0, 1, 2, 3, 4, 5, 6, 7};
    const size_t rowBytes = static_cast<size_t>(w) * 4;
    std::for_each(std::execution::par, std::begin(kBands), std::end(kBands), [=](int band) {
      const size_t end = rowBytes * (static_cast<size_t>(h) * (band + 1) / std::size(kBands));
      for (size_t i = rowBytes * (static_cast<size_t>(h) * band / std::size(kBands)); i < end; i += 4) {
        const unsigned a = in[i + 3];
        if (a == 255) {  // most pixels of a key are opaque or fully transparent: no multiply
          std::memcpy(dst + i, in + i, 4);
        } else if (a == 0) {
          std::memset(dst + i, 0, 4);
        } else {
          dst[i] = scale(in[i], a);
          dst[i + 1] = scale(in[i + 1], a);
          dst[i + 2] = scale(in[i + 2], a);
          dst[i + 3] = static_cast<uint8_t>(a);
        }
      }
    });
    src = dst;
  }

  const uint8_t* srcPlanes[4] = {src, nullptr, nullptr, nullptr};
  const int srcStride[4] = {w * 4, 0, 0, 0};
  uint8_t* dstPlanes[4] = {out.y.data(), out.uv.data(), nullptr, nullptr};
  const int dstStride[4] = {w, w, 0, 0};
  sws_scale(toNv12_, srcPlanes, srcStride, 0, h, dstPlanes, dstStride);
  if (f.alpha) stackAlpha(f, src + 3, static_cast<size_t>(w) * 4, 4, out);
}

void Packer::unpack(const VideoFormat& f, const Nv12& in, std::vector<uint8_t>& bgra) {
  checkSize(f);
  const int w = f.width, h = f.height;
  checkPicture(f, in);
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

// ponytail: one core, plain loops, no colour matrix. Measure before banding it like pack's premultiply.
void packUyvy(const VideoFormat& f, const uint8_t* uyvy, int stride, const uint8_t* alpha, int alphaStride,
              bool premultiplied, Nv12& out) {
  checkSize(f);
  const int w = f.width, h = f.height;
  if (!uyvy || stride < w * 2 || (f.alpha && (!alpha || alphaStride < w)))
    throw std::invalid_argument("uyvy needs a stride of at least width * 2, and alpha a plane of at least width");
  sizeFor(f, out);
  const bool multiply = f.alpha && !premultiplied;
  for (int r = 0; r < h; ++r) {
    const uint8_t* s = uyvy + static_cast<size_t>(r) * stride;
    const uint8_t* a = multiply ? alpha + static_cast<size_t>(r) * alphaStride : nullptr;
    uint8_t* y = out.y.data() + static_cast<size_t>(r) * w;
    for (int x = 0; x < w; ++x) {
      const uint8_t v = kLumaToFull[s[2 * x + 1]];
      y[x] = a ? scale(v, a[x]) : v;
    }
  }
  // 4:2:2 to 4:2:0: each output chroma row is the mean of two input rows, each premultiplied by the mean
  // alpha of the two pixels it covers.
  for (int r = 0; r < h / 2; ++r) {
    const uint8_t* s0 = uyvy + static_cast<size_t>(2 * r) * stride;
    const uint8_t* s1 = s0 + stride;
    const uint8_t* a0 = multiply ? alpha + static_cast<size_t>(2 * r) * alphaStride : nullptr;
    const uint8_t* a1 = multiply ? a0 + alphaStride : nullptr;
    uint8_t* uv = out.uv.data() + static_cast<size_t>(r) * w;
    for (int i = 0; i < w; ++i) {  // i walks U, V, U, V; s holds U Y V Y, so chroma i sits at byte 2 * i
      int c0 = kChromaToFull[s0[2 * i]], c1 = kChromaToFull[s1[2 * i]];
      if (multiply) {
        const int x = i & ~1;  // the first of the two pixels this chroma pair covers
        c0 = scaleChroma(c0, (a0[x] + a0[x + 1] + 1) / 2);
        c1 = scaleChroma(c1, (a1[x] + a1[x + 1] + 1) / 2);
      }
      uv[i] = static_cast<uint8_t>((c0 + c1 + 1) / 2);
    }
  }
  if (f.alpha) stackAlpha(f, alpha, static_cast<size_t>(alphaStride), 1, out);
}

void unpackUyvy(const VideoFormat& f, const Nv12& in, std::vector<uint8_t>& uyva) {
  checkSize(f);
  checkPicture(f, in);
  const int w = f.width, h = f.height;
  const size_t plane = static_cast<size_t>(w) * 2 * h;
  uyva.resize(plane + (f.alpha ? static_cast<size_t>(w) * h : 0));
  for (int r = 0; r < h; ++r) {
    const uint8_t* y = in.y.data() + static_cast<size_t>(r) * w;
    const uint8_t* uv = in.uv.data() + static_cast<size_t>(r / 2) * w;  // 4:2:0 to 4:2:2: chroma rows repeat
    uint8_t* d = uyva.data() + static_cast<size_t>(r) * w * 2;
    for (int i = 0; i < w; ++i) {
      d[2 * i] = kChromaToLimited[uv[i]];
      d[2 * i + 1] = kLumaToLimited[y[i]];
    }
  }
  if (f.alpha) std::memcpy(uyva.data() + plane, in.y.data() + static_cast<size_t>(alphaTop(h)) * w, static_cast<size_t>(w) * h);
}

}  // namespace omthx
