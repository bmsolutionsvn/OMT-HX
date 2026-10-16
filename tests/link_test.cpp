#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "codec.h"
#include "omthx/omthx.h"
#include "pattern.h"

using namespace omthx;
using Clock = std::chrono::steady_clock;

namespace {
constexpr int64_t kFrame = 166'667;  // one frame at 60 fps, in OMT units

bool hardware(Codec codec) {
  try {
    Encoder probe(codec, 640, 360, 60, 1, 1'000'000);
    return true;
  } catch (const std::runtime_error& e) {
    MESSAGE("SKIPPED, no hardware " << codecName(codec) << " encoder on this PC: " << e.what());
    return false;
  }
}

// Sends the test picture and a tone at 60 fps on its own thread until destroyed.
class Pump {
 public:
  Pump(Sender& sender, const VideoFormat& format)
      : thread_([this, &sender, format] {
          std::vector<uint8_t> bgra;
          const std::vector<float> tone(2 * 800, 0.25f);
          auto next = Clock::now();
          for (int i = 0; !stop_; ++i) {
            drawPattern(bgra, format.width, format.height, i, format.alpha);
            const int64_t timestamp = 1'000'000 + i * kFrame;
            sender.sendVideo(format, bgra, false, timestamp);
            sender.sendAudio(tone, 48000, 2, timestamp);
            next += std::chrono::microseconds(16'667);
            std::this_thread::sleep_until(next);
          }
        }) {}
  ~Pump() {
    stop_ = true;
    thread_.join();
  }

 private:
  std::atomic<bool> stop_{false};  // declared before thread_, which reads it
  std::thread thread_;
};

int receiveSome(Receiver& receiver, VideoFrame& frame, int wanted, int seconds) {
  int got = 0;
  const auto deadline = Clock::now() + std::chrono::seconds(seconds);
  while (got < wanted && Clock::now() < deadline) {
    if (receiver.receiveVideo(frame, 100)) ++got;
  }
  return got;
}
}  // namespace

TEST_CASE("an HX source delivers pictures with alpha, audio and tally") {
  Codec codec = Codec::Hevc;
  SUBCASE("hevc") { codec = Codec::Hevc; }
  SUBCASE("h264") { codec = Codec::H264; }
  if (!hardware(codec)) return;
  const VideoFormat f{640, 360, 60, 1, true};
  Sender sender(std::string("omthx-test-hx-") + codecName(codec), Mode::Hx, Quality::Medium, codec);
  Receiver receiver(sender.address());
  Pump pump(sender, f);

  VideoFrame frame;
  REQUIRE(receiveSome(receiver, frame, 30, 10) == 30);
  CHECK(frame.format == f);
  CHECK(frame.premultiplied);
  REQUIRE(frame.bgra.size() == 640u * 360 * 4);
  CHECK(frame.bgra[3] < 16);  // the corner is outside the disc
  int opaque = 0;
  for (size_t i = 3; i < frame.bgra.size(); i += 4) opaque += frame.bgra[i] > 240;
  CHECK(opaque > 10'000);  // the disc itself
  CHECK(receiver.stats().mode == Mode::Hx);
  CHECK(receiver.stats().codec == codec);
  CHECK(receiver.stats().decoder.rfind(codecName(codec), 0) == 0);
  CHECK(sender.stats().encoder.rfind(codecName(codec), 0) == 0);
  CHECK(receiver.stats().framesReceived >= 30);
  CHECK(sender.stats().keyframes >= 1);
  CHECK(sender.stats().connections >= 1);

  AudioFrame audio;
  bool heard = false;
  for (int i = 0; i < 30 && !heard; ++i) heard = receiver.receiveAudio(audio, 100);
  REQUIRE(heard);
  CHECK(audio.sampleRate == 48000);
  CHECK(audio.channels == 2);
  CHECK(audio.samples == 800);
  REQUIRE(audio.planar.size() == 1600);
  CHECK(audio.planar.front() == 0.25f);
  CHECK(audio.planar.back() == 0.25f);

  receiver.setTally(Tally{false, true});
  Tally seen;
  for (int i = 0; i < 40 && !seen.program; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    seen = sender.tally();
  }
  CHECK(seen.program);
  CHECK_FALSE(seen.preview);
}

TEST_CASE("a plain OMT source comes through the same Receiver") {
  const VideoFormat f{640, 360, 60, 1, true};
  Sender sender("omthx-test-full", Mode::Full, Quality::Medium, Codec::H264);  // Full ignores the codec
  Receiver receiver(sender.address());
  Pump pump(sender, f);

  VideoFrame frame;
  REQUIRE(receiveSome(receiver, frame, 10, 10) == 10);
  CHECK(frame.format == f);
  CHECK_FALSE(frame.premultiplied);  // the pump sends straight alpha and plain OMT keeps it
  CHECK(frame.bgra.size() == 640u * 360 * 4);
  CHECK(receiver.stats().mode == Mode::Full);
  CHECK(receiver.stats().decoder == "vmx");
  CHECK(sender.stats().encoder == "vmx");
}

TEST_CASE("a receiver that joins a running HX source gets a picture") {
  Codec codec = Codec::Hevc;
  SUBCASE("hevc") { codec = Codec::Hevc; }
  SUBCASE("h264") { codec = Codec::H264; }
  if (!hardware(codec)) return;
  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender(std::string("omthx-test-late-") + codecName(codec), Mode::Hx, Quality::Medium, codec);
  Pump pump(sender, f);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  const uint64_t before = sender.stats().keyframes;

  const auto t0 = Clock::now();
  Receiver receiver(sender.address());
  VideoFrame frame;
  REQUIRE(receiveSome(receiver, frame, 1, 5) == 1);
  const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  MESSAGE("first picture " << waited << " ms after connecting");
  CHECK(waited < 2500);
  CHECK(sender.stats().keyframes > before);
  CHECK(frame.format == f);
  CHECK(frame.bgra[3] == 255);  // no alpha in this source
}

TEST_CASE("a frame the app skips does not freeze the receiver") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender("omthx-test-skip", Mode::Hx, Quality::Medium);
  Receiver receiver(sender.address());
  std::vector<uint8_t> bgra;
  VideoFrame frame;
  int sent = 0, got = 0;
  uint64_t discardedAtFirst = 0;
  auto next = Clock::now();
  for (int tick = 0; tick < 600 && sent < 240; ++tick) {
    if (got > 0 && tick % 4 == 3) continue;  // once playing, skip every fourth tick, as a late camera does
    drawPattern(bgra, f.width, f.height, tick, false);
    sender.sendVideo(f, bgra, false, 1'000'000 + tick * kFrame);
    if (got > 0) ++sent;
    next += std::chrono::microseconds(16'667);
    while (receiver.receiveVideo(frame, 0)) {
      if (got++ == 0) discardedAtFirst = receiver.stats().framesDiscarded;
    }
    std::this_thread::sleep_until(next);
  }
  receiveSome(receiver, frame, 1, 1);  // the last one or two still in flight
  REQUIRE(sent == 240);
  CHECK(receiver.stats().framesDiscarded == discardedAtFirst);
  CHECK(receiver.stats().framesReceived >= 230);
}

TEST_CASE("a change of picture size reaches the receiver") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat small{640, 360, 60, 1, false};
  const VideoFormat large{1280, 720, 60, 1, false};
  Sender sender("omthx-test-resize", Mode::Hx, Quality::Medium);
  Receiver receiver(sender.address());
  std::vector<uint8_t> bgra;
  VideoFrame frame;
  bool sawSmall = false, sawLarge = false;
  const auto deadline = Clock::now() + std::chrono::seconds(15);
  for (int i = 0; Clock::now() < deadline && !sawLarge; ++i) {
    const VideoFormat& f = sawSmall ? large : small;  // switch as soon as the first size has arrived
    drawPattern(bgra, f.width, f.height, i, false);
    sender.sendVideo(f, bgra, false, 1'000'000 + i * kFrame);
    if (receiver.receiveVideo(frame, 16)) {
      if (frame.format == small) sawSmall = true;
      if (frame.format == large) sawLarge = true;
    }
  }
  CHECK(sawSmall);
  CHECK(sawLarge);
  CHECK(frame.bgra.size() == 1280u * 720 * 4);
}

TEST_CASE("a source that comes back with the other codec is picked up by the same Receiver") {
  if (!hardware(Codec::Hevc) || !hardware(Codec::H264)) return;
  const VideoFormat f{640, 360, 60, 1, false};
  auto sender = std::make_unique<Sender>("omthx-test-switch", Mode::Hx, Quality::Medium, Codec::Hevc);
  Receiver receiver(sender->address());
  VideoFrame frame;
  {
    Pump pump(*sender, f);
    REQUIRE(receiveSome(receiver, frame, 10, 10) == 10);
    CHECK(receiver.stats().codec == Codec::Hevc);
  }
  sender.reset();  // the operator changes the camera's output codec
  std::this_thread::sleep_for(std::chrono::seconds(1));
  sender = std::make_unique<Sender>("omthx-test-switch", Mode::Hx, Quality::Medium, Codec::H264);
  Pump pump(*sender, f);

  // libomt reconnects by itself; on one PC that took about 3 seconds.
  bool switched = false;
  const auto deadline = Clock::now() + std::chrono::seconds(20);
  while (!switched && Clock::now() < deadline) {
    if (receiver.receiveVideo(frame, 100)) switched = receiver.stats().codec == Codec::H264;
  }
  CHECK(switched);
  CHECK(receiver.stats().decoder.rfind("h264", 0) == 0);
  CHECK(frame.format == f);
}

TEST_CASE("probe reads a plain OMT source's full size from its preview") {
  const VideoFormat f{1280, 720, 50, 1, true};
  Sender sender("omthx-test-probe-full", Mode::Full, Quality::Medium);
  Pump pump(sender, f);
  SourceInfo info;
  REQUIRE(probe(sender.address(), 3000, info));
  CHECK(info.mode == Mode::Full);
  CHECK(info.format == f);  // the full size, though only the preview was sent
  CHECK(sender.tally() == Tally{});  // a probe never lights the sender's tally
}

TEST_CASE("probe names an HX source's codec") {
  Codec codec = Codec::Hevc;
  SUBCASE("hevc") { codec = Codec::Hevc; }
  SUBCASE("h264") { codec = Codec::H264; }
  if (!hardware(codec)) return;
  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender(std::string("omthx-test-probe-") + codecName(codec), Mode::Hx, Quality::Medium, codec);
  Pump pump(sender, f);
  SourceInfo info;
  REQUIRE(probe(sender.address(), 3000, info));
  CHECK(info.mode == Mode::Hx);
  CHECK(info.codec == codec);
  CHECK(info.format == f);
}

TEST_CASE("probe gives up on an address with nothing there") {
  SourceInfo info;
  const auto t0 = Clock::now();
  CHECK_FALSE(probe("NOWHERE (omthx-test-absent)", 1000, info));
  CHECK(Clock::now() - t0 < std::chrono::seconds(3));
}

TEST_CASE("Sender rejects input it cannot use") {
  Sender sender("omthx-test-bad", Mode::Hx, Quality::Medium);
  const std::vector<uint8_t> tooSmall(16);
  CHECK_THROWS_AS(sender.sendVideo(VideoFormat{640, 360, 60, 1, false}, tooSmall, false, 0), std::invalid_argument);
  const std::vector<float> three(3);
  CHECK_THROWS_AS(sender.sendAudio(three, 48000, 2, 0), std::invalid_argument);
  CHECK_THROWS_AS(sender.sendAudio(three, 48000, 0, 0), std::invalid_argument);
}
