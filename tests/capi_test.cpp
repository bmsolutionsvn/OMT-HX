// The C API the OBS plugin calls: a sender and a receiver in each mode, with video, audio and tally.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "omthx/omthx_c.h"

namespace {
struct Pump {
  omthx_c_sender* sender = nullptr;
  omthx_c_video_format format{320, 180, 60, 1, 0};
  std::vector<uint8_t> bgra;
  int64_t timestamp = 0;

  explicit Pump(const char* name, int mode, int codec) {
    sender = omthx_c_sender_open(name, mode, OMTHX_C_QUALITY_MEDIUM, codec);
    bgra.resize(static_cast<size_t>(format.width) * format.height * 4);
    for (size_t i = 0; i < bgra.size(); i += 4) {
      bgra[i] = static_cast<uint8_t>(i / 4);
      bgra[i + 1] = static_cast<uint8_t>(200);
      bgra[i + 2] = 64;
      bgra[i + 3] = 255;
    }
  }
  ~Pump() { omthx_c_sender_close(sender); }

  bool sendVideo() {
    const int ok = omthx_c_sender_video(sender, &format, OMTHX_C_PIXEL_BGRA, bgra.data(), format.width * 4, 0,
                                        timestamp);
    timestamp += 166667;
    return ok == 1;
  }
};
}  // namespace

TEST_CASE("the C API sends and receives an Hx picture") {
  Pump pump("capi-hx", OMTHX_C_MODE_HX, OMTHX_C_CODEC_H264);
  REQUIRE_MESSAGE(pump.sender != nullptr, omthx_c_error());
  REQUIRE(pump.sendVideo());
  // probe reads a frame that arrives after it connects, so keep the source sending meanwhile.
  std::atomic<bool> sending{true};
  std::thread pumpThread([&] {
    while (sending) {
      pump.sendVideo();
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  });
  omthx_c_video_format probed{};
  int probeMode = -1, probeCodec = -1;
  const int probeOk = omthx_c_probe(omthx_c_sender_address(pump.sender), 3000, &probed, &probeMode, &probeCodec);
  sending = false;
  pumpThread.join();
  CHECK(probeOk == 1);
  CHECK(probeMode == OMTHX_C_MODE_HX);
  CHECK(probeCodec == OMTHX_C_CODEC_H264);
  CHECK(probed.width == pump.format.width);
  omthx_c_receiver* receiver = omthx_c_receiver_open(omthx_c_sender_address(pump.sender));
  REQUIRE_MESSAGE(receiver != nullptr, omthx_c_error());
  omthx_c_receiver_keyframe(receiver);

  bool got = false;
  omthx_c_video_format out{};
  const uint8_t* pixels = nullptr;
  int64_t timestamp = 0;
  for (int i = 0; i < 400 && !got; ++i) {
    REQUIRE(pump.sendVideo());
    got = omthx_c_receiver_video(receiver, 10, &out, nullptr, &pixels, &timestamp) == 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  CHECK(got);
  CHECK(out.width == pump.format.width);
  CHECK(out.height == pump.format.height);
  CHECK(pixels != nullptr);
  CHECK(timestamp >= 0);
  CHECK(omthx_c_receiver_mode(receiver) == OMTHX_C_MODE_HX);
  CHECK(omthx_c_receiver_codec(receiver) == OMTHX_C_CODEC_H264);
  CHECK(omthx_c_receiver_decoder(receiver)[0] != '\0');
  omthx_c_receiver_close(receiver);
}

TEST_CASE("the C API carries audio and tally") {
  Pump pump("capi-audio", OMTHX_C_MODE_FULL, OMTHX_C_CODEC_HEVC);
  REQUIRE_MESSAGE(pump.sender != nullptr, omthx_c_error());
  omthx_c_receiver* receiver = omthx_c_receiver_open(omthx_c_sender_address(pump.sender));
  REQUIRE_MESSAGE(receiver != nullptr, omthx_c_error());
  omthx_c_receiver_tally(receiver, 1, 0);

  std::vector<float> block(480 * 2, 0.25f);
  bool got = false;
  const float* planar = nullptr;
  int rate = 0, channels = 0, samples = 0;
  int64_t timestamp = 0;
  for (int i = 0; i < 100 && !got; ++i) {
    REQUIRE(omthx_c_sender_audio(pump.sender, block.data(), 48000, 2, 480, pump.timestamp));
    got = omthx_c_receiver_audio(receiver, 10, &planar, &rate, &channels, &samples, &timestamp) == 1;
    pump.sendVideo();
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  CHECK(got);
  CHECK(rate == 48000);
  CHECK(channels == 2);
  CHECK(samples == 480);
  CHECK(planar != nullptr);
  omthx_c_receiver_close(receiver);
}

TEST_CASE("the C API sends plain OMT too") {
  Pump pump("capi-full", OMTHX_C_MODE_FULL, OMTHX_C_CODEC_HEVC);
  REQUIRE_MESSAGE(pump.sender != nullptr, omthx_c_error());
  omthx_c_receiver* receiver = omthx_c_receiver_open(omthx_c_sender_address(pump.sender));
  REQUIRE_MESSAGE(receiver != nullptr, omthx_c_error());

  bool got = false;
  omthx_c_video_format out{};
  const uint8_t* pixels = nullptr;
  int64_t timestamp = 0;
  for (int i = 0; i < 400 && !got; ++i) {
    REQUIRE(pump.sendVideo());
    got = omthx_c_receiver_video(receiver, 10, &out, nullptr, &pixels, &timestamp) == 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  CHECK(got);
  CHECK(out.width == pump.format.width);
  CHECK(pixels != nullptr);
  CHECK(omthx_c_receiver_mode(receiver) == OMTHX_C_MODE_FULL);
  omthx_c_receiver_close(receiver);
}