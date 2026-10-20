#include "codec.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

#include <cstring>
#include <stdexcept>
#include <string>

namespace omthx {
namespace {
// Take D3D11 when FFmpeg offers it; otherwise the software format, which FFmpeg lists last.
AVPixelFormat pickFormat(AVCodecContext*, const AVPixelFormat* formats) {
  const AVPixelFormat* p = formats;
  for (; *p != AV_PIX_FMT_NONE; ++p) {
    if (*p == AV_PIX_FMT_D3D11) return *p;
  }
  return *(p - 1);
}

void copyPlane(uint8_t* dst, const uint8_t* src, int srcStride, int bytes, int rows) {
  for (int r = 0; r < rows; ++r) std::memcpy(dst + static_cast<size_t>(r) * bytes, src + static_cast<size_t>(r) * srcStride, bytes);
}
}  // namespace

Decoder::Decoder(Codec codec) : codec_(codec) {
  const std::string base = codecName(codec);
  const AVCodec* av = avcodec_find_decoder(codec == Codec::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
  if (!av) throw std::runtime_error("FFmpeg was built without the " + base + " decoder");
  ctx_ = avcodec_alloc_context3(av);
  ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  if (av_hwdevice_ctx_create(&hw_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0) {
    ctx_->hw_device_ctx = av_buffer_ref(hw_);
    ctx_->get_format = pickFormat;
    // Surfaces a Gpu receiver's app and the encoders it feeds may still hold while the decoder goes on.
    ctx_->extra_hw_frames = 8;
    name_ = base + " (d3d11va)";
  } else {
    // ponytail: four frame threads keep 1080p60 in real time but add three frames of delay.
    // Hardware decoding is the real path; this is the fallback for a PC without it.
    ctx_->thread_count = 4;
    name_ = base + " (software)";
  }
  if (avcodec_open2(ctx_, av, nullptr) < 0) {
    avcodec_free_context(&ctx_);
    av_buffer_unref(&hw_);
    throw std::runtime_error("cannot open the " + base + " decoder");
  }
  frame_ = av_frame_alloc();
  packet_ = av_packet_alloc();
}

Decoder::~Decoder() {
  av_packet_free(&packet_);
  av_frame_free(&frame_);
  avcodec_free_context(&ctx_);
  av_buffer_unref(&hw_);
}

Decoded Decoder::decodeFrame(std::span<const uint8_t> au, int64_t timestamp, AVFrame* out, int64_t& outTimestamp) {
  if (au.empty()) return Decoded::Error;
  // The packet borrows the caller's bytes; FFmpeg copies them, with the padding it needs, in send_packet.
  packet_->data = const_cast<uint8_t*>(au.data());
  packet_->size = static_cast<int>(au.size());
  packet_->pts = timestamp;
  int rc = avcodec_send_packet(ctx_, packet_);
  packet_->data = nullptr;
  packet_->size = 0;
  if (rc < 0) {
    avcodec_flush_buffers(ctx_);
    return Decoded::Error;
  }
  av_frame_unref(out);
  rc = avcodec_receive_frame(ctx_, out);
  if (rc == AVERROR(EAGAIN)) return Decoded::NeedMore;
  if (rc < 0) {
    avcodec_flush_buffers(ctx_);
    return Decoded::Error;
  }
  if (out->format != AV_PIX_FMT_D3D11) name_ = std::string(codecName(codec_)) + " (software)";
  outTimestamp = out->pts;
  return Decoded::Picture;
}

// ponytail: the decoded picture is copied to CPU memory. A Gpu receiver keeps it in video memory instead.
Decoded Decoder::decode(std::span<const uint8_t> au, int64_t timestamp, Nv12& out, int64_t& outTimestamp) {
  const Decoded d = decodeFrame(au, timestamp, frame_, outTimestamp);
  if (d != Decoded::Picture) return d;
  const bool ok = downloadNv12(frame_, out);
  av_frame_unref(frame_);
  return ok ? Decoded::Picture : Decoded::Error;
}

bool downloadNv12(const AVFrame* frame, Nv12& out) {
  AVFrame* sw = nullptr;
  const AVFrame* f = frame;
  if (frame->format == AV_PIX_FMT_D3D11) {
    sw = av_frame_alloc();
    if (av_hwframe_transfer_data(sw, frame, 0) < 0) {
      av_frame_free(&sw);
      return false;
    }
    f = sw;
  }
  const int w = f->width, h = f->height;
  const bool nv12 = f->format == AV_PIX_FMT_NV12;
  const bool planar = f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P;
  const bool ok = w % 2 == 0 && h % 2 == 0 && (nv12 || planar);  // 10-bit and 4:2:2 are not OMT-HX version 1
  if (ok) {
    out.width = w;
    out.height = h;
    out.y.resize(static_cast<size_t>(w) * h);
    out.uv.resize(static_cast<size_t>(w) * h / 2);
    copyPlane(out.y.data(), f->data[0], f->linesize[0], w, h);
    if (nv12) {
      copyPlane(out.uv.data(), f->data[1], f->linesize[1], w, h / 2);
    } else {
      for (int r = 0; r < h / 2; ++r) {
        const uint8_t* u = f->data[1] + static_cast<size_t>(r) * f->linesize[1];
        const uint8_t* v = f->data[2] + static_cast<size_t>(r) * f->linesize[2];
        uint8_t* d = out.uv.data() + static_cast<size_t>(r) * w;
        for (int x = 0; x < w / 2; ++x) {
          d[2 * x] = u[x];
          d[2 * x + 1] = v[x];
        }
      }
    }
  }
  av_frame_free(&sw);
  return ok;
}

bool download(VideoFrame& frame, PixelFormat pixels) {
  if (!frame.gpu || (pixels != PixelFormat::Bgra && pixels != PixelFormat::Uyvy)) return false;
  Nv12 picture;
  if (!downloadNv12(frame.gpu->frame, picture)) return false;
  const std::vector<uint8_t>* alpha = frame.gpu->alpha.get();
  if (alpha && alpha->size() != static_cast<size_t>(frame.format.width) * frame.format.height) return false;
  try {
    if (pixels == PixelFormat::Uyvy) {
      unpackUyvy(frame.format, picture, frame.uyvy, alpha ? alpha->data() : nullptr);
    } else {
      thread_local Packer packer;  // keeps its converter for the next picture of this size
      packer.unpack(frame.format, picture, frame.bgra, alpha ? alpha->data() : nullptr);
    }
  } catch (const std::invalid_argument&) {
    return false;  // the picture does not match the frame's format
  }
  return true;
}

GpuPicture::GpuPicture() : frame(av_frame_alloc()) {}
GpuPicture::~GpuPicture() { av_frame_free(&frame); }
bool GpuPicture::inVideoMemory() const { return frame->format == AV_PIX_FMT_D3D11; }

}  // namespace omthx
