// Connects to an OMT or OMT-HX source and prints what the receiver measures.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "omthx/omthx.h"
#include "tick.h"

using namespace omthx;

namespace {
[[noreturn]] void usage() {
  std::puts("omthx-recv --list\n"
            "omthx-recv --source \"HOST (Name)\" [--seconds N] [--dump FILE.bmp] [--tally program|preview] [--preview]\n"
            "           [--pixels bgra|nv12|gpu] [--live-seconds N] [--tick]\n"
            "--live-seconds N stops after N seconds in which pictures arrived; --seconds then caps the wait.\n"
            "--pixels: bgra (default) brings the picture to memory as BGRA; nv12 brings the decoder's NV12 with no\n"
            "conversion; gpu keeps an HX picture in video memory, as an app that draws it on the GPU would.\n"
            "--tick (with omthx-send --tick; not with --pixels gpu) finds each second's white picture and beep and\n"
            "prints their delays and how much later the beep arrives than the picture. That last number needs no\n"
            "clock sync: it is right across two PCs.\n"
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

int64_t wallNow() {  // OMT units (100 ns) on the wall clock, as omthx-send stamps
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
             .count() / 100;
}

// --tick: pairs each flash with the beep stamped at the same moment and prints both delays.
class TickMatcher {
 public:
  struct Event {
    int64_t stamp;   // the source's timestamp of the flash or of the beep's first loud sample
    double delayMs;  // arrival here minus the timestamp of the frame that carried it
  };

  void add(bool audio, Event e) {
    std::lock_guard<std::mutex> lock(m_);
    std::deque<Event>& mine = audio ? audio_ : video_;
    std::deque<Event>& other = audio ? video_ : audio_;
    for (auto it = other.begin(); it != other.end(); ++it) {
      if (std::llabs(it->stamp - e.stamp) < 5'000'000) {  // within 0.5 s of stream time: the same second
        const Event& v = audio ? *it : e;
        const Event& a = audio ? e : *it;
        const double late = a.delayMs - v.delayMs;
        lates_.push_back(late);
        videoDelays_.push_back(v.delayMs);
        audioDelays_.push_back(a.delayMs);
        std::printf("tick  video delay %7.1f ms  audio delay %7.1f ms  audio later by %+7.1f ms  (stamped %+.1f ms apart)\n",
                    v.delayMs, a.delayMs, late, (a.stamp - v.stamp) / 10'000.0);
        other.erase(it);
        return;
      }
    }
    mine.push_back(e);
    if (mine.size() > 8) mine.pop_front();  // its partner was lost
  }

  void summary() {
    std::lock_guard<std::mutex> lock(m_);
    if (lates_.empty()) {
      std::puts("tick summary: no flash and beep pairs. Is the source omthx-send --tick?");
      return;
    }
    std::printf("tick summary: %zu pairs  median video delay %.1f ms  audio delay %.1f ms  audio later by %+.1f ms "
                "(min %+.1f, max %+.1f)\n",
                lates_.size(), median(videoDelays_), median(audioDelays_), median(lates_),
                *std::min_element(lates_.begin(), lates_.end()), *std::max_element(lates_.begin(), lates_.end()));
  }

 private:
  static double median(std::vector<double> v) {
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
  }
  std::mutex m_;
  std::deque<Event> video_, audio_;
  std::vector<double> lates_, videoDelays_, audioDelays_;
};

// True when a received picture is the white flash, whichever pixels it came as.
bool flashOf(const VideoFrame& f) {
  const int w = f.format.width, h = f.format.height;
  if (!f.bgra.empty()) return tick::isFlash(f.bgra.data(), w, h, 4, w * 4);
  if (f.nv12Y) return tick::isFlash(f.nv12Y, w, h, 1, w);
  if (!f.uyvy.empty()) return tick::isFlash(f.uyvy.data() + 1, w, h, 2, w * 2);  // Y is every second byte
  return false;
}
}  // namespace

int main(int argc, char** argv) {
  std::string source, dump, tally;
  bool list = false, preview = false, tick = false;
  int seconds = 0;
  int liveSeconds = 0, live = 0;  // stop after this many seconds with pictures
  PixelFormat pixels = PixelFormat::Bgra;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) usage();
      return argv[++i];
    };
    if (a == "--list") list = true;
    else if (a == "--source") source = value();
    else if (a == "--seconds") seconds = std::atoi(value().c_str());
    else if (a == "--live-seconds") liveSeconds = std::atoi(value().c_str());
    else if (a == "--dump") dump = value();
    else if (a == "--tally") tally = value();
    else if (a == "--preview") preview = true;  // the small picture, as NDI's lowest bandwidth
    else if (a == "--tick") tick = true;
    else if (a == "--pixels") {
      const std::string p = value();
      if (p == "bgra") pixels = PixelFormat::Bgra;
      else if (p == "nv12") pixels = PixelFormat::Nv12;
      else if (p == "gpu") pixels = PixelFormat::Gpu;
      else usage();
    } else usage();
  }

  try {
    if (list) {
      discover();
      std::this_thread::sleep_for(std::chrono::seconds(2));  // discovery needs a moment
      for (const std::string& s : discover()) std::puts(s.c_str());
      return 0;
    }
    if (source.empty()) usage();

    if (!dump.empty() && pixels == PixelFormat::Nv12) {
      std::fputs("omthx-recv: --dump needs --pixels bgra or gpu\n", stderr);
      return 2;
    }
    if (tick && pixels == PixelFormat::Gpu) usage();
    Receiver receiver(source, pixels, preview);
    if (!tally.empty()) receiver.setTally(Tally{tally == "preview", tally == "program"});
    TickMatcher ticks;
    // --tick: audio on its own thread, as a player would. Declared after receiver, so it stops before receiver goes.
    std::jthread audioThread;
    if (tick) {
      audioThread = std::jthread([&](std::stop_token stop) {
        AudioFrame a;
        bool inBeep = false;  // the beep spans several blocks: only its first loud block counts
        while (!stop.stop_requested()) {
          if (!receiver.receiveAudio(a, 100)) continue;
          const int64_t now = wallNow();
          const int at = a.planar.empty() || a.sampleRate <= 0 ? -1 : tick::onset(a.planar.data(), a.samples);
          if (at < 0) {
            inBeep = false;
          } else if (!inBeep) {
            inBeep = true;
            ticks.add(true, {a.timestamp + at * 10'000'000LL / a.sampleRate, (now - a.timestamp) / 10'000.0});
          }
        }
      });
    }
    using Clock = std::chrono::steady_clock;
    const auto end = Clock::now() + std::chrono::seconds(seconds);
    auto report = Clock::now() + std::chrono::seconds(1);
    ReceiverStats last;
    VideoFrame frame;
    double delaySum = 0, delayMax = 0;
    int delays = 0;
    while (seconds == 0 || Clock::now() < end) {
      if (receiver.receiveVideo(frame, 100)) {
        const double delay = (wallNow() - frame.timestamp) / 10'000.0;  // ms
        if (tick && flashOf(frame)) ticks.add(false, {frame.timestamp, delay});
        delaySum += delay;
        delayMax = std::max(delayMax, delay);
        ++delays;
        if (!dump.empty()) {
          // A Gpu picture comes to memory only for the dump; a plain OMT source already filled uyvy, not bgra.
          if (pixels == PixelFormat::Gpu && frame.gpu) download(frame, PixelFormat::Bgra);
          if (!frame.bgra.empty() && writeBmp(dump, frame)) {
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
        std::printf("%-4s %-16s %3llu fps  %7.2f Mbps  decode %5.2f ms  discarded %llu  "
                    "delay avg %6.2f max %6.2f ms  %dx%d%s  src %s\n",
                    s.mode == Mode::Hx ? "HX" : "Full", s.decoder.c_str(),
                    static_cast<unsigned long long>(s.framesReceived - last.framesReceived),
                    (s.bytesReceived >= last.bytesReceived ? s.bytesReceived - last.bytesReceived : s.bytesReceived) * 8 / 1e6, s.lastDecodeMs,  // the count restarts with a new connection
                    static_cast<unsigned long long>(s.framesDiscarded), delays ? delaySum / delays : 0.0, delayMax,
                    frame.format.width, frame.format.height, frame.format.alpha ? " +alpha" : "",
                    s.encoder.empty() ? "-" : s.encoder.c_str());
        const bool arrived = s.framesReceived > last.framesReceived;
        last = s;
        delaySum = delayMax = 0;
        delays = 0;
        if (arrived && liveSeconds > 0 && ++live >= liveSeconds) break;
      }
    }
    if (tick) {
      audioThread.request_stop();
      audioThread.join();
      ticks.summary();
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "omthx-recv: %s\n", e.what());
    return 1;
  }
  return 0;
}