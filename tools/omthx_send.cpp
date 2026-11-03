// Sends a moving test picture as an OMT or OMT-HX source and prints what the sender measures.
#ifdef _WIN32
#include <windows.h>

#include <d3d11.h>
#include <timeapi.h>
#include <wrl/client.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "omthx/omthx.h"
#include "pattern.h"

using namespace omthx;

namespace {
[[noreturn]] void usage() {
  std::puts("omthx-send [--name TEXT] [--mode hx|full] [--codec hevc|h264] [--quality low|medium|high]\n"
            "           [--size WxH] [--fps N] [--alpha] [--tone] [--gpu] [--seconds N]\n"
            "Defaults: --name \"OMT-HX Test\" --mode hx --codec hevc --quality medium --size 1920x1080 --fps 60,\n"
            "runs until Ctrl+C. --codec is ignored with --mode full.\n"
            "--tone adds a 1 kHz tone at -20 dBFS, stereo, 48 kHz.\n"
            "--gpu (Windows, no --alpha) sends D3D11 textures with sendVideoD3D11, as BreadCamera and desktop\n"
            "capture do: the picture never comes to memory.\n"
            "--uyvy (no --alpha, not with --gpu) sends UYVY pictures from memory with sendVideoUyvy, as an SDI or\n"
            "NDI input does: no GPU before the encoder.\n");
  std::exit(2);
}

// BGRA to UYVY, BT.709 limited range (what an SDI card delivers). Once per looped picture, so plain C++.
std::vector<uint8_t> toUyvy(const std::vector<uint8_t>& bgra, int w, int h) {
  std::vector<uint8_t> out(static_cast<size_t>(w) * h * 2);
  auto yOf = [](double r, double g, double b) { return 16 + 219 * (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255; };
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; x += 2) {
      const uint8_t* p = bgra.data() + (static_cast<size_t>(y) * w + x) * 4;
      const double r = (p[2] + p[6]) / 2.0, g = (p[1] + p[5]) / 2.0, b = (p[0] + p[4]) / 2.0;
      const double yy = yOf(r, g, b);
      const double u = 128 + 224 * (b - (yy - 16) * 255 / 219) / 255 / 1.8556;
      const double v = 128 + 224 * (r - (yy - 16) * 255 / 219) / 255 / 1.5748;
      uint8_t* o = out.data() + (static_cast<size_t>(y) * w + x) * 2;
      o[0] = static_cast<uint8_t>(std::clamp(u, 16.0, 240.0));
      o[1] = static_cast<uint8_t>(std::clamp(yOf(p[2], p[1], p[0]), 16.0, 235.0));
      o[2] = static_cast<uint8_t>(std::clamp(v, 16.0, 240.0));
      o[3] = static_cast<uint8_t>(std::clamp(yOf(p[6], p[5], p[4]), 16.0, 235.0));
    }
  }
  return out;
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
  bool gpu = false;
  bool uyvy = false;
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
    } else if (a == "--gpu") {
      gpu = true;
    } else if (a == "--uyvy") {
      uyvy = true;
    } else {
      usage();
    }
  }
  if (f.fpsN <= 0) usage();
#ifndef _WIN32
  if (gpu) usage();
#endif
  if (gpu && f.alpha) usage();  // sendVideoD3D11 takes no alpha
  if (uyvy && (gpu || f.alpha || f.width % 2)) usage();

  try {
    // Sixteen pictures drawn once and looped: drawing 1080p in real time would cost more than sending it.
    std::vector<std::vector<uint8_t>> loop(16);
    for (int i = 0; i < 16; ++i) drawPattern(loop[i], f.width, f.height, i * 4, f.alpha);
    if (uyvy)
      for (auto& pic : loop) pic = toUyvy(pic, f.width, f.height);

#ifdef _WIN32
    // --gpu: the sixteen pictures as textures on a hardware device, sent from video memory.
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> textures;
    if (gpu) {
      if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)))
        throw std::runtime_error("no D3D11 hardware device for --gpu");
      D3D11_TEXTURE2D_DESC desc = {};
      desc.Width = static_cast<UINT>(f.width);
      desc.Height = static_cast<UINT>(f.height);
      desc.MipLevels = desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
      for (auto& pic : loop) {
        const D3D11_SUBRESOURCE_DATA init{pic.data(), static_cast<UINT>(f.width * 4), 0};
        if (FAILED(device->CreateTexture2D(&desc, &init, &textures.emplace_back())))
          throw std::runtime_error("cannot create a texture for --gpu");
      }
    }
#endif
    Sender sender(name, mode, quality, codec);
    std::printf("source: %s%s\n", sender.address().c_str(),
                gpu ? "  (from D3D11 textures)" : uyvy ? "  (UYVY from memory, as SDI)" : "  (BGRA from memory)");
#ifdef _WIN32
    timeBeginPeriod(1);  // 1 ms sleeps instead of 15 ms
#endif
    using Clock = std::chrono::steady_clock;
    const int64_t epoch =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count() /
        100;  // wall clock in OMT units, so omthx-recv on this PC can measure delay
    const auto t0 = Clock::now();
    auto report = t0 + std::chrono::seconds(1);
    SenderStats last;
    const int64_t total = static_cast<int64_t>(seconds) * f.fpsN / f.fpsD;
    for (int64_t i = 0; seconds == 0 || i < total; ++i) {
      const int64_t ts = epoch + i * 10'000'000LL * f.fpsD / f.fpsN;
#ifdef _WIN32
      if (gpu) {
        sender.sendVideoD3D11(f, textures[static_cast<size_t>(i % 16)].Get(), ts);
      } else
#endif
      if (uyvy) {
        sender.sendVideoUyvy(f, loop[static_cast<size_t>(i % 16)].data(), f.width * 2, nullptr, 0, false, ts);
      } else {
        sender.sendVideo(f, loop[static_cast<size_t>(i % 16)], false, ts);
      }
      if (tone) {  // one block per frame, sized so the sample count never drifts from the frame count
        const int64_t first = i * 48000 * f.fpsD / f.fpsN;
        const int64_t samples = (i + 1) * 48000 * f.fpsD / f.fpsN - first;
        const size_t n = samples > 0 ? static_cast<size_t>(samples) : 1;
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
        char rtt[32] = "-";  // no receiver has replied (yet)
        if (s.connections > 0 && s.roundTripMs >= 0) std::snprintf(rtt, sizeof(rtt), "%.1f ms", s.roundTripMs);
        std::printf("%-11s %3llu fps  %7.2f Mbps  encode %5.2f ms  keyframes %llu  dropped %llu  connections %d  "
                    "round trip %s  tally %s%s\n",
                    s.encoder.c_str(), static_cast<unsigned long long>(s.framesSent - last.framesSent),
                    (s.bytesSent >= last.bytesSent ? s.bytesSent - last.bytesSent : s.bytesSent) * 8 / 1e6, s.lastEncodeMs,  // the count restarts with a new connection
                    static_cast<unsigned long long>(s.keyframes), static_cast<unsigned long long>(s.framesDropped),
                    s.connections, rtt, t.program ? "PGM " : "", t.preview ? "PVW" : "");
        last = s;
      }
    }
    #ifdef _WIN32
    timeEndPeriod(1);
#endif
  } catch (const std::exception& e) {
    std::fprintf(stderr, "omthx-send: %s\n", e.what());
    return 1;
  }
  return 0;
}