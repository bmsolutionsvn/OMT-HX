#include "codec.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include <stdexcept>

namespace omthx {
namespace {
std::string averr(int rc) {
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(rc, buf, sizeof(buf));
  return buf;
}

// Each encoder's lowest-delay settings. An option an encoder does not know is ignored by FFmpeg.
void lowDelayOptions(const std::string& name, AVDictionary** o) {
  if (name == "hevc_nvenc") {
    av_dict_set(o, "preset", "p3", 0);
    av_dict_set(o, "tune", "ull", 0);
    av_dict_set(o, "rc", "cbr", 0);
    av_dict_set(o, "zerolatency", "1", 0);
    av_dict_set(o, "delay", "0", 0);
    av_dict_set(o, "forced-idr", "1", 0);
  } else if (name == "hevc_amf") {
    av_dict_set(o, "usage", "ultralowlatency", 0);
    av_dict_set(o, "rc", "cbr", 0);
    av_dict_set(o, "header_insertion_mode", "idr", 0);
    av_dict_set(o, "forced_idr", "1", 0);
  } else if (name == "hevc_qsv") {
    av_dict_set(o, "preset", "veryfast", 0);
    av_dict_set(o, "async_depth", "1", 0);
    av_dict_set(o, "forced_idr", "1", 0);
  } else if (name == "hevc_mf") {
    av_dict_set(o, "rate_control", "cbr", 0);
    av_dict_set(o, "scenario", "display_remoting", 0);
    av_dict_set(o, "hw_encoding", "1", 0);
  }
}
}  // namespace

const std::vector<std::string>& Encoder::order() {
  static const std::vector<std::string> order{"hevc_nvenc", "hevc_amf", "hevc_qsv", "hevc_mf"};
  return order;
}

Encoder::Encoder(int w, int h, int fpsN, int fpsD, int64_t bitrate) {
  std::string tried;
  for (const std::string& name : order()) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec) {
      tried += name + " (not built); ";
      continue;
    }
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = w;
    c->height = h;
    c->time_base = {fpsD, fpsN};
    c->framerate = {fpsN, fpsD};
    c->pix_fmt = AV_PIX_FMT_NV12;
    c->gop_size = 10 * fpsN / fpsD;  // a backstop only; the sender's KeyframePolicy asks for one every 2 s
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
    const int rc = avcodec_open2(c, codec, &o);
    av_dict_free(&o);
    if (rc < 0) {
      tried += name + " (" + averr(rc) + "); ";
      avcodec_free_context(&c);
      continue;
    }
    ctx_ = c;
    name_ = name;
    break;
  }
  if (!ctx_) throw std::runtime_error("no hardware HEVC encoder could be opened: " + tried);
  frame_ = av_frame_alloc();
  packet_ = av_packet_alloc();
}

Encoder::~Encoder() {
  av_packet_free(&packet_);
  av_frame_free(&frame_);
  avcodec_free_context(&ctx_);
}

std::vector<Packet> Encoder::encode(const Nv12& pic, bool forceKeyframe) {
  if (pic.width != ctx_->width || pic.height != ctx_->height)
    throw std::invalid_argument("the picture has the wrong size for this encoder");
  // The frame borrows the picture's memory; FFmpeg copies it inside avcodec_send_frame.
  av_frame_unref(frame_);
  frame_->format = AV_PIX_FMT_NV12;
  frame_->width = pic.width;
  frame_->height = pic.height;
  frame_->data[0] = const_cast<uint8_t*>(pic.y.data());
  frame_->linesize[0] = pic.width;
  frame_->data[1] = const_cast<uint8_t*>(pic.uv.data());
  frame_->linesize[1] = pic.width;
  frame_->color_range = AVCOL_RANGE_JPEG;
  frame_->colorspace = AVCOL_SPC_BT709;
  frame_->pts = index_++;
  frame_->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

  const int rc = avcodec_send_frame(ctx_, frame_);
  if (rc < 0) throw std::runtime_error("HEVC encode failed: " + averr(rc));
  std::vector<Packet> out;
  while (avcodec_receive_packet(ctx_, packet_) >= 0) {
    Packet p;
    p.data.assign(packet_->data, packet_->data + packet_->size);
    p.keyframe = isKeyframe(Codec::Hevc, p.data);
    // Safety net for an encoder that keeps its parameter sets out of band.
    if (p.keyframe && !hasNalType(Codec::Hevc, p.data, 32) && ctx_->extradata_size > 4 && ctx_->extradata[0] == 0 &&
        ctx_->extradata[1] == 0) {
      p.data.insert(p.data.begin(), ctx_->extradata, ctx_->extradata + ctx_->extradata_size);
    }
    out.push_back(std::move(p));
    av_packet_unref(packet_);
  }
  return out;
}

}  // namespace omthx
