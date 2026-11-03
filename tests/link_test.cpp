#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
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
  CHECK(sender.stats().encoder.ends_with(" + vmx alpha"));  // the GPU codes the fill, VMX the alpha
  CHECK(receiver.stats().decoder.ends_with(" + vmx alpha"));
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

TEST_CASE("an NV12 picture is sent without conversion and arrives as BGRA") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender("omthx-test-nv12", Mode::Hx, Quality::Medium);
  Receiver receiver(sender.address());
  // Limited-range white over the left half, black over the right; rows padded to 704 bytes.
  const int stride = 704;
  std::vector<uint8_t> y(static_cast<size_t>(stride) * 360), uv(static_cast<size_t>(stride) * 180, 128);
  for (int r = 0; r < 360; ++r)
    for (int x = 0; x < 640; ++x) y[static_cast<size_t>(r) * stride + x] = x < 320 ? 235 : 16;
  VideoFrame frame;
  bool got = false;
  auto next = Clock::now();
  for (int i = 0; i < 300 && !got; ++i) {
    sender.sendVideoNv12(f, y.data(), stride, uv.data(), stride, false, 1'000'000 + i * kFrame);
    got = receiver.receiveVideo(frame, 0);
    next += std::chrono::microseconds(16'667);
    std::this_thread::sleep_until(next);
  }
  REQUIRE(got);
  CHECK(frame.format == f);
  const size_t middle = static_cast<size_t>(180) * 640 * 4;
  CHECK(frame.bgra[middle + 100 * 4] > 245);  // white, so the range was stretched
  CHECK(frame.bgra[middle + 540 * 4] < 10);   // black
  CHECK(sender.stats().lastEncodeMs > 0);

  Sender full("omthx-test-nv12-full", Mode::Full, Quality::Medium);
  CHECK_THROWS_AS(full.sendVideoNv12(f, y.data(), stride, uv.data(), stride, true, 0), std::invalid_argument);
  CHECK_THROWS_AS(sender.sendVideoNv12(VideoFormat{640, 360, 60, 1, true}, y.data(), stride, uv.data(), stride,
                                       false, 0),
                  std::invalid_argument);
}

TEST_CASE("an NV12 receiver gets the decoder's picture without a colour conversion") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender("omthx-test-nv12-out", Mode::Hx, Quality::Medium);
  Receiver receiver(sender.address(), PixelFormat::Nv12);
  // Limited-range white over the left half, black over the right; rows padded to 704 bytes.
  const int stride = 704;
  std::vector<uint8_t> y(static_cast<size_t>(stride) * 360), uv(static_cast<size_t>(stride) * 180, 128);
  for (int r = 0; r < 360; ++r)
    for (int x = 0; x < 640; ++x) y[static_cast<size_t>(r) * stride + x] = x < 320 ? 235 : 16;
  VideoFrame frame;
  bool got = false;
  auto next = Clock::now();
  for (int i = 0; i < 300 && !got; ++i) {
    sender.sendVideoNv12(f, y.data(), stride, uv.data(), stride, false, 1'000'000 + i * kFrame);
    got = receiver.receiveVideo(frame, 0);
    next += std::chrono::microseconds(16'667);
    std::this_thread::sleep_until(next);
  }
  REQUIRE(got);
  CHECK(frame.format == f);
  REQUIRE(frame.nv12Y != nullptr);
  REQUIRE(frame.nv12UV != nullptr);
  const size_t middle = static_cast<size_t>(180) * 640;
  CHECK(frame.nv12Y[middle + 100] > 200);  // the white half, still full range
  CHECK(frame.nv12Y[middle + 540] < 40);   // the black half
  CHECK(frame.bgra.empty());               // no BGRA conversion happened
  CHECK(frame.uyvy.empty());
}

TEST_CASE("an NV12 receiver drops a frame whose header lies about its size") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat honest{640, 360, 60, 1, false};
  const VideoFormat lie{1280, 720, 60, 1, false};
  Sender source("omthx-test-nv12-lie-in", Mode::Hx, Quality::Medium, Codec::Hevc);
  Receiver coded(source.address(), PixelFormat::Coded);
  Sender relay("omthx-test-nv12-lie-out", Mode::Hx, Quality::Medium);
  Receiver receiver(relay.address(), PixelFormat::Nv12);
  Pump pump(source, honest);
  CodedFrame c;
  VideoFrame out;
  // Republish the 640x360 stream under a 1280x720 header: every frame must be dropped without reading past
  // the decoder's picture and without an exception. Once the relay's decoder has a keyframe, a true return
  // here means the size check is missing.
  const auto until = Clock::now() + std::chrono::seconds(10);
  while (receiver.stats().framesDiscarded < 10 && Clock::now() < until) {
    if (relay.keyframeRequested()) coded.requestKeyframe();
    if (!coded.receiveCoded(c, 20)) continue;
    CHECK_FALSE(receiver.receiveVideo(out, 0));
    CHECK(out.nv12Y == nullptr);
    c.format = lie;
    relay.sendCoded(c);
  }
  CHECK(receiver.stats().framesDiscarded >= 10);

  // The honest header: the same frames arrive again.
  bool got = false;
  for (const auto end = Clock::now() + std::chrono::seconds(10); !got && Clock::now() < end;) {
    if (relay.keyframeRequested()) coded.requestKeyframe();
    if (coded.receiveCoded(c, 20)) {
      c.format = honest;
      relay.sendCoded(c);
    }
    got = receiver.receiveVideo(out, 0);
  }
  REQUIRE(got);
  CHECK(out.format == honest);
  REQUIRE(out.nv12Y != nullptr);
}

TEST_CASE("an NV12 receiver of a plain OMT source gets full-range NV12 planes") {
  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender("omthx-test-nv12-full-in", Mode::Full, Quality::Medium);
  Receiver receiver(sender.address(), PixelFormat::Nv12);
  // White over the left half, black over the right.
  std::vector<uint8_t> bgra(static_cast<size_t>(640) * 360 * 4);
  for (int r = 0; r < 360; ++r)
    for (int x = 0; x < 640; ++x)
      for (int c = 0; c < 4; ++c) bgra[(static_cast<size_t>(r) * 640 + x) * 4 + c] = x < 320 || c == 3 ? 255 : 0;
  VideoFrame frame;
  bool got = false;
  auto next = Clock::now();
  for (int i = 0; i < 300 && !got; ++i) {
    sender.sendVideo(f, bgra, false, 1'000'000 + i * kFrame);
    got = receiver.receiveVideo(frame, 0);
    next += std::chrono::microseconds(16'667);
    std::this_thread::sleep_until(next);
  }
  REQUIRE(got);
  CHECK(frame.format == f);
  REQUIRE(frame.nv12Y != nullptr);
  REQUIRE(frame.nv12UV != nullptr);
  const size_t middle = static_cast<size_t>(180) * 640;
  CHECK(frame.nv12Y[middle + 100] > 245);  // white, re-ranged to full range as a decoded Hx picture is
  CHECK(frame.nv12Y[middle + 540] < 10);   // black
  CHECK(frame.nv12UV[static_cast<size_t>(90) * 640 + 100] > 118);  // no colour
  CHECK(frame.nv12UV[static_cast<size_t>(90) * 640 + 100] < 138);
  CHECK(receiver.stats().mode == Mode::Full);
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

TEST_CASE("a preview receiver of an HX source gets the small stream, beside a full receiver") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{1280, 720, 60, 1, true};
  Sender sender("omthx-test-preview-hx", Mode::Hx, Quality::Medium);
  Receiver full(sender.address());
  Receiver small(sender.address(), PixelFormat::Bgra, true);
  Pump pump(sender, f);

  VideoFrame a, b;
  REQUIRE(receiveSome(small, b, 30, 10) == 30);
  REQUIRE(receiveSome(full, a, 30, 10) == 30);
  CHECK(a.format == f);
  CHECK(b.format == VideoFormat{640, 360, 60, 1, false});
  CHECK(b.bgra.size() == 640u * 360 * 4);
  CHECK(full.stats().bytesReceived > 2 * small.stats().bytesReceived);
  SourceInfo info;
  REQUIRE(probe(sender.address(), 3000, info));  // a probe asks for preview, and still reads the full size
  CHECK(info.format == f);
}

TEST_CASE("a preview receiver of a plain OMT source gets the VMX preview") {
  const VideoFormat f{1280, 720, 60, 1, false};
  Sender sender("omthx-test-preview-full", Mode::Full, Quality::Medium);
  Receiver small(sender.address(), PixelFormat::Bgra, true);
  Pump pump(sender, f);
  VideoFrame b;
  REQUIRE(receiveSome(small, b, 10, 10) == 10);
  CHECK(b.format.width < f.width);
  CHECK(b.bgra.size() == static_cast<size_t>(b.format.width) * b.format.height * 4);
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

namespace {
// Left half white and opaque, right half black at quarter alpha, laid out as UYVA: the UYVY plane, then the
// alpha plane right after it with half its stride, as OMT and NDI lay it out.
std::vector<uint8_t> uyvaPicture(int w, int h) {
  std::vector<uint8_t> p(static_cast<size_t>(w) * h * 3);
  for (int r = 0; r < h; ++r) {
    for (int x = 0; x < w; ++x) {
      uint8_t* px = &p[(static_cast<size_t>(r) * w + x) * 2];
      px[0] = 128;
      px[1] = x < w / 2 ? 235 : 16;
      p[static_cast<size_t>(w) * h * 2 + static_cast<size_t>(r) * w + x] = x < w / 2 ? 255 : 64;
    }
  }
  return p;
}

bool closeTo(int value, int want) { return std::abs(value - want) <= 8; }

// Sends the UYVA picture at 60 fps until the receiver hands one back, for at most five seconds.
bool sendUyvaUntilReceived(Sender& sender, Receiver& receiver, const VideoFormat& f, VideoFrame& frame) {
  const std::vector<uint8_t> pic = uyvaPicture(f.width, f.height);
  const uint8_t* alpha = pic.data() + static_cast<size_t>(f.width) * f.height * 2;
  auto next = Clock::now();
  for (int i = 0; i < 300; ++i) {
    sender.sendVideoUyvy(f, pic.data(), f.width * 2, alpha, f.width, false, 1'000'000 + i * kFrame);
    if (receiver.receiveVideo(frame, 0)) return true;
    next += std::chrono::microseconds(16'667);
    std::this_thread::sleep_until(next);
  }
  return false;
}

void checkUyva(const VideoFrame& frame, int w, int h) {
  REQUIRE(frame.uyvy.size() == static_cast<size_t>(w) * h * 3);
  const size_t row = static_cast<size_t>(h / 2) * w;
  const uint8_t* alpha = frame.uyvy.data() + static_cast<size_t>(w) * h * 2;
  CHECK(closeTo(frame.uyvy[row * 2 + 100 * 2 + 1], 235));  // white
  CHECK(closeTo(frame.uyvy[row * 2 + 100 * 2], 128));      // neutral colour
  CHECK(closeTo(frame.uyvy[row * 2 + 540 * 2 + 1], 16));   // black
  CHECK(closeTo(alpha[row + 100], 255));
  CHECK(closeTo(alpha[row + 540], 64));
}
}  // namespace

TEST_CASE("a UYVA picture goes through plain OMT and arrives as UYVA") {
  const VideoFormat f{640, 360, 60, 1, true};
  Sender sender("omthx-test-uyvy-full", Mode::Full, Quality::Medium);
  Receiver receiver(sender.address(), PixelFormat::Uyvy);
  VideoFrame frame;
  REQUIRE(sendUyvaUntilReceived(sender, receiver, f, frame));
  CHECK(frame.format == f);
  CHECK_FALSE(frame.premultiplied);  // sent straight, and plain OMT keeps it
  CHECK(frame.bgra.empty());
  checkUyva(frame, 640, 360);
}

TEST_CASE("a UYVA picture goes through HX and arrives as UYVA") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{640, 360, 60, 1, true};
  Sender sender("omthx-test-uyvy-hx", Mode::Hx, Quality::Medium);
  Receiver receiver(sender.address(), PixelFormat::Uyvy);
  VideoFrame frame;
  REQUIRE(sendUyvaUntilReceived(sender, receiver, f, frame));
  CHECK(frame.format == f);
  CHECK(frame.premultiplied);  // HX carries premultiplied colour; black stays black
  checkUyva(frame, 640, 360);
  CHECK(sender.stats().lastEncodeMs > 0);
}

TEST_CASE("sendVideoUyvy rejects input it cannot use") {
  Sender sender("omthx-test-uyvy-bad", Mode::Hx, Quality::Medium);
  const std::vector<uint8_t> pic(640u * 360 * 3);
  CHECK_THROWS_AS(sender.sendVideoUyvy(VideoFormat{639, 360, 60, 1, false}, pic.data(), 1280, nullptr, 0, false, 0),
                  std::invalid_argument);  // odd width
  CHECK_THROWS_AS(sender.sendVideoUyvy(VideoFormat{640, 360, 60, 1, false}, pic.data(), 1000, nullptr, 0, false, 0),
                  std::invalid_argument);  // stride under width * 2
  CHECK_THROWS_AS(sender.sendVideoUyvy(VideoFormat{640, 360, 60, 1, true}, pic.data(), 1280, nullptr, 0, false, 0),
                  std::invalid_argument);  // alpha without its plane
}

namespace {
// Pumps `from` into `relay` (one call per round) and reads `downstream` until it has handed back `wanted`
// pictures or ten seconds pass.
template <typename Relay>
int relayed(Relay relay, Receiver& downstream, VideoFrame& out, int wanted) {
  int got = 0;
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  while (got < wanted && Clock::now() < deadline) {
    relay();
    if (downstream.receiveVideo(out, 0)) ++got;
  }
  return got;
}
}  // namespace

TEST_CASE("an HX source re-coded through a Gpu receiver stays in video memory") {
  if (!hardware(Codec::Hevc) || !hardware(Codec::H264)) return;
  Codec to = Codec::H264;
  SUBCASE("to h264") { to = Codec::H264; }
  SUBCASE("to hevc") { to = Codec::Hevc; }
  const VideoFormat f{640, 360, 60, 1, true};
  Sender source("omthx-test-gpu-in", Mode::Hx, Quality::Medium, Codec::Hevc);
  Receiver gpu(source.address(), PixelFormat::Gpu);
  Sender recoded("omthx-test-gpu-out", Mode::Hx, Quality::Medium, to);
  Receiver downstream(recoded.address(), PixelFormat::Uyvy);
  Pump pump(source, f);
  VideoFrame frame, out;
  const int got = relayed(
      [&] {
        if (!gpu.receiveVideo(frame, 20)) return;
        REQUIRE(frame.gpu);
        recoded.sendVideo(frame);
      },
      downstream, out, 10);
  REQUIRE(got == 10);
  CHECK(out.format == f);
  CHECK(out.premultiplied);
  CHECK(out.uyvy.size() == 640u * 360 * 3);
  CHECK(downstream.stats().codec == to);
  MESSAGE("re-coded by " << recoded.stats().encoder);
#ifdef _WIN32
  CHECK(recoded.stats().encoder.find("(d3d11)") != std::string::npos);  // no copy through memory
#endif
  CHECK(recoded.stats().encoder.ends_with(" + vmx alpha"));              // alpha re-coded by VMX
  REQUIRE(frame.gpu);
  CHECK(download(frame, PixelFormat::Uyvy));
  CHECK(frame.uyvy.size() == 640u * 360 * 3);
  CHECK(download(frame, PixelFormat::Bgra));
  CHECK(frame.bgra.size() == 640u * 360 * 4);
}

TEST_CASE("a Gpu receiver of a plain OMT source hands back UYVY, which a Full sender takes") {
  const VideoFormat f{640, 360, 60, 1, false};
  Sender source("omthx-test-gpu-full-in", Mode::Full, Quality::Medium);
  Receiver gpu(source.address(), PixelFormat::Gpu);
  Sender out("omthx-test-gpu-full-out", Mode::Full, Quality::Medium);
  Receiver downstream(out.address());
  Pump pump(source, f);
  VideoFrame frame, back;
  const int got = relayed(
      [&] {
        if (!gpu.receiveVideo(frame, 20)) return;
        CHECK_FALSE(frame.gpu);
        CHECK(frame.uyvy.size() == 640u * 360 * 2);
        out.sendVideo(frame);
      },
      downstream, back, 5);
  CHECK(got == 5);
  CHECK(back.format == f);
}

TEST_CASE("an HX relay passes coded frames on, and keyframe requests back") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{640, 360, 60, 1, false};
  Sender source("omthx-test-relay-in", Mode::Hx, Quality::Medium, Codec::Hevc);
  Receiver coded(source.address(), PixelFormat::Coded);
  Sender relay("omthx-test-relay-out", Mode::Hx, Quality::Medium);
  Pump pump(source, f);
  CodedFrame c;
  // Let the relay run before anyone watches it: the late receiver must get its keyframe by asking.
  for (const auto until = Clock::now() + std::chrono::seconds(1); Clock::now() < until;) {
    if (coded.receiveCoded(c, 20)) relay.sendCoded(c);
  }
  Receiver downstream(relay.address());
  VideoFrame out;
  uint64_t forwarded = 0;
  const int got = relayed(
      [&] {
        if (relay.keyframeRequested()) {
          ++forwarded;
          coded.requestKeyframe();
        }
        if (coded.receiveCoded(c, 20)) {
          CHECK(c.mode == Mode::Hx);
          relay.sendCoded(c);
        }
      },
      downstream, out, 10);
  CHECK(got == 10);
  CHECK(out.format == f);
  CHECK(forwarded >= 1);  // the late receiver asked, and the relay passed it on
  CHECK(relay.stats().encoder == "none");
  CHECK_FALSE(c.encoder.empty());                         // the source said what codes it
  CHECK(downstream.stats().encoder == c.encoder);         // and the relay passed that on, not "none"
  CHECK(coded.stats().decoder == "none");
  VideoFrame nothing;
  CHECK_FALSE(coded.receiveVideo(nothing, 0));  // a Coded receiver decodes nothing
}

TEST_CASE("a coded stream that starts mid-GOP shows nothing until its next keyframe") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{640, 360, 60, 1, false};
  // Record about 1.5 s of a source, with a second keyframe asked for part way through.
  std::vector<CodedFrame> rec;
  {
    Sender source("omthx-test-midgop-in", Mode::Hx, Quality::Medium, Codec::Hevc);
    Receiver coded(source.address(), PixelFormat::Coded);
    Pump pump(source, f);
    const auto start = Clock::now();
    bool asked = false;
    for (CodedFrame c; Clock::now() - start < std::chrono::milliseconds(1500);) {
      if (!asked && Clock::now() - start > std::chrono::milliseconds(800)) {
        source.requestKeyframe();
        asked = true;
      }
      if (coded.receiveCoded(c, 20)) rec.push_back(c);
    }
  }
  size_t key = 0;  // the second keyframe
  for (size_t i = 1; i < rec.size() && !key; ++i)
    if (rec[i].keyframe) key = i;
  REQUIRE(key >= 20);

  // Replay from 20 frames before it, to a receiver that is already watching.
  Sender relay("omthx-test-midgop-out", Mode::Hx, Quality::Medium);
  Receiver downstream(relay.address());
  for (const auto until = Clock::now() + std::chrono::seconds(5);
       relay.stats().connections == 0 && Clock::now() < until;)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::thread replay([&] {
    auto next = Clock::now();
    for (size_t i = key - 20; i < rec.size(); ++i) {
      relay.sendCoded(rec[i]);
      next += std::chrono::microseconds(16'667);
      std::this_thread::sleep_until(next);
    }
  });
  VideoFrame out;
  int64_t first = -1;
  for (const auto until = Clock::now() + std::chrono::seconds(5); first < 0 && Clock::now() < until;) {
    if (downstream.receiveVideo(out, 50)) first = out.timestamp;
  }
  replay.join();
  // The first picture is the keyframe or a later one: the 20 frames before it were discarded, not decoded. It
  // may be later because frames that queue while the decoder opens are decoded but not brought to memory: the
  // receiver shows the newest. Later frames are not counted: a frame lost while the decoder opens makes the
  // receiver ask for a keyframe, which a live source answers and this replay cannot.
  CHECK(first >= rec[key].timestamp);
  CHECK(downstream.stats().framesDiscarded >= 20);
  CHECK(out.format == f);
}

TEST_CASE("an HX relay passes a source's VMX alpha on with its fill") {
  if (!hardware(Codec::H264)) return;
  const VideoFormat f{640, 360, 60, 1, true};
  Sender source("omthx-test-relay-alpha-in", Mode::Hx, Quality::Medium, Codec::H264);
  Receiver coded(source.address(), PixelFormat::Coded);
  Sender relay("omthx-test-relay-alpha-out", Mode::Hx, Quality::Medium);
  Receiver downstream(relay.address());
  Pump pump(source, f);
  CodedFrame c;
  VideoFrame out;
  const int got = relayed(
      [&] {
        if (relay.keyframeRequested()) coded.requestKeyframe();
        if (!coded.receiveCoded(c, 20)) return;
        CHECK((c.alphaBytes > 0) != c.alphaRepeats);  // alpha, or the previous frame's again
        if (c.keyframe) CHECK(c.alphaBytes > 0);       // a keyframe always carries its own
        CHECK(c.alphaBytes < c.data.size());
        relay.sendCoded(c);
      },
      downstream, out, 10);
  CHECK(got == 10);
  CHECK(out.format == f);
  CHECK(out.premultiplied);
  CHECK(downstream.stats().decoder.ends_with(" + vmx alpha"));
}

TEST_CASE("a plain OMT relay passes VMX frames on without decoding them") {
  const VideoFormat f{640, 360, 60, 1, true};
  Sender source("omthx-test-relay-vmx-in", Mode::Full, Quality::Medium);
  Receiver coded(source.address(), PixelFormat::Coded);
  Sender relay("omthx-test-relay-vmx-out", Mode::Full, Quality::Medium);
  Receiver downstream(relay.address(), PixelFormat::Uyvy);
  Pump pump(source, f);
  CodedFrame c;
  VideoFrame out;
  const int got = relayed(
      [&] {
        if (!coded.receiveCoded(c, 20)) return;
        CHECK(c.mode == Mode::Full);
        relay.sendCoded(c);
      },
      downstream, out, 5);
  CHECK(got == 5);
  CHECK(out.format == f);
  CHECK(out.uyvy.size() == 640u * 360 * 3);
  CHECK_FALSE(out.premultiplied);  // the pump's straight alpha, flags passed on untouched
}

TEST_CASE("a limited-range NV12 picture goes to VMX as it is") {
  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender("omthx-test-nv12-vmx", Mode::Full, Quality::Medium);
  Receiver receiver(sender.address());
  // White over the left half, black over the right; rows padded to 704 bytes, chroma in its own buffer.
  const int stride = 704;
  std::vector<uint8_t> y(static_cast<size_t>(stride) * 360), uv(static_cast<size_t>(stride) * 180, 128);
  for (int r = 0; r < 360; ++r)
    for (int x = 0; x < 640; ++x) y[static_cast<size_t>(r) * stride + x] = x < 320 ? 235 : 16;
  VideoFrame frame;
  bool got = false;
  auto next = Clock::now();
  for (int i = 0; i < 300 && !got; ++i) {
    sender.sendVideoNv12(f, y.data(), stride, uv.data(), stride, false, 1'000'000 + i * kFrame);
    got = receiver.receiveVideo(frame, 0);
    next += std::chrono::microseconds(16'667);
    std::this_thread::sleep_until(next);
  }
  REQUIRE(got);
  CHECK(frame.format == f);
  const size_t middle = static_cast<size_t>(180) * 640 * 4;
  CHECK(frame.bgra[middle + 100 * 4] > 245);  // white
  CHECK(frame.bgra[middle + 540 * 4] < 10);   // black
  CHECK(receiver.stats().mode == Mode::Full);
}

#ifdef _WIN32
#include <d3d11.h>
#include <wrl/client.h>

TEST_CASE("a BGRA texture is converted and encoded in video memory") {
  if (!hardware(Codec::Hevc)) return;
  // A device as windows-capture makes one: BGRA support, no video-support flag, 11.1 first.
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                      levels, 2, D3D11_SDK_VERSION, &device, nullptr, nullptr)));
  // The texture is larger than the picture: white left half, red right half, green outside the picture.
  const int tw = 648, th = 364;
  std::vector<uint8_t> bgra(static_cast<size_t>(tw) * th * 4);
  for (int r = 0; r < th; ++r)
    for (int x = 0; x < tw; ++x) {
      uint8_t* p = &bgra[(static_cast<size_t>(r) * tw + x) * 4];
      const bool outside = x >= 640 || r >= 360;
      p[0] = outside ? 0 : x < 320 ? 255 : 0;
      p[1] = outside ? 255 : x < 320 ? 255 : 0;
      p[2] = outside ? 0 : 255;
      p[3] = 255;
    }
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = tw;
  desc.Height = th;
  desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  const D3D11_SUBRESOURCE_DATA init{bgra.data(), static_cast<UINT>(tw * 4), 0};
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, &init, &texture)));

  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender("omthx-test-d3d11", Mode::Hx, Quality::Medium);
  Receiver receiver(sender.address());
  VideoFrame frame;
  bool got = false;
  auto next = Clock::now();
  for (int i = 0; i < 300 && !got; ++i) {
    sender.sendVideoD3D11(f, texture.Get(), 1'000'000 + i * kFrame);
    got = receiver.receiveVideo(frame, 0);
    next += std::chrono::microseconds(16'667);
    std::this_thread::sleep_until(next);
  }
  REQUIRE(got);
  CHECK(frame.format == f);
  MESSAGE("encoded by " << sender.stats().encoder);
  CHECK(sender.stats().encoder.find("(d3d11)") != std::string::npos);  // no copy through memory
  const uint8_t* white = &frame.bgra[(static_cast<size_t>(180) * 640 + 100) * 4];
  const uint8_t* red = &frame.bgra[(static_cast<size_t>(180) * 640 + 636) * 4];
  CHECK(white[0] > 240);
  CHECK(white[1] > 240);
  CHECK(white[2] > 240);
  CHECK(red[0] < 20);  // red, so BT.709 full range on both sides, and no green from outside the picture
  CHECK(red[1] < 20);
  CHECK(red[2] > 235);

  CHECK_THROWS_AS(sender.sendVideoD3D11(VideoFormat{640, 360, 60, 1, true}, texture.Get(), 0), std::invalid_argument);
  CHECK_THROWS_AS(sender.sendVideoD3D11(VideoFormat{700, 360, 60, 1, false}, texture.Get(), 0), std::invalid_argument);
}

TEST_CASE("sendVideoD3D11 takes an RGBA texture") {
  if (!hardware(Codec::Hevc)) return;
  // A device as windows-capture makes one: BGRA support, no video-support flag, 11.1 first.
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                      levels, 2, D3D11_SDK_VERSION, &device, nullptr, nullptr)));
  // The texture is larger than the picture: white left half, red right half, green outside the picture.
  const int tw = 648, th = 364;
  std::vector<uint8_t> rgba(static_cast<size_t>(tw) * th * 4);
  for (int r = 0; r < th; ++r)
    for (int x = 0; x < tw; ++x) {
      uint8_t* p = &rgba[(static_cast<size_t>(r) * tw + x) * 4];
      const bool outside = x >= 640 || r >= 360;
      p[0] = outside ? 0 : 255;
      p[1] = outside ? 255 : x < 320 ? 255 : 0;
      p[2] = outside ? 0 : x < 320 ? 255 : 0;
      p[3] = 255;
    }
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = tw;
  desc.Height = th;
  desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  const D3D11_SUBRESOURCE_DATA init{rgba.data(), static_cast<UINT>(tw * 4), 0};
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, &init, &texture)));

  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender("omthx-test-d3d11-rgba", Mode::Hx, Quality::Medium);
  Receiver receiver(sender.address());
  VideoFrame frame;
  bool got = false;
  auto next = Clock::now();
  for (int i = 0; i < 300 && !got; ++i) {
    sender.sendVideoD3D11(f, texture.Get(), 1'000'000 + i * kFrame);
    got = receiver.receiveVideo(frame, 0);
    next += std::chrono::microseconds(16'667);
    std::this_thread::sleep_until(next);
  }
  REQUIRE(got);
  CHECK(frame.format == f);
  MESSAGE("encoded by " << sender.stats().encoder);
  CHECK(sender.stats().encoder.find("(d3d11)") != std::string::npos);  // no copy through memory
  const uint8_t* white = &frame.bgra[(static_cast<size_t>(180) * 640 + 100) * 4];
  const uint8_t* red = &frame.bgra[(static_cast<size_t>(180) * 640 + 636) * 4];
  CHECK(white[0] > 240);
  CHECK(white[1] > 240);
  CHECK(white[2] > 240);
  CHECK(red[0] < 20);  // red, so BT.709 full range on both sides, and no green from outside the picture
  CHECK(red[1] < 20);
  CHECK(red[2] > 235);
}

namespace {
// A still UYVY picture: neutral chroma, luma 126 (limited range; 128 once expanded to full).
std::vector<uint8_t> uyvyGrey(int w, int h) {
  std::vector<uint8_t> p(static_cast<size_t>(w) * h * 2);
  for (size_t i = 0; i < p.size(); i += 2) {
    p[i] = 128;
    p[i + 1] = 126;
  }
  return p;
}

// Pumps the picture at 60 fps until the receiver hands one back or the frames run out, and returns whether it did.
bool sendUyvyUntilReceived(Sender& sender, Receiver& receiver, const VideoFormat& f, const std::vector<uint8_t>& pic,
                           VideoFrame& frame, int frames = 300) {
  auto next = Clock::now();
  for (int i = 0; i < frames; ++i) {
    sender.sendVideoUyvy(f, pic.data(), f.width * 2, nullptr, 0, false, 1'000'000 + i * kFrame);
    if (receiver.receiveVideo(frame, 0)) return true;
    next += std::chrono::microseconds(16'667);
    std::this_thread::sleep_until(next);
  }
  return false;
}
}  // namespace

TEST_CASE("sendVideoUyvy on an HX sender encodes from video memory") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{1280, 720, 60, 1, false};
  const std::vector<uint8_t> pic = uyvyGrey(f.width, f.height);
  Sender sender("omthx-test-uyvy-gpu", Mode::Hx, Quality::Medium);
  Receiver receiver(sender.address(), PixelFormat::Nv12);
  VideoFrame frame;
  bool got = false;
  auto next = Clock::now();
  const auto until = Clock::now() + std::chrono::seconds(1);
  for (int i = 0; Clock::now() < until; ++i) {
    sender.sendVideoUyvy(f, pic.data(), f.width * 2, nullptr, 0, false, 1'000'000 + i * kFrame);
    if (receiver.receiveVideo(frame, 0)) got = true;
    next += std::chrono::microseconds(16'667);
    std::this_thread::sleep_until(next);
  }
  REQUIRE(got);
  MESSAGE("encoded by " << sender.stats().encoder);
  CHECK(sender.stats().encoder.ends_with(" (d3d11)"));  // no copy through memory
  REQUIRE(frame.nv12Y != nullptr);
  const int y = frame.nv12Y[static_cast<size_t>(f.height / 2) * f.width + f.width / 2];
  MESSAGE("centre luma " << y << " for a limited-range 126");
  CHECK(std::abs(y - 128) <= 4);
}

TEST_CASE("sendVideoUyvy keeps working after a size change") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat fmtSmall{640, 360, 60, 1, false}, fmtBig{1280, 720, 60, 1, false};
  Sender sender("omthx-test-uyvy-resize", Mode::Hx, Quality::Medium);
  Receiver receiver(sender.address(), PixelFormat::Nv12);
  VideoFrame frame;
  bool sawSmall = false, sawLarge = false;
  const auto deadline = Clock::now() + std::chrono::seconds(15);
  for (int i = 0; Clock::now() < deadline && !sawLarge; ++i) {
    const VideoFormat& f = sawSmall ? fmtBig : fmtSmall;
    const std::vector<uint8_t> pic = uyvyGrey(f.width, f.height);
    sender.sendVideoUyvy(f, pic.data(), f.width * 2, nullptr, 0, false, 1'000'000 + i * kFrame);
    if (receiver.receiveVideo(frame, 16)) {
      if (frame.format == fmtSmall) sawSmall = true;
      if (frame.format == fmtBig) sawLarge = true;
    }
  }
  CHECK(sawSmall);
  CHECK(sawLarge);
}

TEST_CASE("OMTHX_NO_GPU_UYVY keeps the CPU path") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{640, 360, 60, 1, false};
  const std::vector<uint8_t> pic = uyvyGrey(f.width, f.height);
  _putenv_s("OMTHX_NO_GPU_UYVY", "1");
  Sender sender("omthx-test-uyvy-switch", Mode::Hx, Quality::Medium);
  Receiver receiver(sender.address(), PixelFormat::Nv12);
  VideoFrame frame;
  const bool got = sendUyvyUntilReceived(sender, receiver, f, pic, frame);
  _putenv_s("OMTHX_NO_GPU_UYVY", "");
  REQUIRE(got);
  MESSAGE("encoded by " << sender.stats().encoder);
  CHECK(sender.stats().encoder.find("(d3d11)") == std::string::npos);
  CHECK(sender.stats().encoder.rfind(codecName(Codec::Hevc), 0) == 0);
}

TEST_CASE("sendVideoUyvy on the GPU costs less CPU than the pack") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{1920, 1080, 60, 1, false};
  const std::vector<uint8_t> pic = uyvyGrey(f.width, f.height);
  const auto cpuSeconds = [&](bool gpu) {
    _putenv_s("OMTHX_NO_GPU_UYVY", gpu ? "" : "1");
    Sender sender(gpu ? "omthx-test-uyvy-cost-gpu" : "omthx-test-uyvy-cost-pack", Mode::Hx, Quality::Medium);
    Receiver receiver(sender.address(), PixelFormat::Nv12);
    VideoFrame frame;
    FILETIME idle{}, kernel{}, user{}, exit{};
    auto time = [](const FILETIME& k, const FILETIME& u) {
      return (static_cast<uint64_t>(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
             (static_cast<uint64_t>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
    };
    GetProcessTimes(GetCurrentProcess(), &idle, &kernel, &user, &exit);
    const uint64_t before = time(kernel, user);
    auto next = Clock::now();
    for (int i = 0; i < 300; ++i) {
      sender.sendVideoUyvy(f, pic.data(), f.width * 2, nullptr, 0, false, 1'000'000 + i * kFrame);
      receiver.receiveVideo(frame, 0);
      next += std::chrono::microseconds(16'667);
      std::this_thread::sleep_until(next);
    }
    GetProcessTimes(GetCurrentProcess(), &idle, &kernel, &user, &exit);
    _putenv_s("OMTHX_NO_GPU_UYVY", "");
    return static_cast<double>(time(kernel, user) - before) / 10'000'000.0;  // FILETIME: 100 ns
  };
  const double gpu = cpuSeconds(true), pack = cpuSeconds(false);
  MESSAGE("CPU seconds for 300 pictures at 1920x1080: GPU path " << gpu << ", pack " << pack);
  CHECK(gpu < pack);
}
#endif

TEST_CASE("an HX sender with no receiver codes nothing") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender("omthx-test-idle-hx", Mode::Hx, Quality::Medium, Codec::Hevc);
  std::vector<uint8_t> bgra;
  for (int i = 0; i < 120; ++i) {
    drawPattern(bgra, f.width, f.height, i, false);
    sender.sendVideo(f, bgra, false, 1'000'000 + i * kFrame);
  }
  const SenderStats s = sender.stats();
  CHECK(s.framesIdle == 120);
  CHECK(s.framesSent == 0);
  CHECK(s.encoder.empty());  // no encoder was opened
  CHECK_FALSE(sender.hasReceivers());

  Receiver receiver(sender.address());
  Pump pump(sender, f);
  VideoFrame frame;
  REQUIRE(receiveSome(receiver, frame, 1, 3) == 1);
  CHECK(sender.hasReceivers());
}

TEST_CASE("a receiver that comes back to a warm HX sender gets a keyframe at once") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender("omthx-test-idle-warm", Mode::Hx, Quality::Medium, Codec::Hevc);
  Pump pump(sender, f);
  {
    Receiver first(sender.address());
    VideoFrame frame;
    REQUIRE(receiveSome(first, frame, 1, 3) == 1);
  }
  const uint64_t idleAfter = sender.stats().framesIdle;
  for (const auto until = Clock::now() + std::chrono::seconds(2);
       Clock::now() < until && (sender.hasReceivers() || sender.stats().framesIdle == idleAfter);)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE_FALSE(sender.hasReceivers());
  REQUIRE(sender.stats().framesIdle > idleAfter);

  const uint64_t before = sender.stats().keyframes;
  const auto t0 = Clock::now();
  Receiver second(sender.address());
  VideoFrame frame;
  REQUIRE(receiveSome(second, frame, 1, 2) == 1);
  const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  MESSAGE("picture " << waited << " ms after the receiver came back");
  CHECK(waited < 1000);
  CHECK(sender.stats().keyframes > before);
}

TEST_CASE("probe reads an idle HX source") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender("omthx-test-idle-probe", Mode::Hx, Quality::Medium, Codec::Hevc);
  Pump pump(sender, f);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  REQUIRE(sender.stats().framesIdle > 0);  // nothing codes until the probe connects
  SourceInfo info;
  REQUIRE(probe(sender.address(), 3000, info));
  CHECK(info.format == f);
}

TEST_CASE("a plain OMT sender with no receiver hands libomt nothing") {
  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender("omthx-test-idle-full", Mode::Full, Quality::Medium);
  std::vector<uint8_t> bgra;
  for (int i = 0; i < 120; ++i) {
    drawPattern(bgra, f.width, f.height, i, false);
    sender.sendVideo(f, bgra, false, 1'000'000 + i * kFrame);
  }
  CHECK(sender.stats().framesIdle == 120);
  CHECK(sender.stats().framesSent == 0);
  Receiver receiver(sender.address());
  Pump pump(sender, f);
  VideoFrame frame;
  REQUIRE(receiveSome(receiver, frame, 1, 3) == 1);
  CHECK(frame.format == f);
}

TEST_CASE("a plain OMT relay with no receiver sends nothing, and asks for a keyframe when one joins") {
  const VideoFormat f{640, 360, 60, 1, false};
  Sender source("omthx-test-idle-relay-in", Mode::Full, Quality::Medium);
  Receiver coded(source.address(), PixelFormat::Coded);
  Pump pump(source, f);
  CodedFrame c;
  const auto until = Clock::now() + std::chrono::seconds(5);
  while (!coded.receiveCoded(c, 100) && Clock::now() < until) {
  }
  REQUIRE_FALSE(c.data.empty());

  Sender relay("omthx-test-idle-relay-out", Mode::Full, Quality::Medium);
  for (int i = 0; i < 10; ++i) relay.sendCoded(c);
  CHECK(relay.stats().framesIdle == 10);
  CHECK(relay.stats().framesSent == 0);
  CHECK_FALSE(relay.keyframeRequested());

  Receiver downstream(relay.address(), PixelFormat::Coded);
  bool requested = false;
  for (const auto deadline = Clock::now() + std::chrono::seconds(2);
       !requested && Clock::now() < deadline;) {
    relay.sendCoded(c);
    requested = relay.keyframeRequested();
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  CHECK(requested);
  CHECK(relay.stats().framesSent > 0);
}

TEST_CASE("FrameDecoder decodes an HX source's coded frames like a Uyvy receiver") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{640, 360, 60, 1, true};
  Sender sender("omthx-test-framedecoder-hx", Mode::Hx, Quality::Medium, Codec::Hevc);
  Receiver coded(sender.address(), PixelFormat::Coded);
  Receiver uyvy(sender.address(), PixelFormat::Uyvy);
  Pump pump(sender, f);

  FrameDecoder decoder;
  CodedFrame c;
  VideoFrame out;
  std::map<int64_t, uint8_t> centre;  // the Uyvy receiver's Y at the centre, by timestamp
  int pictures = 0, compared = 0;
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  while (pictures < 30 && Clock::now() < deadline) {
    VideoFrame u;
    while (uyvy.receiveVideo(u, 0)) centre[u.timestamp] = u.uyvy[(180u * 640 + 320) * 2 + 1];
    if (!coded.receiveCoded(c, 20)) continue;
    if (!decoder.decode(c, out)) continue;
    ++pictures;
    CHECK(out.format == f);
    CHECK(out.uyvy.size() == 640u * 360 * 3);
    const auto it = centre.find(out.timestamp);
    if (it != centre.end()) {
      CHECK(std::abs(static_cast<int>(out.uyvy[(180u * 640 + 320) * 2 + 1]) - it->second) <= 4);
      ++compared;
    }
  }
  CHECK(pictures >= 25);
  CHECK(compared > 0);
  CHECK_FALSE(decoder.name().empty());
}

TEST_CASE("FrameDecoder(Nv12) hands back the decoder's planes") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender("omthx-test-framedecoder-nv12", Mode::Hx, Quality::Medium, Codec::Hevc);
  Receiver coded(sender.address(), PixelFormat::Coded);
  Receiver nv12(sender.address(), PixelFormat::Nv12);
  Pump pump(sender, f);

  FrameDecoder decoder(PixelFormat::Nv12);
  CodedFrame c;
  VideoFrame out;
  std::map<int64_t, uint8_t> centre;  // the Nv12 receiver's Y at the centre, by timestamp
  int pictures = 0, compared = 0;
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  while (pictures < 30 && Clock::now() < deadline) {
    VideoFrame r;
    while (nv12.receiveVideo(r, 0)) centre[r.timestamp] = r.nv12Y[180u * 640 + 320];
    if (!coded.receiveCoded(c, 20)) continue;
    if (!decoder.decode(c, out)) continue;
    ++pictures;
    REQUIRE(out.nv12Y != nullptr);
    REQUIRE(out.nv12UV != nullptr);
    CHECK(out.format == f);
    const auto it = centre.find(out.timestamp);
    if (it != centre.end()) {
      CHECK(std::abs(static_cast<int>(out.nv12Y[180u * 640 + 320]) - it->second) <= 4);
      ++compared;
    }
  }
  CHECK(pictures >= 25);
  CHECK(compared > 0);
}

TEST_CASE("FrameDecoder(Nv12) packs a VMX frame") {
  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender("omthx-test-framedecoder-vmx-nv12", Mode::Full, Quality::Medium);
  Receiver coded(sender.address(), PixelFormat::Coded);
  Pump pump(sender, f);

  FrameDecoder decoder(PixelFormat::Nv12);
  CodedFrame c;
  VideoFrame out;
  bool picture = false;
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  while (!picture && Clock::now() < deadline) {
    if (coded.receiveCoded(c, 50)) picture = decoder.decode(c, out);
  }
  REQUIRE(picture);
  CHECK(out.nv12Y != nullptr);
  CHECK(out.nv12UV != nullptr);
  CHECK(out.format.width == f.width);
  CHECK(out.format.height == f.height);
  CHECK_FALSE(out.format.alpha);
  CHECK(decoder.name() == "vmx");
}

TEST_CASE("FrameDecoder decodes plain OMT VMX with alpha") {
  const VideoFormat f{640, 360, 60, 1, true};
  Sender sender("omthx-test-framedecoder-vmx", Mode::Full, Quality::Medium);
  Receiver coded(sender.address(), PixelFormat::Coded);
  Pump pump(sender, f);

  FrameDecoder decoder;
  CodedFrame c;
  VideoFrame out;
  bool picture = false;
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  while (!picture && Clock::now() < deadline) {
    if (coded.receiveCoded(c, 50)) picture = decoder.decode(c, out);
  }
  REQUIRE(picture);
  CHECK(out.format == f);
  REQUIRE(out.uyvy.size() == 640u * 360 * 3);
  size_t opaque = 0;
  for (size_t i = 640u * 360 * 2; i < out.uyvy.size(); ++i) opaque += out.uyvy[i] > 0;
  CHECK(opaque > 0);  // the alpha plane, not all zero
  CHECK(decoder.name() == "vmx");
}

TEST_CASE("FrameDecoder starts at a keyframe after reset") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender("omthx-test-framedecoder-reset", Mode::Hx, Quality::Medium, Codec::Hevc);
  Receiver coded(sender.address(), PixelFormat::Coded);
  Pump pump(sender, f);

  FrameDecoder decoder;
  CodedFrame c;
  VideoFrame out;
  bool picture = false;
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  while (!picture && Clock::now() < deadline) {
    if (coded.receiveCoded(c, 50)) picture = decoder.decode(c, out);
  }
  REQUIRE(picture);

  decoder.reset();
  int nonKey = 0;  // only non-key frames after the reset: no picture
  for (const auto until = Clock::now() + std::chrono::seconds(2); nonKey < 5 && Clock::now() < until;) {
    if (!coded.receiveCoded(c, 50) || c.keyframe) continue;
    ++nonKey;
    CHECK_FALSE(decoder.decode(c, out));
  }
  CHECK(nonKey == 5);

  coded.requestKeyframe();
  picture = false;
  int afterKey = -1;  // frames fed since the keyframe
  for (const auto until = Clock::now() + std::chrono::seconds(5); !picture && Clock::now() < until;) {
    if (afterKey >= 2) break;
    if (!coded.receiveCoded(c, 50)) continue;
    picture = decoder.decode(c, out);
    if (c.keyframe) afterKey = 0;
    else if (afterKey >= 0) ++afterKey;
  }
  CHECK(picture);
  CHECK(out.format == f);
}

TEST_CASE("FrameDecoder rejects pixel formats it does not offer") {
  CHECK_THROWS_AS((void)FrameDecoder(PixelFormat::Gpu), std::invalid_argument);
}

TEST_CASE("FrameDecoder restarts at a keyframe after a frame it cannot split") {
  if (!hardware(Codec::Hevc)) return;
  const VideoFormat f{640, 360, 60, 1, false};
  Sender sender("omthx-test-framedecoder-bad", Mode::Hx, Quality::Medium, Codec::Hevc);
  Receiver coded(sender.address(), PixelFormat::Coded);
  Pump pump(sender, f);

  FrameDecoder decoder;
  CodedFrame c;
  VideoFrame out;
  bool picture = false;
  for (const auto deadline = Clock::now() + std::chrono::seconds(10); !picture && Clock::now() < deadline;) {
    if (coded.receiveCoded(c, 50)) picture = decoder.decode(c, out);
  }
  REQUIRE(picture);

  // A frame whose alpha leaves no room for the fill is a broken stream, not just a bad frame.
  bool fed = false;
  for (const auto deadline = Clock::now() + std::chrono::seconds(2); !fed && Clock::now() < deadline;) {
    if (!coded.receiveCoded(c, 50) || c.keyframe) continue;
    CodedFrame bad = c;
    bad.alphaBytes = bad.data.size();
    CHECK_FALSE(decoder.decode(bad, out));
    fed = true;
  }
  REQUIRE(fed);

  int nonKey = 0;  // the stream is broken: nothing decodes until a keyframe
  for (const auto until = Clock::now() + std::chrono::seconds(2); nonKey < 5 && Clock::now() < until;) {
    if (!coded.receiveCoded(c, 50) || c.keyframe) continue;
    ++nonKey;
    CHECK_FALSE(decoder.decode(c, out));
  }
  CHECK(nonKey == 5);
}
