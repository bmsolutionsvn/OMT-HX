// Sends a moving test picture as an OMT or OMT-HX source and prints what the sender measures.
#include <windows.h>

#include <timeapi.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include "omthx/omthx.h"
#include "pattern.h"

using namespace omthx;

namespace {
[[noreturn]] void usage() {
  std::puts("omthx-send [--name TEXT] [--mode hx|full] [--codec hevc|h264] [--quality low|medium|high]\n"
            "           [--size WxH] [--fps N] [--alpha] [--tone] [--seconds N]\n"
            "Defaults: --name \"OMT-HX Test\" --mode hx --codec hevc --quality medium --size 1920x1080 --fps 60,\n"
            "runs until Ctrl+C. --codec is ignored with --mode full.\n"
            "--tone adds a 1 kHz tone at -20 dBFS, stereo, 48 kHz.\n");
  std::exit(2);
}
}  // namespace

int main(int argc, char** argv) {
  std::string name = "OMT-HX Test";
  Mode mode = Mode::Hx;
  Quality quality = Quality::Medium;
  Codec codec = Codec::Hevc;
  VideoFormat f{1920, 1080, 60, 1, false};
  int seconds = 0;
  bool tone = false;
  std::vector<float> block;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) usage();
      return argv[++i];
    };
    if (a == "--name") {
      name = value();
    } else if (a == "--mode") {
      mode = value() == "full" ? Mode::Full : Mode::Hx;
    } else if (a == "--codec") {
      codec = value() == "h264" ? Codec::H264 : Codec::Hevc;
    } else if (a == "--quality") {
      const std::string q = value();
      quality = q == "low" ? Quality::Low : q == "high" ? Quality::High : Quality::Medium;
    } else if (a == "--size") {
      if (std::sscanf(value().c_str(), "%dx%d", &f.width, &f.height) != 2) usage();
    } else if (a == "--fps") {
      f.fpsN = std::atoi(value().c_str());
    } else if (a == "--alpha") {
      f.alpha = true;
    } else if (a == "--seconds") {
      seconds = std::atoi(value().c_str());
    } else if (a == "--tone") {
      tone = true;
    } else {
      usage();
    }
  }
  if (f.fpsN <= 0) usage();

  try {
    // Sixteen pictures drawn once and looped: drawing 1080p in real time would cost more than sending it.
    std::vector<std::vector<uint8_t>> loop(16);
    for (int i = 0; i < 16; ++i) drawPattern(loop[i], f.width, f.height, i * 4, f.alpha);

    Sender sender(name, mode, quality, codec);
    std::printf("source: %s\n", sender.address().c_str());
    timeBeginPeriod(1);  // 1 ms sleeps instead of 15 ms
    using Clock = std::chrono::steady_clock;
    const int64_t epoch =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count() /
        100;  // wall clock in OMT units, so omthx-recv on this PC can measure delay
    const auto t0 = Clock::now();
    auto report = t0 + std::chrono::seconds(1);
    SenderStats last;
    const int64_t total = static_cast<int64_t>(seconds) * f.fpsN / f.fpsD;
    for (int64_t i = 0; seconds == 0 || i < total; ++i) {
      sender.sendVideo(f, loop[static_cast<size_t>(i % 16)], false, epoch + i * 10'000'000LL * f.fpsD / f.fpsN);
      if (tone) {  // one block per frame, sized so the sample count never drifts from the frame count
        const int64_t first = i * 48000 * f.fpsD / f.fpsN;
        const size_t n = static_cast<size_t>((i + 1) * 48000 * f.fpsD / f.fpsN - first);
        block.resize(2 * n);
        for (size_t k = 0; k < n; ++k)
          block[k] = block[n + k] =
              0.1f * static_cast<float>(std::sin(2 * 3.141592653589793 * 1000 * static_cast<double>(first + static_cast<int64_t>(k)) / 48000));
        sender.sendAudio(block, 48000, 2, epoch + i * 10'000'000LL * f.fpsD / f.fpsN);
      }
      std::this_thread::sleep_until(t0 + std::chrono::nanoseconds((i + 1) * 1'000'000'000LL * f.fpsD / f.fpsN));
      if (Clock::now() >= report) {
        report += std::chrono::seconds(1);
        const SenderStats s = sender.stats();
        const Tally t = sender.tally();
        std::printf("%-11s %3llu fps  %7.2f Mbps  encode %5.2f ms  keyframes %llu  dropped %llu  connections %d  tally %s%s\n",
                    s.encoder.c_str(), static_cast<unsigned long long>(s.framesSent - last.framesSent),
                    (s.bytesSent - last.bytesSent) * 8 / 1e6, s.lastEncodeMs,
                    static_cast<unsigned long long>(s.keyframes), static_cast<unsigned long long>(s.framesDropped),
                    s.connections, t.program ? "PGM " : "", t.preview ? "PVW" : "");
        last = s;
      }
    }
    timeEndPeriod(1);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "omthx-send: %s\n", e.what());
    return 1;
  }
  return 0;
}