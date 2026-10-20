#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
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
  CHECK(coded.stats().decoder == "none");
  VideoFrame nothing;
  CHECK_FALSE(coded.receiveVideo(nothing, 0));  // a Coded receiver decodes nothing
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
