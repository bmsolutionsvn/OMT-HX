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
#include <libavutil/frame.h>
#include <libavutil/opt.h>
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

int64_t fillBitrate(const VideoFormat& f, Quality q, Codec codec) {
  VideoFormat opaque = f;
  opaque.alpha = false;
  return targetBitrate(opaque, q, codec);
}

int64_t alphaBitrate(const VideoFormat& f, Quality q, Codec codec) { return fillBitrate(f, q, codec) / 4; }

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

std::string seqMetadata(uint64_t seq, size_t alphaBytes) {
  std::string m = "<OMTHX Seq=\"" + std::to_string(seq) + "\"";
  if (alphaBytes) m += " Alpha=\"" + std::to_string(alphaBytes) + "\"";
  return m + " />";
}

bool parseSeq(std::string_view m, uint64_t& out) {
  static constexpr std::string_view kKey = "<OMTHX Seq=\"";
  const size_t at = m.find(kKey);
  if (at == std::string_view::npos) return false;
  const char* first = m.data() + at + kKey.size();
  const auto [end, ec] = std::from_chars(first, m.data() + m.size(), out);
  return ec == std::errc() && end != first && end < m.data() + m.size() && *end == '"';
}

size_t parseAlphaBytes(std::string_view m) {
  static constexpr std::string_view kKey = " Alpha=\"";
  const size_t element = m.find("<OMTHX ");
  if (element == std::string_view::npos) return 0;
  const size_t at = m.find(kKey, element);
  if (at == std::string_view::npos || at > m.find("/>", element)) return 0;  // only inside the OMTHX element
  const char* first = m.data() + at + kKey.size();
  size_t out = 0;
  const auto [end, ec] = std::from_chars(first, m.data() + m.size(), out);
  return ec == std::errc() && end != first && end < m.data() + m.size() && *end == '"' ? out : 0;
}

bool KeyframePolicy::next(int64_t nowMs, uint64_t dropped, bool requested) {
  if (dropped > dropped_ || requested) pending_ = true;
  dropped_ = dropped;
  const int64_t since = nowMs - lastKeyMs_;
  const bool key = first_ || since >= kPeriodicMs || (pending_ && since >= 500);
  if (key) {
    first_ = false;
    pending_ = false;
    lastKeyMs_ = nowMs;
  }
  return key;
}

uint8_t mulDiv255(unsigned value, unsigned alpha) {
  const unsigned x = value * alpha + 128;  // round(x / 255) without a divide: exact for x up to 255 * 255
  return static_cast<uint8_t>((x + (x >> 8)) >> 8);
}

namespace {
constexpr int kThreads = 8;  // bands of rows for the CPU passes, and swscale's slice threads

// ponytail: swscale's own slice threads halve the 1080p BGRA <-> NV12 pass (measured 7.1 to 2.7 ms, 8 threads).
SwsContext* makeConverter(int w, int h, AVPixelFormat from, AVPixelFormat to) {
  SwsContext* c = sws_alloc_context();
  if (!c) throw std::runtime_error("cannot create the colour converter");
  av_opt_set_int(c, "srcw", w, 0);
  av_opt_set_int(c, "srch", h, 0);
  av_opt_set_int(c, "src_format", from, 0);
  av_opt_set_int(c, "dstw", w, 0);
  av_opt_set_int(c, "dsth", h, 0);
  av_opt_set_int(c, "dst_format", to, 0);
  av_opt_set_int(c, "sws_flags", SWS_BILINEAR, 0);
  av_opt_set_int(c, "threads", kThreads, 0);
  av_opt_set_int(c, "src_range", 1, 0);  // full range on both sides
  av_opt_set_int(c, "dst_range", 1, 0);
  if (sws_init_context(c, nullptr, nullptr) < 0) {
    sws_freeContext(c);
    throw std::runtime_error("cannot create the colour converter");
  }
  const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
  sws_setColorspaceDetails(c, bt709, 1, bt709, 1, 0, 1 << 16, 1 << 16);
  return c;
}

void borrowed(void*, uint8_t*) {}  // the buffer belongs to the caller

// sws_scale_frame, not sws_scale: only the frame call uses the slice threads. The frames borrow the buffers
// through references that free nothing; a destination without references would get buffers of its own.
void convert(SwsContext* c, int w, int h, AVPixelFormat from, uint8_t* const src[2], const int srcStride[2],
             AVPixelFormat to, uint8_t* const dst[2], const int dstStride[2]) {
  AVFrame* in = av_frame_alloc();
  AVFrame* out = av_frame_alloc();
  auto borrow = [&](AVFrame* f, AVPixelFormat format, uint8_t* const data[2], const int stride[2]) {
    f->format = format;
    f->width = w;
    f->height = h;
    f->color_range = AVCOL_RANGE_JPEG;
    f->colorspace = AVCOL_SPC_BT709;
    for (int i = 0; i < 2 && data[i]; ++i) {
      f->data[i] = data[i];
      f->linesize[i] = stride[i];
      f->buf[i] = av_buffer_create(data[i], static_cast<size_t>(stride[i]) * h, borrowed, nullptr, 0);
    }
  };
  borrow(in, from, src, srcStride);
  borrow(out, to, dst, dstStride);
  const bool wrapped = in->buf[0] && out->buf[0] && (!dst[1] || out->buf[1]) && (!src[1] || in->buf[1]);
  const int rc = wrapped ? sws_scale_frame(c, out, in) : AVERROR(ENOMEM);
  av_frame_free(&in);
  av_frame_free(&out);
  if (rc < 0) throw std::runtime_error("the colour conversion failed");
}

// Runs fn(first, end) over `rows` split into kThreads bands on the thread pool.
template <typename F>
void inBands(int rows, F fn) {
  static constexpr int kBands[] = {0, 1, 2, 3, 4, 5, 6, 7};
  static_assert(std::size(kBands) == kThreads);
  std::for_each(std::execution::par, std::begin(kBands), std::end(kBands),
                [&](int b) { fn(rows * b / kThreads, rows * (b + 1) / kThreads); });
}

void checkSize(const VideoFormat& f) {
  if (f.width < 2 || f.height < 2 || f.width % 2 || f.height % 2)
    throw std::invalid_argument("width and height must be even and at least 2");
}

uint8_t scale(unsigned colour, unsigned alpha) { return mulDiv255(colour, alpha); }

// Chroma premultiplied in YUV: the distance from neutral is scaled, which equals scaling R, G and B.
uint8_t scaleChroma(int chroma, int alpha) {
  const int d = chroma - 128;
  return static_cast<uint8_t>(d >= 0 ? 128 + mulDiv255(d, alpha) : 128 - mulDiv255(-d, alpha));
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
  inBands(h, [&](int first, int last) {
    for (int r = first; r < last; ++r) {
      uint8_t* dst = y + (top + r) * row;
      const uint8_t* src = a + r * rowStride;
      if (step == 1) {
        std::memcpy(dst, src, row);
      } else {
        for (int x = 0; x < w; ++x) dst[x] = src[static_cast<size_t>(x) * step];
      }
    }
  });
  for (int r = h; r < top; ++r) std::memcpy(y + (top + r) * row, y + (top + h - 1) * row, row);     // under the alpha
  std::memset(uv + (top / 2) * row, 128, (top / 2) * row);                                           // neutral colour
}

// The separate alpha picture: `a` as brightness, at the display size, with neutral colour.
void separateAlpha(const VideoFormat& f, const uint8_t* a, size_t rowStride, size_t step, Nv12& out) {
  const int w = f.width, h = f.height;
  const size_t row = static_cast<size_t>(w);
  out.width = w;
  out.height = h;
  out.y.resize(row * h);
  out.uv.assign(row * h / 2, 128);
  uint8_t* y = out.y.data();
  inBands(h, [&](int first, int last) {
    for (int r = first; r < last; ++r) {
      uint8_t* dst = y + r * row;
      const uint8_t* src = a + r * rowStride;
      if (step == 1) {
        std::memcpy(dst, src, row);
      } else {
        for (int x = 0; x < w; ++x) dst[x] = src[static_cast<size_t>(x) * step];
      }
    }
  });
}

// stacked: alpha goes under the fill, so the picture is codedHeight rows; otherwise it is the display height.
void sizeFor(const VideoFormat& f, bool stacked, Nv12& out) {
  const int coded = stacked ? codedHeight(f) : f.height;
  out.width = f.width;
  out.height = coded;
  out.y.resize(static_cast<size_t>(f.width) * coded);
  out.uv.resize(static_cast<size_t>(f.width) * coded / 2);
}

void checkPicture(const VideoFormat& f, const Nv12& in, int rows) {
  const size_t n = static_cast<size_t>(rows);
  if (in.width != f.width || in.height < rows || in.y.size() < n * f.width || in.uv.size() < n * f.width / 2)
    throw std::invalid_argument("the picture does not match the frame header");
}

// The fill, and the alpha picture when it is separate. Returns where the alpha values start.
const uint8_t* checkPictures(const VideoFormat& f, const Nv12& in, const Nv12* alpha) {
  if (f.alpha && alpha) {
    checkPicture(f, in, f.height);
    checkPicture(f, *alpha, f.height);
    return alpha->y.data();
  }
  checkPicture(f, in, codedHeight(f));
  return f.alpha ? in.y.data() + static_cast<size_t>(alphaTop(f.height)) * f.width : nullptr;
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
void Packer::pack(const VideoFormat& f, std::span<const uint8_t> bgra, bool premultiplied, Nv12& out,
                  Nv12* alphaOut) {
  checkSize(f);
  const int w = f.width, h = f.height;
  if (bgra.size() != static_cast<size_t>(w) * h * 4)
    throw std::invalid_argument("bgra must be width * height * 4 bytes");
  ensure(w, h);
  sizeFor(f, !alphaOut, out);

  const uint8_t* src = bgra.data();
  if (f.alpha && !premultiplied) {
    scratch_.resize(bgra.size());
    const uint8_t* in = bgra.data();
    uint8_t* dst = scratch_.data();
    // Bands of rows on the thread pool: a fade makes every pixel need the multiply, which one core
    // cannot do for 1080p inside a 60 fps frame.
    const size_t rowBytes = static_cast<size_t>(w) * 4;
    inBands(h, [=](int first, int last) {
      const size_t end = rowBytes * last;
      for (size_t i = rowBytes * first; i < end; i += 4) {
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

  uint8_t* const srcPlanes[2] = {const_cast<uint8_t*>(src), nullptr};
  const int srcStride[2] = {w * 4, 0};
  uint8_t* const dstPlanes[2] = {out.y.data(), out.uv.data()};
  const int dstStride[2] = {w, w};
  convert(toNv12_, w, h, AV_PIX_FMT_BGRA, srcPlanes, srcStride, AV_PIX_FMT_NV12, dstPlanes, dstStride);
  if (f.alpha && alphaOut) {
    separateAlpha(f, src + 3, static_cast<size_t>(w) * 4, 4, *alphaOut);
  } else if (f.alpha) {
    stackAlpha(f, src + 3, static_cast<size_t>(w) * 4, 4, out);
  }
}

void Packer::unpack(const VideoFormat& f, const Nv12& in, std::vector<uint8_t>& bgra, const Nv12* alpha) {
  checkSize(f);
  const int w = f.width, h = f.height;
  const uint8_t* a = checkPictures(f, in, alpha);
  ensure(w, h);
  bgra.resize(static_cast<size_t>(w) * h * 4);
  uint8_t* const srcPlanes[2] = {const_cast<uint8_t*>(in.y.data()), const_cast<uint8_t*>(in.uv.data())};
  const int srcStride[2] = {w, w};
  uint8_t* const dstPlanes[2] = {bgra.data(), nullptr};
  const int dstStride[2] = {w * 4, 0};
  convert(toBgra_, w, h, AV_PIX_FMT_NV12, srcPlanes, srcStride, AV_PIX_FMT_BGRA, dstPlanes, dstStride);
  if (!f.alpha) return;  // the converter wrote alpha 255
  for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) bgra[i * 4 + 3] = a[i];
}

// No colour matrix: a table per value, and bands of row pairs on the thread pool (straight alpha measured 7.3 ms
// for 1080p on one core with a divide per sample).
void packUyvy(const VideoFormat& f, const uint8_t* uyvy, int stride, const uint8_t* alpha, int alphaStride,
              bool premultiplied, Nv12& out, Nv12* alphaOut) {
  checkSize(f);
  const int w = f.width, h = f.height;
  if (!uyvy || stride < w * 2 || (f.alpha && (!alpha || alphaStride < w)))
    throw std::invalid_argument("uyvy needs a stride of at least width * 2, and alpha a plane of at least width");
  sizeFor(f, !alphaOut, out);
  const bool multiply = f.alpha && !premultiplied;
  // Row pair r: luma rows 2r and 2r + 1, and chroma row r, the mean of the two input rows' chroma (4:2:2 to
  // 4:2:0), each premultiplied by the mean alpha of the two pixels it covers.
  inBands(h / 2, [&](int first, int last) {
    for (int r = first; r < last; ++r) {
      const uint8_t* s0 = uyvy + static_cast<size_t>(2 * r) * stride;
      const uint8_t* s1 = s0 + stride;
      const uint8_t* a0 = multiply ? alpha + static_cast<size_t>(2 * r) * alphaStride : nullptr;
      const uint8_t* a1 = multiply ? a0 + alphaStride : nullptr;
      uint8_t* y0 = out.y.data() + static_cast<size_t>(2 * r) * w;
      uint8_t* y1 = y0 + w;
      uint8_t* uv = out.uv.data() + static_cast<size_t>(r) * w;
      for (int i = 0; i < w; ++i) {  // i walks the pixels for luma, and U, V, U, V for chroma: both at byte 2 * i
        const uint8_t l0 = kLumaToFull[s0[2 * i + 1]], l1 = kLumaToFull[s1[2 * i + 1]];
        int c0 = kChromaToFull[s0[2 * i]], c1 = kChromaToFull[s1[2 * i]];
        if (multiply) {
          y0[i] = scale(l0, a0[i]);
          y1[i] = scale(l1, a1[i]);
          const int x = i & ~1;  // the first of the two pixels this chroma pair covers
          c0 = scaleChroma(c0, (a0[x] + a0[x + 1] + 1) / 2);
          c1 = scaleChroma(c1, (a1[x] + a1[x + 1] + 1) / 2);
        } else {
          y0[i] = l0;
          y1[i] = l1;
        }
        uv[i] = static_cast<uint8_t>((c0 + c1 + 1) / 2);
      }
    }
  });
  if (f.alpha && alphaOut) {
    separateAlpha(f, alpha, static_cast<size_t>(alphaStride), 1, *alphaOut);
  } else if (f.alpha) {
    stackAlpha(f, alpha, static_cast<size_t>(alphaStride), 1, out);
  }
}

void unpackUyvy(const VideoFormat& f, const Nv12& in, std::vector<uint8_t>& uyva, const Nv12* alpha) {
  checkSize(f);
  const uint8_t* a = checkPictures(f, in, alpha);
  const int w = f.width, h = f.height;
  const size_t plane = static_cast<size_t>(w) * 2 * h;
  uyva.resize(plane + (f.alpha ? static_cast<size_t>(w) * h : 0));
  inBands(h, [&](int first, int last) {
    for (int r = first; r < last; ++r) {
      const uint8_t* y = in.y.data() + static_cast<size_t>(r) * w;
      const uint8_t* uv = in.uv.data() + static_cast<size_t>(r / 2) * w;  // 4:2:0 to 4:2:2: chroma rows repeat
      uint8_t* d = uyva.data() + static_cast<size_t>(r) * w * 2;
      for (int i = 0; i < w; ++i) {
        d[2 * i] = kChromaToLimited[uv[i]];
        d[2 * i + 1] = kLumaToLimited[y[i]];
      }
    }
  });
  if (f.alpha) std::memcpy(uyva.data() + plane, a, static_cast<size_t>(w) * h);
}

}  // namespace omthx
