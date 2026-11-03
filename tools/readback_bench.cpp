// Times bringing a 4K picture from each GPU to memory, split into the GPU copy and the CPU read, so a slow
// receiver can be told apart: a slow link to the GPU, or slow reading of uncached (write-combined) memory.
#include <d3d11.h>
#include <dxgi1_6.h>
#include <smmintrin.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

// Reads 64 bytes at a time with streaming loads (MOVNTDQA): fast from write-combined memory, the same as normal
// loads from cached memory.
static void streamCopy(uint8_t* dst, const uint8_t* src, size_t n) {
  size_t i = 0;
  if ((reinterpret_cast<uintptr_t>(src) & 15) == 0) {
    for (; i + 64 <= n; i += 64) {
      __m128i a = _mm_stream_load_si128((__m128i*)(src + i)), b = _mm_stream_load_si128((__m128i*)(src + i + 16));
      __m128i c = _mm_stream_load_si128((__m128i*)(src + i + 32)), d = _mm_stream_load_si128((__m128i*)(src + i + 48));
      _mm_storeu_si128((__m128i*)(dst + i), a); _mm_storeu_si128((__m128i*)(dst + i + 16), b);
      _mm_storeu_si128((__m128i*)(dst + i + 32), c); _mm_storeu_si128((__m128i*)(dst + i + 48), d);
    }
  }
  std::memcpy(dst + i, src + i, n - i);
}

static void bench(IDXGIAdapter1* adapter, DXGI_FORMAT format, const char* name, UINT bytesPerRow, UINT rows) {
  ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
  if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
    std::puts("    cannot create a device"); return;
  }
  D3D11_TEXTURE2D_DESC d = {}; d.Width = 3840; d.Height = 2160; d.MipLevels = d.ArraySize = 1; d.Format = format;
  d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> gpu, staging;
  if (FAILED(dev->CreateTexture2D(&d, nullptr, &gpu))) { std::printf("    %s: no texture\n", name); return; }
  d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  dev->CreateTexture2D(&d, nullptr, &staging);
  std::vector<uint8_t> out(size_t(bytesPerRow) * rows);
  const int N = 40; double copyMs = 0, readMs = 0, streamMs = 0;
  for (int i = 0; i < N + 5; ++i) {
    auto t0 = Clock::now();
    ctx->CopyResource(staging.Get(), gpu.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);  // waits for the GPU copy
    auto t1 = Clock::now();
    for (UINT r = 0; r < rows; ++r) std::memcpy(out.data() + size_t(r) * bytesPerRow, (uint8_t*)m.pData + size_t(r) * m.RowPitch, bytesPerRow);
    auto t2 = Clock::now();
    for (UINT r = 0; r < rows; ++r) streamCopy(out.data() + size_t(r) * bytesPerRow, (uint8_t*)m.pData + size_t(r) * m.RowPitch, bytesPerRow);
    auto t3 = Clock::now();
    ctx->Unmap(staging.Get(), 0);
    if (i >= 5) { copyMs += ms(t0, t1); readMs += ms(t1, t2); streamMs += ms(t2, t3); }
  }
  const double mb = out.size() / 1e6;
  std::printf("    %-5s %5.1f MB: GPU copy %6.2f ms (%5.2f GB/s) | CPU read memcpy %6.2f ms (%5.2f GB/s) | streaming %6.2f ms (%5.2f GB/s)\n",
              name, mb, copyMs / N, mb / (copyMs / N), readMs / N, mb / (readMs / N), streamMs / N, mb / (streamMs / N));
}

int main() {
  ComPtr<IDXGIFactory1> f;
  CreateDXGIFactory1(IID_PPV_ARGS(&f));
  ComPtr<IDXGIAdapter1> a;
  for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc; a->GetDesc1(&desc);
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    std::printf("GPU %u: %ls (%llu MB video memory)\n", i, desc.Description, (unsigned long long)(desc.DedicatedVideoMemory >> 20));
    bench(a.Get(), DXGI_FORMAT_NV12, "NV12", 3840, 2160 * 3 / 2);
    bench(a.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, "BGRA", 3840 * 4, 2160);
  }
  std::puts("Budget at 4K60: 16.7 ms per frame for everything.");
}
