#include "omthx/omthx_c.h"

#include "omthx/omthx.h"

#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

// Omthx throws; C callers cannot. Every entry point catches and leaves the reason here.
thread_local std::string g_error;

int fail(const char* message) {
  g_error = message;
  return 0;
}

int fail(const std::exception& e) {
  g_error = e.what();
  return 0;
}

omthx::Mode modeOf(int mode) { return mode == OMTHX_C_MODE_HX ? omthx::Mode::Hx : omthx::Mode::Full; }

omthx::Quality qualityOf(int quality) {
  switch (quality) {
    case OMTHX_C_QUALITY_LOW: return omthx::Quality::Low;
    case OMTHX_C_QUALITY_HIGH: return omthx::Quality::High;
    default: return omthx::Quality::Medium;
  }
}

omthx::Codec codecOf(int codec) { return codec == OMTHX_C_CODEC_H264 ? omthx::Codec::H264 : omthx::Codec::Hevc; }

omthx::VideoFormat formatOf(const omthx_c_video_format& f) {
  return omthx::VideoFormat{f.width, f.height, f.fps_n, f.fps_d, f.alpha != 0};
}

}  // namespace

struct omthx_c_sender {
  std::unique_ptr<omthx::Sender> sender;
  std::string address;
  std::vector<uint8_t> packed;  // BGRA copied to a tight stride
};

struct omthx_c_receiver {
  std::unique_ptr<omthx::Receiver> receiver;
  omthx::VideoFrame video;
  omthx::AudioFrame audio;
  std::string decoder;
};

extern "C" {

const char* omthx_c_error(void) { return g_error.c_str(); }

omthx_c_sender* omthx_c_sender_open(const char* name, int mode, int quality, int codec) {
  try {
    auto s = std::make_unique<omthx_c_sender>();
    s->sender = std::make_unique<omthx::Sender>(name ? name : "OMT", modeOf(mode), qualityOf(quality), codecOf(codec));
    s->address = s->sender->address();
    return s.release();
  } catch (const std::exception& e) {
    fail(e);
    return nullptr;
  }
}

void omthx_c_sender_close(omthx_c_sender* sender) { delete sender; }

int omthx_c_sender_video(omthx_c_sender* sender, const omthx_c_video_format* format, int pixel_format,
                         const uint8_t* data, int stride, int premultiplied, int64_t timestamp) {
  if (!sender || !format || !data) return fail("sender_video: a null argument");
  try {
    const omthx::VideoFormat fmt = formatOf(*format);
    if (pixel_format == OMTHX_C_PIXEL_UYVY) {
      if (fmt.alpha) return fail("sender_video: UYVY with alpha is not offered by this API");
      if (stride < fmt.width * 2 || stride % 2) return fail("sender_video: the UYVY stride is too small");
      sender->sender->sendVideoUyvy(fmt, data, stride, nullptr, 0, premultiplied != 0, timestamp);
      return 1;
    }
    const size_t rowBytes = static_cast<size_t>(fmt.width) * 4;
    const size_t tight = rowBytes * static_cast<size_t>(fmt.height);
    std::span<const uint8_t> bgra;
    if (static_cast<size_t>(stride) == rowBytes) {
      bgra = {data, tight};
    } else {
      if (stride < static_cast<int>(rowBytes)) return fail("sender_video: the BGRA stride is too small");
      sender->packed.resize(tight);
      for (int y = 0; y < fmt.height; ++y) {
        std::memcpy(sender->packed.data() + static_cast<size_t>(y) * rowBytes, data + static_cast<size_t>(y) * stride,
                    rowBytes);
      }
      bgra = sender->packed;
    }
    sender->sender->sendVideo(fmt, bgra, premultiplied != 0, timestamp);
    return 1;
  } catch (const std::exception& e) {
    return fail(e);
  }
}

int omthx_c_sender_video_nv12(omthx_c_sender* sender, const omthx_c_video_format* format, const uint8_t* y,
                              int y_stride, const uint8_t* uv, int uv_stride, int full_range, int64_t timestamp) {
  if (!sender || !format || !y || !uv) return fail("sender_video_nv12: a null argument");
  try {
    sender->sender->sendVideoNv12(formatOf(*format), y, y_stride, uv, uv_stride, full_range != 0, timestamp);
    return 1;
  } catch (const std::exception& e) {
    return fail(e);
  }
}

int omthx_c_sender_audio(omthx_c_sender* sender, const float* planar, int sample_rate, int channels,
                         int samples_per_channel, int64_t timestamp) {
  if (!sender || !planar || channels <= 0 || samples_per_channel <= 0) return fail("sender_audio: a null argument");
  try {
    sender->sender->sendAudio(std::span<const float>(planar, static_cast<size_t>(channels) * samples_per_channel),
                              sample_rate, channels, timestamp);
    return 1;
  } catch (const std::exception& e) {
    return fail(e);
  }
}

void omthx_c_sender_keyframe(omthx_c_sender* sender) {
  if (!sender) return;
  try {
    sender->sender->requestKeyframe();
  } catch (const std::exception& e) {
    fail(e);
  }
}

const char* omthx_c_sender_address(omthx_c_sender* sender) { return sender ? sender->address.c_str() : ""; }

omthx_c_receiver* omthx_c_receiver_open(const char* address) {
  if (!address || !*address) {
    fail("receiver_open: an empty address");
    return nullptr;
  }
  try {
    auto r = std::make_unique<omthx_c_receiver>();
    r->receiver = std::make_unique<omthx::Receiver>(address);
    r->decoder = r->receiver->stats().decoder;
    return r.release();
  } catch (const std::exception& e) {
    fail(e);
    return nullptr;
  }
}

void omthx_c_receiver_close(omthx_c_receiver* receiver) { delete receiver; }

int omthx_c_receiver_video(omthx_c_receiver* receiver, int timeout_ms, omthx_c_video_format* format,
                           int* premultiplied, const uint8_t** bgra, int64_t* timestamp) {
  if (!receiver || !format || !bgra || !timestamp) return fail("receiver_video: a null argument");
  try {
    if (!receiver->receiver->receiveVideo(receiver->video, timeout_ms)) return 0;
    const omthx::VideoFrame& f = receiver->video;
    format->width = f.format.width;
    format->height = f.format.height;
    format->fps_n = f.format.fpsN;
    format->fps_d = f.format.fpsD;
    format->alpha = f.format.alpha ? 1 : 0;
    if (premultiplied) *premultiplied = f.premultiplied ? 1 : 0;
    *bgra = f.bgra.data();
    *timestamp = f.timestamp;
    return 1;
  } catch (const std::exception& e) {
    return fail(e);
  }
}

int omthx_c_receiver_audio(omthx_c_receiver* receiver, int timeout_ms, const float** planar, int* sample_rate,
                           int* channels, int* samples_per_channel, int64_t* timestamp) {
  if (!receiver || !planar || !sample_rate || !channels || !samples_per_channel || !timestamp)
    return fail("receiver_audio: a null argument");
  try {
    if (!receiver->receiver->receiveAudio(receiver->audio, timeout_ms)) return 0;
    const omthx::AudioFrame& a = receiver->audio;
    *planar = a.planar.data();
    *sample_rate = a.sampleRate;
    *channels = a.channels;
    *samples_per_channel = a.samples;
    *timestamp = a.timestamp;
    return 1;
  } catch (const std::exception& e) {
    return fail(e);
  }
}

void omthx_c_receiver_tally(omthx_c_receiver* receiver, int preview, int program) {
  if (!receiver) return;
  try {
    receiver->receiver->setTally(omthx::Tally{preview != 0, program != 0});
  } catch (const std::exception& e) {
    fail(e);
  }
}

void omthx_c_receiver_keyframe(omthx_c_receiver* receiver) {
  if (!receiver) return;
  try {
    receiver->receiver->requestKeyframe();
  } catch (const std::exception& e) {
    fail(e);
  }
}

int omthx_c_receiver_mode(omthx_c_receiver* receiver) {
  if (!receiver) return OMTHX_C_MODE_FULL;
  try {
    return receiver->receiver->stats().mode == omthx::Mode::Hx ? OMTHX_C_MODE_HX : OMTHX_C_MODE_FULL;
  } catch (const std::exception& e) {
    fail(e);
    return OMTHX_C_MODE_FULL;
  }
}

int omthx_c_receiver_codec(omthx_c_receiver* receiver) {
  if (!receiver) return OMTHX_C_CODEC_HEVC;
  try {
    return receiver->receiver->stats().codec == omthx::Codec::H264 ? OMTHX_C_CODEC_H264 : OMTHX_C_CODEC_HEVC;
  } catch (const std::exception& e) {
    fail(e);
    return OMTHX_C_CODEC_HEVC;
  }
}

const char* omthx_c_receiver_decoder(omthx_c_receiver* receiver) {
  if (!receiver) return "";
  try {
    receiver->decoder = receiver->receiver->stats().decoder;
  } catch (const std::exception& e) {
    fail(e);
  }
  return receiver->decoder.c_str();
}

}  // extern "C"