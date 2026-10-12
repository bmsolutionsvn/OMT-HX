// Connects to an OMT or OMT-HX source and prints what the receiver measures.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <string>
#include <thread>

#include "omthx/omthx.h"

using namespace omthx;

namespace {
[[noreturn]] void usage() {
  std::puts("omthx-recv --list\n"
            "omthx-recv --source \"HOST (Name)\" [--seconds N] [--dump FILE.bmp] [--tally program|preview]\n"
            "Delay is only meaningful when omthx-send runs on this PC, or both PCs share a synchronised clock.");
  std::exit(2);
}

// 32-bit BMP, top row first. Enough to look at one frame. False if the file could not be written.
bool writeBmp(const std::string& path, const VideoFrame& v) {
  const uint32_t size = static_cast<uint32_t>(v.bgra.size());
  unsigned char h[54] = {'B', 'M'};
  auto put = [&](int at, uint32_t x) { std::memcpy(h + at, &x, 4); };
  put(2, 54 + size);
  put(10, 54);
  put(14, 40);
  put(18, static_cast<uint32_t>(v.format.width));
  put(22, static_cast<uint32_t>(-v.format.height));
  h[26] = 1;
  h[28] = 32;
  put(34, size);
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(h), sizeof(h));
  out.write(reinterpret_cast<const char*>(v.bgra.data()), size);
  return out.good();
}
}  // namespace

int main(int argc, char** argv) {
  std::string source, dump, tally;
  bool list = false;
  int seconds = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) usage();
      return argv[++i];
    };
    if (a == "--list") list = true;
    else if (a == "--source") source = value();
    else if (a == "--seconds") seconds = std::atoi(value().c_str());
    else if (a == "--dump") dump = value();
    else if (a == "--tally") tally = value();
    else usage();
  }

  try {
    if (list) {
      discover();
      std::this_thread::sleep_for(std::chrono::seconds(2));  // discovery needs a moment
      for (const std::string& s : discover()) std::puts(s.c_str());
      return 0;
    }
    if (source.empty()) usage();

    Receiver receiver(source);
    if (!tally.empty()) receiver.setTally(Tally{tally == "preview", tally == "program"});
    using Clock = std::chrono::steady_clock;
    const auto end = Clock::now() + std::chrono::seconds(seconds);
    auto report = Clock::now() + std::chrono::seconds(1);
    ReceiverStats last;
    VideoFrame frame;
    double delaySum = 0, delayMax = 0;
    int delays = 0;
    while (seconds == 0 || Clock::now() < end) {
      if (receiver.receiveVideo(frame, 100)) {
        const int64_t now =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count() / 100;
        const double delay = (now - frame.timestamp) / 10'000.0;  // ms
        delaySum += delay;
        delayMax = std::max(delayMax, delay);
        ++delays;
        if (!dump.empty()) {
          if (writeBmp(dump, frame)) {
            std::printf("wrote %s\n", dump.c_str());
          } else {
            std::fprintf(stderr, "omthx-recv: cannot write %s\n", dump.c_str());
          }
          dump.clear();
        }
      }
      if (Clock::now() >= report) {
        report += std::chrono::seconds(1);
        const ReceiverStats s = receiver.stats();
        std::printf("%-4s %-16s %3llu fps  %7.2f Mbps  decode %5.2f ms  discarded %llu  delay avg %6.2f max %6.2f ms  %dx%d%s\n",
                    s.mode == Mode::Hx ? "HX" : "Full", s.decoder.c_str(),
                    static_cast<unsigned long long>(s.framesReceived - last.framesReceived),
                    (s.bytesReceived - last.bytesReceived) * 8 / 1e6, s.lastDecodeMs,
                    static_cast<unsigned long long>(s.framesDiscarded), delays ? delaySum / delays : 0.0, delayMax,
                    frame.format.width, frame.format.height, frame.format.alpha ? " +alpha" : "");
        last = s;
        delaySum = delayMax = 0;
        delays = 0;
      }
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "omthx-recv: %s\n", e.what());
    return 1;
  }
  return 0;
}