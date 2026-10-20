#include "codec.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

#include <chrono>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace omthx {
namespace {
std::string averr(int rc) {
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(rc, buf, sizeof(buf));
  return buf;
}

// Each vendor's lowest-delay settings, the same for both codecs.
// An option an encoder does not know is ignored by FFmpeg.
void lowDelayOptions(const std::string& name, AVDictionary** o) {
  if (name.ends_with("_nvenc")) {
    av_dict_set(o, "preset", "p3", 0);
    av_dict_set(o, "tune", "ull", 0);
    av_dict_set(o, "rc", "cbr", 0);
    av_dict_set(o, "zerolatency", "1", 0);
    av_dict_set(o, "delay", "0", 0);
    av_dict_set(o, "forced-idr", "1", 0);
  } else if (name.ends_with("_amf")) {
    av_dict_set(o, "usage", "ultralowlatency", 0);
    av_dict_set(o, "rc", "cbr", 0);
    av_dict_set(o, "header_insertion_mode", "idr", 0);  // hevc_amf only
    av_dict_set(o, "forced_idr", "1", 0);
  } else if (name.ends_with("_qsv")) {
    av_dict_set(o, "preset", "veryfast", 0);
    av_dict_set(o, "async_depth", "1", 0);
    av_dict_set(o, "forced_idr", "1", 0);
  } else if (name.ends_with("_mf")) {
    av_dict_set(o, "rate_control", "cbr", 0);
    av_dict_set(o, "scenario", "display_remoting", 0);
    av_dict_set(o, "hw_encoding", "1", 0);
  } else if (name == "libx265") {
    av_dict_set(o, "preset", "ultrafast", 0);
    av_dict_set(o, "tune", "zerolatency", 0);
    av_dict_set(o, "x265-params", "bframes=0:repeat-headers=1", 0);
    av_dict_set(o, "forced-idr", "1", 0);  // a requested keyframe must be an IRAP, not just an I picture
  } else if (name == "libx264") {
    av_dict_set(o, "preset", "ultrafast", 0);
    av_dict_set(o, "tune", "zerolatency", 0);
  }
}
}  // namespace

const std::vector<std::string>& Encoder::order(Codec codec) {
  // The software encoders come last: a machine without a hardware one still sends Hx (Linux, or a PC the
  // GPU encoder refuses). libx265/libx264 are absent from some FFmpeg builds and then simply are not tried.
  static const std::vector<std::string> hevc{"hevc_nvenc", "hevc_amf", "hevc_qsv", "hevc_mf", "libx265"};
  static const std::vector<std::string> h264{"h264_nvenc", "h264_amf", "h264_qsv", "h264_mf", "libx264"};
  return codec == Codec::H264 ? h264 : hevc;
}

Encoder::Encoder(Codec codec, int w, int h, int fpsN, int fpsD, int64_t bitrate, AVBufferRef* gpuFrames)
    : codec_(codec) {
  std::string tried;
  for (const std::string& name : order(codec)) {
    const AVCodec* av = avcodec_find_encoder_by_name(name.c_str());
    if (!av) {
      tried += name + " (not built); ";
      continue;
    }
    AVCodecContext* c = avcodec_alloc_context3(av);
    c->width = w;
    c->height = h;
    c->time_base = {fpsD, fpsN};
    c->framerate = {fpsN, fpsD};
    c->pix_fmt = AV_PIX_FMT_NV12;
    if (name == "libx265") {  // it does not take NV12; encode() splits the chroma instead
      c->pix_fmt = AV_PIX_FMT_YUV420P;
      planar_ = true;
    }
    if (gpuFrames) {  // pictures in video memory, on the decoder's own D3D11 device: nothing is copied
      c->pix_fmt = AV_PIX_FMT_D3D11;
      c->hw_frames_ctx = av_buffer_ref(gpuFrames);
      c->hw_device_ctx = av_buffer_ref(reinterpret_cast<AVHWFramesContext*>(gpuFrames->data)->device_ref);
    }
    c->gop_size = static_cast<int>(2 * KeyframePolicy::kPeriodicMs / 1000 * fpsN / fpsD);  // beyond the KeyframePolicy
    c->max_b_frames = 0;
    c->bit_rate = bitrate;
    c->rc_max_rate = bitrate;
    c->rc_buffer_size = static_cast<int>(bitrate * 2 * fpsD / fpsN);  // about two frames
    c->color_range = AVCOL_RANGE_JPEG;
    c->colorspace = AVCOL_SPC_BT709;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_BT709;
    c->flags |= AV_CODEC_FLAG_LOW_DELAY;  // no GLOBAL_HEADER: parameter sets travel with every keyframe
    AVDictionary* o = nullptr;
    lowDelayOptions(name, &o);
    const int rc = avcodec_open2(c, av, &o);
    av_dict_free(&o);
    if (rc < 0) {
      tried += name + " (" + averr(rc) + "); ";
      avcodec_free_context(&c);
      continue;
    }
    ctx_ = c;
    // Keep the codec first in the name the stats print: "h264 (libx264)", "hevc (libx265)".
    name_ = name == "libx264" ? "h264 (libx264)" : name == "libx265" ? "hevc (libx265)" : name;
    break;
  }
  if (!ctx_) {
    throw std::runtime_error(std::string("no hardware ") + (codec == Codec::H264 ? "H.264" : "HEVC") +
                             (gpuFrames ? " encoder took D3D11 pictures: " : " encoder could be opened: ") + tried);
  }
  frame_ = av_frame_alloc();
  packet_ = av_packet_alloc();
  frameUs_ = 1'000'000LL * fpsD / fpsN;
#ifdef _WIN32
  // Sleep() waits whole timer ticks (up to 15.6 ms); this timer wakes within about half a millisecond.
  timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
}

Encoder::~Encoder() {
#ifdef _WIN32
  if (timer_) CloseHandle(timer_);
#endif
  av_packet_free(&packet_);
  av_frame_free(&frame_);
  avcodec_free_context(&ctx_);
}

std::vector<Packet> Encoder::encode(const Nv12& pic, bool forceKeyframe) {
  if (pic.width != ctx_->width || pic.height != ctx_->height)
    throw std::invalid_argument("the picture has the wrong size for this encoder");
  // The frame borrows the picture's memory; FFmpeg copies it inside avcodec_send_frame.
  av_frame_unref(frame_);
  frame_->format = planar_ ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NV12;
  frame_->width = pic.width;
  frame_->height = pic.height;
  if (planar_) {
    const int cw = pic.width / 2, ch = pic.height / 2;
    u_.resize(static_cast<size_t>(cw) * ch);
    v_.resize(static_cast<size_t>(cw) * ch);
    for (int r = 0; r < ch; ++r) {
      const uint8_t* src = pic.uv.data() + static_cast<size_t>(r) * pic.width;
      for (int x = 0; x < cw; ++x) {
        u_[static_cast<size_t>(r) * cw + x] = src[2 * x];
        v_[static_cast<size_t>(r) * cw + x] = src[2 * x + 1];
      }
    }
    frame_->data[0] = const_cast<uint8_t*>(pic.y.data());
    frame_->linesize[0] = pic.width;
    frame_->data[1] = u_.data();
    frame_->linesize[1] = cw;
    frame_->data[2] = v_.data();
    frame_->linesize[2] = cw;
  } else {
    frame_->data[0] = const_cast<uint8_t*>(pic.y.data());
    frame_->linesize[0] = pic.width;
    frame_->data[1] = const_cast<uint8_t*>(pic.uv.data());
    frame_->linesize[1] = pic.width;
  }
  return send(forceKeyframe);
}

std::vector<Packet> Encoder::encodeGpu(const AVFrame* pic, bool forceKeyframe) {
  if (pic->width != ctx_->width || pic->height != ctx_->height || pic->format != ctx_->pix_fmt)
    throw std::invalid_argument("the picture has the wrong size or kind for this encoder");
  av_frame_unref(frame_);
  if (av_frame_ref(frame_, pic) < 0) throw std::runtime_error("cannot reference the picture");
  return send(forceKeyframe);
}

std::vector<Packet> Encoder::send(bool forceKeyframe) {
  frame_->color_range = AVCOL_RANGE_JPEG;
  frame_->colorspace = AVCOL_SPC_BT709;
  frame_->pts = index_++;
  frame_->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
  const int rc = avcodec_send_frame(ctx_, frame_);
  av_frame_unref(frame_);  // the encoder holds its own reference, or its own copy
  if (rc < 0) throw std::runtime_error(std::string(codecName(codec_)) + " encode failed: " + averr(rc));
  ++owed_;
  return drain();
}

// AMF hands the packet back a few ms after the picture, not inside avcodec_send_frame: taking only what is ready
// would send every picture one frame late (16.7 ms at 60 fps). Waiting for it measured about 3 ms for 1080p on
// hevc_amf. The wait stops after one frame time, so a stalled encoder costs one frame, as before.
std::vector<Packet> Encoder::drain() {
  const int firstParameterSet = codec_ == Codec::H264 ? 7 : 32;  // SPS; VPS
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(frameUs_);
  std::vector<Packet> out;
  for (;;) {
    const int rc = avcodec_receive_packet(ctx_, packet_);
    if (rc >= 0) {
      Packet p;
      p.data.assign(packet_->data, packet_->data + packet_->size);
      p.keyframe = isKeyframe(codec_, p.data);
      // Safety net for an encoder that keeps its parameter sets out of band.
      if (p.keyframe && !hasNalType(codec_, p.data, firstParameterSet) && ctx_->extradata_size > 4 &&
          ctx_->extradata[0] == 0 && ctx_->extradata[1] == 0) {
        p.data.insert(p.data.begin(), ctx_->extradata, ctx_->extradata + ctx_->extradata_size);
      }
      out.push_back(std::move(p));
      av_packet_unref(packet_);
      owed_ = owed_ > 0 ? owed_ - 1 : 0;
      continue;
    }
    if (rc != AVERROR(EAGAIN) || owed_ == 0 || std::chrono::steady_clock::now() >= deadline) break;
#ifdef _WIN32
    if (timer_) {
      LARGE_INTEGER due{};
      due.QuadPart = -2'000;  // 200 us, in 100 ns units; negative is relative
      if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer_, INFINITE);
    } else {
      SwitchToThread();
    }
#else
    std::this_thread::sleep_for(std::chrono::microseconds(200));
#endif
  }
  return out;
}

}  // namespace omthx