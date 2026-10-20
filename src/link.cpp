// The only file that calls libomt. Everything OMT-specific stays here (roadmap R12).
#include "codec.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string_view>

extern "C" {
#include <libavutil/frame.h>
}

#include <libomt.h>

namespace omthx {
namespace {
using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

void checkBgra(const VideoFormat& f, std::span<const uint8_t> bgra) {
  if (f.width <= 0 || f.height <= 0 || bgra.size() != static_cast<size_t>(f.width) * f.height * 4)
    throw std::invalid_argument("bgra must be width * height * 4 bytes");
}

OMTMediaFrame videoHeader(const VideoFormat& fmt, int64_t timestamp) {
  OMTMediaFrame f = {};
  f.Type = OMTFrameType_Video;
  f.Timestamp = timestamp;
  f.Width = fmt.width;
  f.Height = fmt.height;
  f.FrameRateN = fmt.fpsN;
  f.FrameRateD = fmt.fpsD;
  f.AspectRatio = static_cast<float>(fmt.width) / static_cast<float>(fmt.height);
  f.ColorSpace = OMTColorSpace_BT709;
  return f;
}

OMTVideoFlags alphaFlags(const VideoFormat& fmt, bool premultiplied) {
  int flags = 0;
  if (fmt.alpha) flags |= OMTVideoFlags_Alpha;
  if (fmt.alpha && premultiplied) flags |= OMTVideoFlags_PreMultiplied;
  return static_cast<OMTVideoFlags>(flags);
}

VideoFormat formatOf(const OMTMediaFrame* f) {
  return VideoFormat{f->Width, f->Height, f->FrameRateN, f->FrameRateD, (f->Flags & OMTVideoFlags_Alpha) != 0};
}

std::string_view metadataOf(const OMTMediaFrame* f) {
  if (!f->FrameMetadata || f->FrameMetadataLength <= 0) return {};
  return std::string_view(static_cast<const char*>(f->FrameMetadata), static_cast<size_t>(f->FrameMetadataLength));
}

// An HX frame's fill access unit and its VMX alpha, which ends the frame; `alpha` says which kind of frame it is.
// False for an alpha length that leaves no room for the fill.
bool splitFrame(const OMTMediaFrame* f, std::span<const uint8_t> data, std::span<const uint8_t>& fill,
                std::span<const uint8_t>& vmx, AlphaPart& alpha) {
  alpha = parseAlpha(metadataOf(f));
  if (alpha.bytes >= data.size()) return false;
  fill = data.first(data.size() - alpha.bytes);
  vmx = data.last(alpha.bytes);
  return true;
}
}  // namespace

// ---------------------------------------------------------------- Sender

struct Sender::Impl {
  // One picture's VMX alpha, waiting for the fill encoder's packet of the same picture.
  struct PendingAlpha {
    std::shared_ptr<const std::vector<uint8_t>> coded;  // this picture's VMX alpha, or the last one it repeats
    bool repeats = false;                               // the same plane as the picture before
  };

  Mode mode = Mode::Hx;
  Quality quality = Quality::Medium;
  Codec codec = Codec::Hevc;
  omt_send_t* tx = nullptr;
  VideoFormat format;          // what the encoder was opened for
  bool vmxAlpha = false;       // with alpha: the GPU codes the fill alone and VMX codes alpha
  const void* gpuKey = nullptr;  // and the D3D11 frames context it takes pictures from, or null
  bool gpuRefused = false;     // no GPU encoder took D3D11 pictures: they go through memory
  std::unique_ptr<Encoder> encoder;            // the fill, or the stacked picture
  std::unique_ptr<AlphaEncoder> alphaEncoder;  // VMX alpha, or null
  Packer packer;
  Nv12 picture;
  std::vector<uint8_t> alphaPlane;  // this picture's alpha, width * height
  std::vector<uint8_t> lastAlpha;   // the last alpha coded, to skip coding a plane that did not change
  std::shared_ptr<const std::vector<uint8_t>> lastCoded;  // and its VMX bytes, which keyframes repeat
  std::deque<PendingAlpha> alphas;  // one per picture the encoder still holds, in step with `stamps`
  std::vector<uint8_t> uyva;  // Full: a UYVA picture whose alpha plane did not follow its UYVY plane
  std::vector<uint8_t> gpuUyva;  // Full: a Gpu picture copied to memory
  KeyframePolicy policy;
  std::atomic<bool> appKeyframe{false};
  std::atomic<bool> forward{false};  // sendCoded: a receiver asked for a keyframe
  uint64_t codedDropped = 0;
  std::deque<int64_t> stamps;  // timestamps of pictures the encoder still holds
  uint64_t seq = 0;            // number of the next coded frame, carried in its metadata
  std::string seqMeta;
  std::vector<uint8_t> payload;  // the fill's packet with its VMX alpha after it
  Clock::time_point start = Clock::now();
  std::mutex statsMutex;
  SenderStats stats;

  // vmx: code alpha, when the format has it and VMX takes its size, with VMX rather than stacked under the fill.
  void openEncoder(const VideoFormat& fmt, bool vmx, AVBufferRef* gpuFrames = nullptr);
  bool askedForKeyframe();
  // Codes `picture`, or `gpu` when set, with `alphaPlane` when alpha goes by VMX, and sends what comes back.
  void sendPicture(const VideoFormat& fmt, int64_t timestamp, Clock::time_point t0, OMTMediaFrame& f,
                   const AVFrame* gpu = nullptr);
  void sendFull(OMTMediaFrame& f) {  // libomt encodes VMX itself
    omt_send(tx, &f);
    std::lock_guard lock(statsMutex);
    ++stats.framesSent;
  }
};

// Opens the encoders for this format and source of pictures, or keeps the ones already open for them.
void Sender::Impl::openEncoder(const VideoFormat& fmt, bool vmx, AVBufferRef* gpuFrames) {
  const bool sep = fmt.alpha && vmx && vmxAlphaFits(fmt);
  const void* key = gpuFrames ? gpuFrames->data : nullptr;
  if (encoder && fmt == format && sep == vmxAlpha && key == gpuKey) return;
  encoder.reset();  // the old session closes first: a GPU runs only a few at a time
  alphaEncoder.reset();
  stamps.clear();  // the new encoder owes nothing the old one held
  alphas.clear();
  lastAlpha.clear();
  lastCoded.reset();
  auto fill = std::make_unique<Encoder>(codec, fmt.width, sep ? fmt.height : codedHeight(fmt), fmt.fpsN, fmt.fpsD,
                                        sep ? fillBitrate(fmt, quality, codec) : targetBitrate(fmt, quality, codec),
                                        gpuFrames);
  // Alpha quality is not a setting: the plane is small, so it always codes at High.
  if (sep) alphaEncoder = std::make_unique<AlphaEncoder>(fmt.width, fmt.height, Quality::High);
  encoder = std::move(fill);
  format = fmt;
  vmxAlpha = sep;
  gpuKey = key;
  policy = KeyframePolicy();  // a new encoder starts with a keyframe
}

// True when a receiver of this source sent a keyframe request since the last call.
bool Sender::Impl::askedForKeyframe() {
  bool asked = false;
  while (OMTMediaFrame* m = omt_send_receive(tx, 0)) {  // messages from receivers
    if (m->Data && m->DataLength > 0 &&
        std::string_view(static_cast<const char*>(m->Data), static_cast<size_t>(m->DataLength)).find("<OMTHX Keyframe") !=
            std::string_view::npos)
      asked = true;
  }
  return asked;
}

void Sender::Impl::sendPicture(const VideoFormat& fmt, int64_t timestamp, Clock::time_point t0, OMTMediaFrame& f,
                               const AVFrame* gpu) {
  const bool requested = appKeyframe.exchange(false) | askedForKeyframe();
  OMTStatistics omt = {};
  omt_send_getvideostatistics(tx, &omt);
  const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  const bool key = policy.next(nowMs, static_cast<uint64_t>(omt.FramesDropped), requested);

  std::vector<Packet> packets;
  PendingAlpha alpha;
  if (alphaEncoder) {
    // A plane that did not change is not coded again: the frame says so and carries none.
    alpha.repeats = lastCoded && alphaPlane == lastAlpha;
    try {
      if (alpha.repeats) {
        alpha.coded = lastCoded;
        packets = gpu ? encoder->encodeGpu(gpu, key) : encoder->encode(picture, key);
      } else {
        // VMX codes alpha on the CPU while the GPU codes the fill; it takes about a millisecond for 1080p.
        auto coded = std::make_shared<std::vector<uint8_t>>();
        auto job = std::async(std::launch::async, [&] { alphaEncoder->encode(alphaPlane.data(), fmt.width, *coded); });
        try {
          packets = gpu ? encoder->encodeGpu(gpu, key) : encoder->encode(picture, key);
        } catch (...) {
          job.wait();
          throw;
        }
        job.get();
        alpha.coded = coded;
        lastCoded = std::move(coded);
        lastAlpha.swap(alphaPlane);  // alphaPlane is filled again for the next picture
      }
    } catch (...) {
      encoder.reset();  // the fill encoder may hold a picture whose alpha is lost: start both again
      alphaEncoder.reset();
      throw;
    }
  } else {
    packets = gpu ? encoder->encodeGpu(gpu, key) : encoder->encode(picture, key);
  }
  stamps.push_back(timestamp);  // after encode, so a throw leaves the deques unchanged
  if (alphaEncoder) alphas.push_back(std::move(alpha));
  const double encodeMs = msSince(t0);

  f.Codec = static_cast<OMTCodec>(codecTag(codec));
  f.Flags = static_cast<OMTVideoFlags>(fmt.alpha ? (OMTVideoFlags_Alpha | OMTVideoFlags_PreMultiplied) : 0);
  uint64_t sent = 0, keyframes = 0;
  for (Packet& p : packets) {
    const uint64_t n = seq++;  // numbered even when not sent, so the receiver sees the loss
    if (stamps.empty()) continue;
    f.Timestamp = stamps.front();
    stamps.pop_front();
    AlphaPart part;
    f.Data = p.data.data();
    f.DataLength = static_cast<int>(p.data.size());
    if (!alphas.empty()) {
      const PendingAlpha a = std::move(alphas.front());
      alphas.pop_front();
      // A keyframe always carries its alpha, so a receiver can start from it.
      if (a.repeats && !p.keyframe) {
        part.repeats = true;
      } else {
        part.bytes = a.coded->size();
        payload.assign(p.data.begin(), p.data.end());
        payload.insert(payload.end(), a.coded->begin(), a.coded->end());
        f.Data = payload.data();
        f.DataLength = static_cast<int>(payload.size());
      }
    }
    seqMeta = seqMetadata(n, part);
    f.FrameMetadata = seqMeta.data();
    f.FrameMetadataLength = static_cast<int>(seqMeta.size() + 1);  // with the terminating null
    omt_send(tx, &f);
    ++sent;
    keyframes += p.keyframe ? 1 : 0;
  }
  std::lock_guard lock(statsMutex);
  stats.encoder = encoder->name() + (gpu ? " (d3d11)" : "") + (alphaEncoder ? " + vmx alpha" : "");
  stats.framesSent += sent;
  stats.keyframes += keyframes;
  stats.lastEncodeMs = encodeMs;
}

Sender::Sender(const std::string& name, Mode mode, Quality quality, Codec codec) : d_(std::make_unique<Impl>()) {
  static constexpr OMTQuality kOmtQuality[] = {OMTQuality_Low, OMTQuality_Medium, OMTQuality_High};
  d_->mode = mode;
  d_->quality = quality;
  d_->codec = codec;
  d_->tx = omt_send_create(name.c_str(), kOmtQuality[static_cast<int>(quality)]);
  if (!d_->tx) throw std::runtime_error("cannot create the OMT source \"" + name + "\"");
  if (mode == Mode::Full) d_->stats.encoder = "vmx";
}

Sender::~Sender() { omt_send_destroy(d_->tx); }

void Sender::sendVideo(const VideoFormat& fmt, std::span<const uint8_t> bgra, bool premultiplied, int64_t timestamp) {
  checkBgra(fmt, bgra);
  OMTMediaFrame f = videoHeader(fmt, timestamp);

  if (d_->mode == Mode::Full) {
    f.Flags = alphaFlags(fmt, premultiplied);
    f.Codec = OMTCodec_BGRA;
    f.Stride = fmt.width * 4;
    f.Data = const_cast<uint8_t*>(bgra.data());
    f.DataLength = static_cast<int>(bgra.size());
    d_->sendFull(f);
    return;
  }

  d_->openEncoder(fmt, true);
  const auto t0 = Clock::now();
  d_->packer.pack(fmt, bgra, premultiplied, d_->picture, d_->vmxAlpha ? &d_->alphaPlane : nullptr);
  d_->sendPicture(fmt, timestamp, t0, f);
}

void Sender::sendVideoNv12(const VideoFormat& fmt, const uint8_t* y, int yStride, const uint8_t* uv, int uvStride,
                           bool fullRange, int64_t timestamp) {
  if (fmt.alpha) throw std::invalid_argument("sendVideoNv12 is for a picture without alpha");
  if (!y || !uv || yStride < fmt.width || uvStride < fmt.width || fmt.width < 2 || fmt.height < 2 || fmt.width % 2 ||
      fmt.height % 2)
    throw std::invalid_argument("NV12 needs an even size and a stride of at least the width for each plane");
  OMTMediaFrame f = videoHeader(fmt, timestamp);

  if (d_->mode == Mode::Full) {
    // VMX takes limited-range NV12 as it is and makes its 4:2:2 itself. libomt reads the chroma plane right after
    // the luma plane, so planes that are apart, or a full-range picture, are copied first.
    if (fullRange) throw std::invalid_argument("plain OMT takes limited-range NV12 only");
    const size_t plane = static_cast<size_t>(yStride) * fmt.height;
    f.Codec = OMTCodec_NV12;
    f.Stride = yStride;
    f.Data = const_cast<uint8_t*>(y);
    if (uv != y + plane || uvStride != yStride) {
      const size_t row = static_cast<size_t>(fmt.width);
      d_->uyva.resize(row * fmt.height * 3 / 2);
      for (int r = 0; r < fmt.height; ++r) std::memcpy(d_->uyva.data() + r * row, y + static_cast<size_t>(r) * yStride, row);
      for (int r = 0; r < fmt.height / 2; ++r)
        std::memcpy(d_->uyva.data() + row * fmt.height + r * row, uv + static_cast<size_t>(r) * uvStride, row);
      f.Stride = static_cast<int>(row);
      f.Data = d_->uyva.data();
    }
    f.DataLength = f.Stride * fmt.height * 3 / 2;
    d_->sendFull(f);
    return;
  }
  d_->openEncoder(fmt, true);
  const auto t0 = Clock::now();
  copyNv12(fmt, y, yStride, uv, uvStride, fullRange, d_->picture);
  d_->sendPicture(fmt, timestamp, t0, f);
}

void Sender::sendVideoUyvy(const VideoFormat& fmt, const uint8_t* uyvy, int stride, const uint8_t* alpha,
                           int alphaStride, bool premultiplied, int64_t timestamp) {
  if (fmt.width < 2 || fmt.height < 2 || fmt.width % 2 || !uyvy || stride < fmt.width * 2 ||
      (fmt.alpha && (!alpha || alphaStride < fmt.width)))
    throw std::invalid_argument("uyvy needs an even width and a stride of at least width * 2, and alpha a plane");
  OMTMediaFrame f = videoHeader(fmt, timestamp);

  if (d_->mode == Mode::Full) {
    const size_t plane = static_cast<size_t>(stride) * fmt.height;
    f.Flags = alphaFlags(fmt, premultiplied);
    f.Codec = fmt.alpha ? OMTCodec_UYVA : OMTCodec_UYVY;
    f.Stride = stride;
    f.Data = const_cast<uint8_t*>(uyvy);
    f.DataLength = static_cast<int>(plane);
    if (fmt.alpha) {
      // libvmx reads the alpha plane from Data + Stride * Height, rows Stride / 2 apart.
      if (alpha != uyvy + plane || stride % 2 || alphaStride != stride / 2) {
        const size_t row = static_cast<size_t>(fmt.width) * 2;
        d_->uyva.resize(row * fmt.height * 3 / 2);
        for (int r = 0; r < fmt.height; ++r) {
          std::memcpy(d_->uyva.data() + r * row, uyvy + static_cast<size_t>(r) * stride, row);
          std::memcpy(d_->uyva.data() + row * fmt.height + r * row / 2, alpha + static_cast<size_t>(r) * alphaStride,
                      row / 2);
        }
        f.Stride = static_cast<int>(row);
        f.Data = d_->uyva.data();
      }
      f.DataLength = f.Stride * fmt.height * 3 / 2;
    }
    d_->sendFull(f);
    return;
  }

  d_->openEncoder(fmt, true);
  const auto t0 = Clock::now();
  packUyvy(fmt, uyvy, stride, alpha, alphaStride, premultiplied, d_->picture, d_->vmxAlpha ? &d_->alphaPlane : nullptr);
  d_->sendPicture(fmt, timestamp, t0, f);
}

void Sender::sendVideo(const VideoFrame& frame) {
  const VideoFormat& fmt = frame.format;
  const size_t w = static_cast<size_t>(fmt.width), h = static_cast<size_t>(fmt.height);
  if (frame.gpu) {
    const AVFrame* pic = frame.gpu->frame;
    const std::vector<uint8_t>* alpha = fmt.alpha ? frame.gpu->alpha.get() : nullptr;
    const bool vmx = alpha != nullptr;  // otherwise alpha, if any, is stacked under the fill
    if (pic->width != fmt.width || pic->height != (vmx ? fmt.height : codedHeight(fmt)) ||
        (vmx && alpha->size() != static_cast<size_t>(fmt.width) * fmt.height))
      throw std::invalid_argument("the Gpu picture does not match its format");
    auto toMemory = [&] {
      if (!downloadNv12(pic, d_->picture)) throw std::runtime_error("cannot copy the picture from video memory");
    };
    if (d_->mode == Mode::Hx) {
      // Alpha is coded the way the picture holds it, by VMX or stacked, so nothing is re-packed.
      OMTMediaFrame f = videoHeader(fmt, frame.timestamp);
      const auto t0 = Clock::now();
      bool sent = false;
      if (frame.gpu->inVideoMemory() && !d_->gpuRefused) {
        try {
          d_->openEncoder(fmt, vmx, pic->hw_frames_ctx);
        } catch (const std::runtime_error&) {
          d_->gpuRefused = true;  // no GPU encoder takes D3D11 pictures: from now on, through memory
        }
        if (!d_->gpuRefused) {
          if (d_->vmxAlpha) d_->alphaPlane.assign(alpha->begin(), alpha->end());
          d_->sendPicture(fmt, frame.timestamp, t0, f, pic);
          sent = true;
        }
      }
      if (!sent) {  // through memory; the picture is already packed as the encoder wants it, premultiplied
        toMemory();
        d_->openEncoder(fmt, vmx);
        if (d_->vmxAlpha) d_->alphaPlane.assign(alpha->begin(), alpha->end());
        d_->sendPicture(fmt, frame.timestamp, t0, f);
      }
      return;
    }
    toMemory();
    unpackUyvy(fmt, d_->picture, d_->gpuUyva, vmx ? alpha->data() : nullptr);
    const uint8_t* p = d_->gpuUyva.data();
    sendVideoUyvy(fmt, p, fmt.width * 2, fmt.alpha ? p + w * 2 * h : nullptr, fmt.width, frame.premultiplied,
                  frame.timestamp);
    return;
  }
  if (!frame.uyvy.empty()) {
    if (frame.uyvy.size() < w * h * (fmt.alpha ? 3 : 2)) throw std::invalid_argument("uyvy is smaller than its format");
    const uint8_t* p = frame.uyvy.data();
    sendVideoUyvy(fmt, p, fmt.width * 2, fmt.alpha ? p + w * 2 * h : nullptr, fmt.width, frame.premultiplied,
                  frame.timestamp);
    return;
  }
  sendVideo(fmt, frame.bgra, frame.premultiplied, frame.timestamp);
}

void Sender::sendCoded(const CodedFrame& c) {
  if (c.data.empty() || c.format.width <= 0 || c.format.height <= 0)
    throw std::invalid_argument("a coded frame needs its data and its size");
  if (c.mode == Mode::Hx && (c.alphaBytes >= c.data.size() || (c.alphaBytes && c.alphaRepeats)))
    throw std::invalid_argument("alphaBytes must leave room for the fill, and a frame that repeats alpha has none");
  OMTMediaFrame f = videoHeader(c.format, c.timestamp);
  f.Flags = static_cast<OMTVideoFlags>(c.flags);
  f.ColorSpace = static_cast<OMTColorSpace>(c.colorSpace);
  f.Codec = c.mode == Mode::Hx ? static_cast<OMTCodec>(codecTag(c.codec)) : OMTCodec_VMX1;
  f.Data = const_cast<uint8_t*>(c.data.data());
  f.DataLength = static_cast<int>(c.data.size());
  if (c.mode == Mode::Hx) {  // the source's own number, so a frame lost before this sender shows downstream too
    d_->seqMeta = seqMetadata(c.seq, AlphaPart{c.alphaBytes, c.alphaRepeats});
    f.FrameMetadata = d_->seqMeta.data();
    f.FrameMetadataLength = static_cast<int>(d_->seqMeta.size() + 1);
  }
  // A keyframe request, or a frame libomt dropped for a slow receiver, is for this source's source to answer.
  OMTStatistics omt = {};
  omt_send_getvideostatistics(d_->tx, &omt);
  const uint64_t dropped = static_cast<uint64_t>(omt.FramesDropped);
  if (d_->askedForKeyframe() || dropped > d_->codedDropped) d_->forward = true;
  d_->codedDropped = dropped;
  omt_send(d_->tx, &f);
  std::lock_guard lock(d_->statsMutex);
  d_->stats.encoder = "none";
  ++d_->stats.framesSent;
  d_->stats.keyframes += c.mode == Mode::Hx && c.keyframe ? 1 : 0;
}

bool Sender::keyframeRequested() { return d_->forward.exchange(false); }

void Sender::sendAudio(std::span<const float> planar, int sampleRate, int channels, int64_t timestamp) {
  if (channels <= 0 || channels > 32 || planar.empty() || planar.size() % static_cast<size_t>(channels) != 0)
    throw std::invalid_argument("audio must hold the same number of samples for each of 1 to 32 channels");
  OMTMediaFrame f = {};
  f.Type = OMTFrameType_Audio;
  f.Codec = OMTCodec_FPA1;
  f.Timestamp = timestamp;
  f.SampleRate = sampleRate;
  f.Channels = channels;
  f.SamplesPerChannel = static_cast<int>(planar.size() / static_cast<size_t>(channels));
  f.Data = const_cast<float*>(planar.data());
  f.DataLength = static_cast<int>(planar.size() * sizeof(float));
  omt_send(d_->tx, &f);
}

void Sender::requestKeyframe() { d_->appKeyframe = true; }

Tally Sender::tally() {
  OMTTally t = {};
  omt_send_gettally(d_->tx, 0, &t);
  return Tally{t.preview != 0, t.program != 0};
}

SenderStats Sender::stats() {
  OMTStatistics omt = {};
  omt_send_getvideostatistics(d_->tx, &omt);
  std::lock_guard lock(d_->statsMutex);
  SenderStats s = d_->stats;
  s.bytesSent = static_cast<uint64_t>(omt.BytesSent);
  s.framesDropped = static_cast<uint64_t>(omt.FramesDropped);
  s.connections = omt_send_connections(d_->tx);
  return s;
}

std::string Sender::address() const {
  char buf[OMT_MAX_STRING_LENGTH] = {};
  omt_send_getaddress(d_->tx, buf, sizeof(buf));
  return buf;
}

// ---------------------------------------------------------------- Receiver

struct Receiver::Impl {
  omt_receive_t* rx = nullptr;
  PixelFormat pixels = PixelFormat::Bgra;
  std::unique_ptr<Decoder> decoder;
  AlphaDecoder alphaDecoder;  // VMX alpha
  std::shared_ptr<std::vector<uint8_t>> alpha;  // the last VMX alpha plane, which a frame may repeat
  // The alpha of each frame the decoder still holds, by timestamp: a software decoder hands pictures back late.
  std::deque<std::pair<int64_t, std::shared_ptr<const std::vector<uint8_t>>>> pendingAlpha;
  Packer packer;
  Nv12 picture;
  bool waiting = true;  // Hx: discard until a keyframe
  bool haveCodec = false;
  Codec lastCodec = Codec::Hevc;
  bool lastVmx = false;
  bool haveLast = false;
  int64_t lastTimestamp = 0;
  bool haveSeq = false;  // the source numbers its frames (this version on); older ones are judged by timestamp
  uint64_t lastSeq = 0;
  uint64_t count = 0;    // Hx frames seen, the number given to frames of a source that does not number them
  Clock::time_point lastAsk{};
  std::atomic<bool> askPending{false};  // requestKeyframe, served on the receiving thread
  std::mutex statsMutex;
  ReceiverStats stats;

  // Asks the sender for a keyframe, at most once per 500 ms. Receiving thread only.
  void ask() {
    const auto now = Clock::now();
    if (now - lastAsk < std::chrono::milliseconds(500)) return;
    lastAsk = now;
    OMTMediaFrame m = {};
    m.Type = OMTFrameType_Metadata;
    m.Data = const_cast<char*>(kKeyframeRequest);
    m.DataLength = sizeof(kKeyframeRequest);
    omt_receive_send(rx, &m);
  }

  // Counts a discarded frame and asks for a keyframe.
  void discard() {
    {
      std::lock_guard lock(statsMutex);
      ++stats.framesDiscarded;
    }
    ask();
  }

  // Hx: true when this access unit can be used. A lost coded frame breaks the picture until the next keyframe;
  // a frame the app skipped does not, as the encoder never saw it. Only the frame number tells them apart.
  // vmx: the frame has VMX alpha. VMX frames stand alone, so only the fill decides what is a keyframe.
  bool admit(const OMTMediaFrame* f, Codec codec, std::span<const uint8_t> au, bool vmx, uint64_t& seqOut) {
    // The source came back with the other codec, or started or stopped sending VMX alpha.
    if (haveCodec && (codec != lastCodec || vmx != lastVmx)) waiting = true;
    haveCodec = true;
    lastCodec = codec;
    lastVmx = vmx;
    uint64_t seq = 0;
    const bool numbered = parseSeq(metadataOf(f), seq);
    if (numbered) {
      if (haveSeq && seq != lastSeq + 1) waiting = true;
      lastSeq = seq;
    } else if (haveLast && isGap(lastTimestamp, f->Timestamp, f->FrameRateN, f->FrameRateD)) {
      waiting = true;
    }
    haveSeq = numbered;
    lastTimestamp = f->Timestamp;
    haveLast = true;
    seqOut = numbered ? seq : count;
    ++count;
    if (waiting && !isKeyframe(codec, au)) {
      discard();
      return false;
    }
    return true;
  }

  // This frame's alpha plane: its VMX alpha decoded, or the last plane again. Null when there is none to be had.
  std::shared_ptr<const std::vector<uint8_t>> takeAlpha(const VideoFormat& fmt, AlphaPart part,
                                                        std::span<const uint8_t> vmx) {
    if (part.repeats) return alpha && alpha->size() == static_cast<size_t>(fmt.width) * fmt.height ? alpha : nullptr;
    if (!alpha || alpha.use_count() > 1) alpha = std::make_shared<std::vector<uint8_t>>();  // others hold the old one
    if (!alphaDecoder.decode(vmx, fmt.width, fmt.height, *alpha)) alpha.reset();
    return alpha;
  }
};

Receiver::Receiver(const std::string& address, PixelFormat pixels) : d_(std::make_unique<Impl>()) {
  d_->pixels = pixels;
  const bool uyvy = pixels == PixelFormat::Uyvy || pixels == PixelFormat::Gpu;
  d_->rx = omt_receive_create(
      address.c_str(), static_cast<OMTFrameType>(OMTFrameType_Video | OMTFrameType_Audio | OMTFrameType_Metadata),
      uyvy ? OMTPreferredVideoFormat_UYVYorUYVA : OMTPreferredVideoFormat_BGRA,
      pixels == PixelFormat::Coded ? OMTReceiveFlags_CompressedOnly : OMTReceiveFlags_None);
  if (!d_->rx) throw std::runtime_error("cannot connect to the OMT source \"" + address + "\"");
  if (pixels == PixelFormat::Coded) d_->stats.decoder = "none";
}

Receiver::~Receiver() { omt_receive_destroy(d_->rx); }

bool Receiver::receiveVideo(VideoFrame& out, int timeoutMs) {
  if (d_->pixels == PixelFormat::Coded) return false;
  if (d_->askPending.exchange(false)) d_->ask();
  OMTMediaFrame* f = omt_receive(d_->rx, OMTFrameType_Video, timeoutMs);
  if (!f) return false;
  VideoFormat fmt = formatOf(f);

  Codec codec = Codec::Hevc;
  if (!codecFromTag(static_cast<int>(f->Codec), codec)) {
    // Plain OMT: libomt already decoded VMX to the format asked for.
    const bool uyvy = d_->pixels != PixelFormat::Bgra;
    if (!f->Data || fmt.width <= 0 || fmt.height <= 0) return false;
    if (uyvy ? (f->Codec != OMTCodec_UYVY && f->Codec != OMTCodec_UYVA) : f->Codec != OMTCodec_BGRA) return false;
    if (uyvy) fmt.alpha = fmt.alpha && f->Codec == OMTCodec_UYVA;
    const size_t rowBytes = static_cast<size_t>(fmt.width) * (uyvy ? 2 : 4);
    const size_t plane = static_cast<size_t>(f->Stride) * fmt.height;
    const size_t needed =  // the bytes the copies below read
        uyvy && fmt.alpha ? plane + static_cast<size_t>(f->Stride / 2) * (fmt.height - 1) + rowBytes / 2 : plane;
    if (static_cast<size_t>(f->Stride) < rowBytes || f->DataLength < 0 || static_cast<size_t>(f->DataLength) < needed)
      return false;  // a header that does not match its data
    const auto* data = static_cast<const uint8_t*>(f->Data);
    std::vector<uint8_t>& dst = uyvy ? out.uyvy : out.bgra;
    dst.resize(rowBytes * fmt.height + (uyvy && fmt.alpha ? rowBytes / 2 * fmt.height : 0));
    for (int r = 0; r < fmt.height; ++r)
      std::memcpy(dst.data() + r * rowBytes, data + static_cast<size_t>(r) * f->Stride, rowBytes);
    if (uyvy && fmt.alpha) {  // libomt's UYVA: the alpha plane follows, rows Stride / 2 apart
      const size_t alphaRow = rowBytes / 2;
      for (int r = 0; r < fmt.height; ++r)
        std::memcpy(dst.data() + rowBytes * fmt.height + r * alphaRow,
                    data + plane + static_cast<size_t>(r) * (f->Stride / 2), alphaRow);
    }
    // Without alpha libomt decodes BGRX, and libvmx fills its alpha with 255 (VMX_DecodeBGRX), so BGRA is opaque.
    out.gpu.reset();
    out.format = fmt;
    out.premultiplied = fmt.alpha && (f->Flags & OMTVideoFlags_PreMultiplied) != 0;
    out.timestamp = f->Timestamp;
    std::lock_guard lock(d_->statsMutex);
    d_->stats.mode = Mode::Full;
    d_->stats.decoder = "vmx";
    ++d_->stats.framesReceived;
    return true;
  }

  // OMT-HX: the patched libomt hands the HEVC or H.264 access unit over untouched.
  if (!f->CompressedData || f->CompressedLength <= 0) return false;
  const std::span<const uint8_t> data(static_cast<const uint8_t*>(f->CompressedData),
                                      static_cast<size_t>(f->CompressedLength));
  std::span<const uint8_t> au, vmx;
  AlphaPart part;
  if (!splitFrame(f, data, au, vmx, part)) {
    d_->waiting = true;
    d_->discard();
    return false;
  }
  const bool vmxAlpha = fmt.alpha && part.any();  // otherwise alpha, if any, is stacked under the fill
  if (d_->decoder && d_->decoder->codec() != codec) d_->decoder.reset();  // the source came back with the other codec
  uint64_t seq = 0;
  if (!d_->admit(f, codec, au, vmxAlpha, seq)) return false;
  if (!d_->decoder) d_->decoder = std::make_unique<Decoder>(codec);
  auto fail = [&] {
    d_->waiting = true;
    d_->pendingAlpha.clear();
    d_->discard();
    return false;
  };

  const auto t0 = Clock::now();
  if (vmxAlpha) {
    auto a = d_->takeAlpha(fmt, part, vmx);
    if (!a) return fail();
    d_->pendingAlpha.emplace_back(f->Timestamp, std::move(a));
    if (d_->pendingAlpha.size() > 32) d_->pendingAlpha.pop_front();  // far more than a decoder holds
  }
  int64_t timestamp = 0;
  const bool gpu = d_->pixels == PixelFormat::Gpu;
  std::shared_ptr<GpuPicture> picture = gpu ? std::make_shared<GpuPicture>() : nullptr;
  const Decoded result = gpu ? d_->decoder->decodeFrame(au, f->Timestamp, picture->frame, timestamp)
                             : d_->decoder->decode(au, f->Timestamp, d_->picture, timestamp);
  if (result == Decoded::NeedMore) {  // the decoder took the keyframe and is filling its pipeline
    d_->waiting = false;
    return false;
  }
  if (result != Decoded::Picture) return fail();
  std::shared_ptr<const std::vector<uint8_t>> alpha;  // the alpha of the frame the picture came from
  if (vmxAlpha) {
    while (!d_->pendingAlpha.empty() && !alpha) {
      if (d_->pendingAlpha.front().first == timestamp) alpha = d_->pendingAlpha.front().second;
      d_->pendingAlpha.pop_front();
    }
    if (!alpha) return fail();
  }
  bool ok = true;
  if (gpu) {
    ok = picture->frame->width == fmt.width && picture->frame->height == (vmxAlpha ? fmt.height : codedHeight(fmt));
    picture->alpha = alpha;
  } else {
    try {
      if (d_->pixels == PixelFormat::Uyvy) {
        unpackUyvy(fmt, d_->picture, out.uyvy, alpha ? alpha->data() : nullptr);
      } else {
        d_->packer.unpack(fmt, d_->picture, out.bgra, alpha ? alpha->data() : nullptr);
      }
    } catch (const std::invalid_argument&) {
      ok = false;  // the picture does not match its header: treat it like a decode error
    }
  }
  if (!ok) return fail();
  d_->waiting = false;
  out.gpu = std::move(picture);
  out.format = fmt;
  out.premultiplied = fmt.alpha;
  out.timestamp = timestamp;
  std::lock_guard lock(d_->statsMutex);
  d_->stats.mode = Mode::Hx;
  d_->stats.codec = codec;
  d_->stats.decoder = d_->decoder->name() + (vmxAlpha ? " + vmx alpha" : "");
  d_->stats.lastDecodeMs = msSince(t0);
  ++d_->stats.framesReceived;
  return true;
}

bool Receiver::receiveCoded(CodedFrame& out, int timeoutMs) {
  if (d_->pixels != PixelFormat::Coded) return false;
  if (d_->askPending.exchange(false)) d_->ask();
  const OMTMediaFrame* f = omt_receive(d_->rx, OMTFrameType_Video, timeoutMs);
  if (!f || !f->CompressedData || f->CompressedLength <= 0 || f->Width <= 0 || f->Height <= 0) return false;
  const std::span<const uint8_t> data(static_cast<const uint8_t*>(f->CompressedData),
                                      static_cast<size_t>(f->CompressedLength));
  Codec codec = Codec::Hevc;
  if (codecFromTag(static_cast<int>(f->Codec), codec)) {
    std::span<const uint8_t> fill, vmx;
    AlphaPart part;
    if (!splitFrame(f, data, fill, vmx, part)) return false;
    uint64_t seq = 0;
    if (!d_->admit(f, codec, fill, part.any(), seq)) return false;
    d_->waiting = false;
    out.mode = Mode::Hx;
    out.codec = codec;
    out.keyframe = isKeyframe(codec, fill);
    out.seq = seq;
    out.alphaBytes = part.bytes;
    out.alphaRepeats = part.repeats;
  } else if (f->Codec == OMTCodec_VMX1) {
    out.mode = Mode::Full;
    out.keyframe = true;
    out.seq = 0;
    out.alphaBytes = 0;
    out.alphaRepeats = false;
  } else {
    return false;
  }
  out.format = formatOf(f);
  out.timestamp = f->Timestamp;
  out.flags = static_cast<int>(f->Flags);
  out.colorSpace = static_cast<int>(f->ColorSpace);
  out.data.assign(data.begin(), data.end());
  std::lock_guard lock(d_->statsMutex);
  d_->stats.mode = out.mode;
  d_->stats.codec = codec;
  ++d_->stats.framesReceived;
  return true;
}

void Receiver::requestKeyframe() { d_->askPending = true; }

bool Receiver::receiveAudio(AudioFrame& out, int timeoutMs) {
  OMTMediaFrame* f = omt_receive(d_->rx, OMTFrameType_Audio, timeoutMs);
  if (!f || !f->Data || f->Channels <= 0 || f->SamplesPerChannel <= 0) return false;
  out.sampleRate = f->SampleRate;
  out.channels = f->Channels;
  out.samples = f->SamplesPerChannel;
  out.timestamp = f->Timestamp;
  const float* p = static_cast<const float*>(f->Data);
  out.planar.assign(p, p + static_cast<size_t>(f->Channels) * f->SamplesPerChannel);
  return true;
}

void Receiver::setTally(Tally tally) {
  OMTTally t = {tally.preview ? 1 : 0, tally.program ? 1 : 0};
  omt_receive_settally(d_->rx, &t);
}

ReceiverStats Receiver::stats() {
  OMTStatistics omt = {};
  omt_receive_getvideostatistics(d_->rx, &omt);
  std::lock_guard lock(d_->statsMutex);
  ReceiverStats s = d_->stats;
  s.bytesReceived = static_cast<uint64_t>(omt.BytesReceived);
  return s;
}

bool probe(const std::string& address, int timeoutMs, SourceInfo& out) {
  // Preview: a plain OMT sender sends only the preview part of each VMX frame, and the header keeps the full
  // size. CompressedOnly: libomt does not decode, so it does not rewrite the header to the preview's size.
  omt_receive_t* rx = omt_receive_create(
      address.c_str(), OMTFrameType_Video, OMTPreferredVideoFormat_BGRA,
      static_cast<OMTReceiveFlags>(OMTReceiveFlags_Preview | OMTReceiveFlags_CompressedOnly));
  if (!rx) return false;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  bool found = false;
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0) break;
    const OMTMediaFrame* f = omt_receive(rx, OMTFrameType_Video, static_cast<int>(left));
    if (!f || f->Width <= 0 || f->Height <= 0) continue;
    Codec codec = Codec::Hevc;
    out.mode = codecFromTag(static_cast<int>(f->Codec), codec) ? Mode::Hx : Mode::Full;
    out.codec = codec;
    out.format = formatOf(f);
    found = true;
    break;
  }
  omt_receive_destroy(rx);
  return found;
}

std::vector<std::string> discover() {
  int count = 0;
  char** names = omt_discovery_getaddresses(&count);
  std::vector<std::string> out;
  for (int i = 0; names && i < count; ++i) out.emplace_back(names[i]);
  return out;
}

}  // namespace omthx
