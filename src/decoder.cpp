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
  sw_ = av_frame_alloc();
  packet_ = av_packet_alloc();
}

Decoder::~Decoder() {
  av_packet_free(&packet_);
  av_frame_free(&sw_);
  av_frame_free(&frame_);
  avcodec_free_context(&ctx_);
  av_buffer_unref(&hw_);
}

// ponytail: the decoded picture is copied to CPU memory. Upgrade path: hand the D3D11 texture to the app.
Decoded Decoder::decode(std::span<const uint8_t> au, int64_t timestamp, Nv12& out, int64_t& outTimestamp) {
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
  rc = avcodec_receive_frame(ctx_, frame_);
  if (rc == AVERROR(EAGAIN)) return Decoded::NeedMore;
  if (rc < 0) {
    avcodec_flush_buffers(ctx_);
    return Decoded::Error;
  }

  const AVFrame* f = frame_;
  if (frame_->format == AV_PIX_FMT_D3D11) {
    av_frame_unref(sw_);
    if (av_hwframe_transfer_data(sw_, frame_, 0) < 0) {
      av_frame_unref(frame_);
      return Decoded::Error;
    }
    f = sw_;
  } else {
    name_ = std::string(codecName(codec_)) + " (software)";
  }

  const int w = f->width, h = f->height;
  const bool nv12 = f->format == AV_PIX_FMT_NV12;
  const bool planar = f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P;
  if (w % 2 || h % 2 || !(nv12 || planar)) {  // 10-bit and 4:2:2 are not OMT-HX version 1
    av_frame_unref(frame_);
    av_frame_unref(sw_);
    return Decoded::Error;
  }
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
  outTimestamp = frame_->pts;
  av_frame_unref(frame_);
  av_frame_unref(sw_);
  return Decoded::Picture;
}

}  // namespace omthx
