// BGRA or RGBA or UYVY -> NV12 D3D11 frame on the GPU, so a picture reaches the encoder without leaving video memory.
#include "codec.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <d3d11_4.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

using Microsoft::WRL::ComPtr;

namespace omthx {
namespace {
void check(HRESULT hr, const char* what) {
  if (FAILED(hr)) throw std::runtime_error(std::string("d3d11: ") + what + " failed");
}

// The texture formats a picture may arrive in: BGRA or RGBA, either colour space.
constexpr DXGI_FORMAT kTextureFormats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                           DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};

int textureFormatIndex(DXGI_FORMAT format) {
  int i = 0;
  for (const DXGI_FORMAT f : kTextureFormats) {
    if (f == format) return i;
    ++i;
  }
  return -1;
}

const char* formatName(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_YUY2: return "YUY2";
    default: return "an unknown format";
  }
}

using RangeTable = std::array<uint8_t, 256>;
// packUyvy's limited-range-to-full mapping (makeTable in src/pure.cpp), rounded half up in integers.
RangeTable rangeTable(int offsetIn, int num, int den, int offsetOut) {
  RangeTable t{};
  for (int i = 0; i < 256; ++i) {
    const int v = 2 * (i - offsetIn) * num + (2 * offsetOut + 1) * den;
    const int q = v >= 0 ? v / (2 * den) : -((-v + 2 * den - 1) / (2 * den));
    t[i] = static_cast<uint8_t>(std::clamp(q, 0, 255));
  }
  return t;
}
const RangeTable kLumaToFull = rangeTable(16, 255, 219, 0);
const RangeTable kChromaToFull = rangeTable(128, 255, 224, 128);

// The video processor and the NV12 frame pool it writes, on one device at one size: what BgraToNv12 and
// UyvyToNv12 share.
struct VideoConverter {
  ID3D11Device* device = nullptr;  // not owned: identifies the device the objects below belong to
  int width = 0, height = 0;
  bool rgbStream = false;  // the stream is full-range RGB, else full-range BT.709 YCbCr
  ComPtr<ID3D11VideoDevice> video;
  ComPtr<ID3D11VideoContext> context;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
  ComPtr<ID3D11VideoProcessor> processor;
  DXGI_FORMAT checked = DXGI_FORMAT_UNKNOWN;  // the input format the enumerator last accepted
  AVBufferRef* hwDevice = nullptr;
  AVBufferRef* frames = nullptr;

  ~VideoConverter() { reset(); }

  void reset() {
    av_buffer_unref(&frames);
    av_buffer_unref(&hwDevice);
    processor.Reset();
    enumerator.Reset();
    context.Reset();
    video.Reset();
    checked = DXGI_FORMAT_UNKNOWN;  // the enumerator went with them
    device = nullptr;
  }

  // The video processor and the NV12 frame pool for this device, size and stream colour, made once.
  void setup(ID3D11Device* dev, int w, int h, bool rgb) {
    if (dev == device && w == width && h == height && rgb == rgbStream) return;
    reset();
    check(dev->QueryInterface(IID_PPV_ARGS(&video)), "the device's video interface");
    ComPtr<ID3D11DeviceContext> immediate;
    dev->GetImmediateContext(&immediate);
    check(immediate.As(&context), "the context's video interface");
    // The capture thread copies on this context while the sender thread converts and encodes.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(immediate.As(&mt))) mt->SetMultithreadProtected(TRUE);

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.InputWidth = static_cast<UINT>(w);
    content.InputHeight = static_cast<UINT>(h);
    content.OutputFrameRate = {60, 1};
    content.OutputWidth = static_cast<UINT>(w);
    content.OutputHeight = static_cast<UINT>(h);
    content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    check(video->CreateVideoProcessorEnumerator(&content, &enumerator), "CreateVideoProcessorEnumerator");
    check(video->CreateVideoProcessor(enumerator.Get(), 0, &processor), "CreateVideoProcessor");

    // Both sides full-range BT.709, the matrix and range the encoder signals. UyvyToNv12 expands the range itself:
    // this GPU's video processor passes YCbCr to YCbCr through without converting the range (measured 2026-11-04).
    ComPtr<ID3D11VideoContext1> context1;
    if (SUCCEEDED(context.As(&context1))) {
      context1->VideoProcessorSetStreamColorSpace1(processor.Get(), 0, rgb ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
                                                                          : DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709);
      context1->VideoProcessorSetOutputColorSpace1(processor.Get(), DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709);
    } else {
      D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs = {};
      cs.YCbCr_Matrix = 1;  // BT.709
      cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
      context->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &cs);
      context->VideoProcessorSetOutputColorSpace(processor.Get(), &cs);
    }
    context->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    context->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
    const RECT rect{0, 0, w, h};  // the texture may be larger: only its top-left corner is the picture
    context->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &rect);
    context->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &rect);
    context->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &rect);

    hwDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hwDevice) throw std::runtime_error("d3d11: cannot allocate the device context");
    auto* hw = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(hwDevice->data)->hwctx);
    dev->AddRef();  // FFmpeg releases it with the context
    hw->device = dev;
    if (av_hwdevice_ctx_init(hwDevice) < 0) throw std::runtime_error("d3d11: cannot wrap the device for FFmpeg");

    frames = av_hwframe_ctx_alloc(hwDevice);
    if (!frames) throw std::runtime_error("d3d11: cannot allocate the frame pool");
    auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
    fc->format = AV_PIX_FMT_D3D11;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = w;
    fc->height = h;
    // No initial pool: FFmpeg would make it one texture array, and NV12 render targets cannot be arrays. Single
    // textures are made as the encoder asks and reused through FFmpeg's buffer pool.
    fc->initial_pool_size = 0;
    static_cast<AVD3D11VAFramesContext*>(fc->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;  // the processor writes it
    if (av_hwframe_ctx_init(frames) < 0) throw std::runtime_error("d3d11: cannot create the NV12 frame pool");

    device = dev;
    width = w;
    height = h;
    rgbStream = rgb;
  }

  // Whether the video processor can read this format, asked once per format and remembered.
  void checkInputFormat(DXGI_FORMAT format) {
    if (checked == format) return;
    UINT support = 0;
    check(enumerator->CheckVideoProcessorFormat(format, &support), "CheckVideoProcessorFormat");
    if (!(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
      throw std::runtime_error(std::string("the video processor cannot read ") + formatName(format));
    checked = format;
  }

  // The top-left corner set up as the picture, as a new NV12 frame in `out`.
  void blt(ID3D11Texture2D* texture, AVFrame* out) {
    if (av_hwframe_get_buffer(frames, out, 0) < 0) throw std::runtime_error("d3d11: no free NV12 frame in the pool");
    auto* nv12 = reinterpret_cast<ID3D11Texture2D*>(out->data[0]);
    const UINT slice = static_cast<UINT>(reinterpret_cast<intptr_t>(out->data[1]));

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in = {};
    in.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorInputView> inView;
    check(video->CreateVideoProcessorInputView(texture, enumerator.Get(), &in, &inView), "the input view");
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC o = {};
    o.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
    o.Texture2DArray = {0, slice, 1};
    ComPtr<ID3D11VideoProcessorOutputView> outView;
    check(video->CreateVideoProcessorOutputView(nv12, enumerator.Get(), &o, &outView), "the NV12 output view");

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inView.Get();
    auto* hw = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(hwDevice->data)->hwctx);
    hw->lock(hw->lock_ctx);  // FFmpeg's own lock on this context, which the encoder takes too
    const HRESULT hr = context->VideoProcessorBlt(processor.Get(), outView.Get(), 0, 1, &stream);
    hw->unlock(hw->lock_ctx);
    check(hr, "VideoProcessorBlt");
  }
};
}  // namespace

struct BgraToNv12::Impl : VideoConverter {
  void convert(ID3D11Texture2D* texture, int w, int h, AVFrame* out) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    if (textureFormatIndex(desc.Format) < 0 || desc.Width < static_cast<UINT>(w) ||
        desc.Height < static_cast<UINT>(h))
      throw std::invalid_argument("the texture must be BGRA or RGBA and at least the picture's size");
    ComPtr<ID3D11Device> dev;
    texture->GetDevice(&dev);
    setup(dev.Get(), w, h, true);
    checkInputFormat(desc.Format);
    blt(texture, out);
  }
};

BgraToNv12::BgraToNv12() : d_(std::make_unique<Impl>()) {}
BgraToNv12::~BgraToNv12() = default;

void BgraToNv12::convert(void* texture, int width, int height, GpuPicture& out) {
  av_frame_unref(out.frame);
  d_->convert(static_cast<ID3D11Texture2D*>(texture), width, height, out.frame);
}

// ---------------------------------------------------------------- UyvyToNv12

struct UyvyToNv12::Impl {
  ComPtr<ID3D11Device> device;  // this object's own: the default adapter, with video support
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Texture2D> upload;  // DYNAMIC YUY2, what Map writes
  // A video processor input view needs a DEFAULT texture, so the mapped rows are copied to this one on the GPU.
  ComPtr<ID3D11Texture2D> yuy2;
  VideoConverter converter;
  int width = 0, height = 0;

  void convert(const uint8_t* uyvy, int stride, int w, int h, AVFrame* out) {
    if (w < 2 || h < 2 || w % 2 || stride < w * 2)
      throw std::invalid_argument("UYVY needs an even size and a stride of at least width * 2");
    if (!device) {
      const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
      const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                           device.GetAddressOf(), nullptr, context.GetAddressOf());
      if (FAILED(hr)) {
        device.Reset();
        context.Reset();
        throw std::runtime_error("d3d11: no video device to convert UYVY");
      }
    }
    if (!upload || w != width || h != height) {
      D3D11_TEXTURE2D_DESC d = {};
      d.Width = static_cast<UINT>(w);
      d.Height = static_cast<UINT>(h);
      d.MipLevels = d.ArraySize = 1;
      d.Format = DXGI_FORMAT_YUY2;
      d.SampleDesc.Count = 1;
      d.Usage = D3D11_USAGE_DYNAMIC;
      d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;  // YUY2 needs a bind flag to exist at all
      upload.Reset();
      check(device->CreateTexture2D(&d, nullptr, &upload), "the YUY2 upload texture");
      d.Usage = D3D11_USAGE_DEFAULT;
      d.CPUAccessFlags = 0;
      d.BindFlags = D3D11_BIND_DECODER;  // the bind flag the video processor accepts on an input surface
      yuy2.Reset();
      check(device->CreateTexture2D(&d, nullptr, &yuy2), "the YUY2 texture");
      width = w;
      height = h;
    }
    D3D11_MAPPED_SUBRESOURCE m = {};
    check(context->Map(upload.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map of the YUY2 texture");
    for (int r = 0; r < h; ++r) {
      const uint8_t* s = uyvy + static_cast<size_t>(r) * stride;
      auto* d = static_cast<uint8_t*>(m.pData) + static_cast<size_t>(r) * m.RowPitch;
      for (int x = 0; x < w * 2; x += 4) {  // U Y0 V Y1 -> Y0 U Y1 V, expanded to the full range
        d[x] = kLumaToFull[s[x + 1]];
        d[x + 1] = kChromaToFull[s[x]];
        d[x + 2] = kLumaToFull[s[x + 3]];
        d[x + 3] = kChromaToFull[s[x + 2]];
      }
    }
    context->Unmap(upload.Get(), 0);
    context->CopyResource(yuy2.Get(), upload.Get());
    converter.setup(device.Get(), w, h, false);
    converter.checkInputFormat(DXGI_FORMAT_YUY2);
    converter.blt(yuy2.Get(), out);
  }
};

UyvyToNv12::UyvyToNv12() : d_(std::make_unique<Impl>()) {}
UyvyToNv12::~UyvyToNv12() = default;

void UyvyToNv12::convert(const uint8_t* uyvy, int stride, int width, int height, GpuPicture& out) {
  av_frame_unref(out.frame);
  d_->convert(uyvy, stride, width, height, out.frame);
}

// ---------------------------------------------------------------- Readback

struct Readback::Impl {
  struct Slot {
    ComPtr<ID3D11Texture2D> staging;
    UINT texWidth = 0, texHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool pending = false;
    uint64_t order = 0;  // larger is newer
    std::chrono::steady_clock::time_point queued;
    int width = 0, height = 0;
    UINT planeRows = 0;  // NV12: the staging texture's height, where the UV plane starts
    Picture picture;
  };
  // Two copies in flight at most (this picture and the one before), and one spare.
  static constexpr int kSlots = 3;
  Slot slots[kSlots];
  uint64_t next = 0;

  AVBufferRef* deviceRef = nullptr;  // the decoder's FFmpeg device: its D3D11 device, context and lock
  AVD3D11VADeviceContext* hw = nullptr;
  // BGRA on the GPU: the device's video processor, made on first use and per size.
  ComPtr<ID3D11VideoDevice> video;
  ComPtr<ID3D11VideoContext> vcontext;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
  ComPtr<ID3D11VideoProcessor> processor;
  ComPtr<ID3D11Texture2D> target;  // the processor's BGRA output, copied to a slot's staging texture
  int vpWidth = 0, vpHeight = 0;

  ~Impl() {
    detach();
    if (timer) CloseHandle(timer);
  }

  void detach() {
    for (Slot& s : slots) s = Slot{};
    target.Reset();
    processor.Reset();
    enumerator.Reset();
    vcontext.Reset();
    video.Reset();
    vpWidth = vpHeight = 0;
    hw = nullptr;
    av_buffer_unref(&deviceRef);
  }

  // Follows the decoder's device: a new decoder (another codec, the preview stream) may bring a new one.
  void attach(const AVFrame* frame) {
    auto* fc = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
    if (deviceRef && deviceRef->data == fc->device_ref->data) return;
    detach();
    deviceRef = av_buffer_ref(fc->device_ref);
    if (!deviceRef) throw std::runtime_error("d3d11: cannot hold the decoder's device");
    hw = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(deviceRef->data)->hwctx);
  }

  struct Lock {
    AVD3D11VADeviceContext* hw;
    explicit Lock(AVD3D11VADeviceContext* h) : hw(h) { hw->lock(hw->lock_ctx); }
    ~Lock() { hw->unlock(hw->lock_ctx); }
  };

  void ensureStaging(Slot& s, UINT w, UINT h, DXGI_FORMAT format) {
    if (s.staging && s.texWidth == w && s.texHeight == h && s.format == format) return;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    s.staging.Reset();
    check(hw->device->CreateTexture2D(&d, nullptr, &s.staging), "the staging texture");
    s.texWidth = w;
    s.texHeight = h;
    s.format = format;
  }

  // The video processor for NV12 -> BGRA at this size, full-range BT.709 in (as the encoder signals), full-range
  // RGB out: what Packer::unpack makes on the CPU.
  void ensureProcessor(int w, int h) {
    if (processor && w == vpWidth && h == vpHeight) return;
    processor.Reset();
    enumerator.Reset();
    target.Reset();
    if (!video) {
      check(hw->device->QueryInterface(IID_PPV_ARGS(&video)), "the device's video interface");
      check(hw->device_context->QueryInterface(IID_PPV_ARGS(&vcontext)), "the context's video interface");
    }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.InputWidth = static_cast<UINT>(w);
    content.InputHeight = static_cast<UINT>(h);
    content.OutputFrameRate = {60, 1};
    content.OutputWidth = static_cast<UINT>(w);
    content.OutputHeight = static_cast<UINT>(h);
    content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    check(video->CreateVideoProcessorEnumerator(&content, &enumerator), "CreateVideoProcessorEnumerator");
    UINT support = 0;
    check(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &support), "CheckVideoProcessorFormat");
    if (!(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
      throw std::runtime_error("the video processor cannot write BGRA");
    check(video->CreateVideoProcessor(enumerator.Get(), 0, &processor), "CreateVideoProcessor");
    ComPtr<ID3D11VideoContext1> context1;
    if (SUCCEEDED(vcontext.As(&context1))) {
      context1->VideoProcessorSetStreamColorSpace1(processor.Get(), 0, DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709);
      context1->VideoProcessorSetOutputColorSpace1(processor.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    } else {
      D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs = {};
      cs.YCbCr_Matrix = 1;  // BT.709
      cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
      vcontext->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &cs);
      vcontext->VideoProcessorSetOutputColorSpace(processor.Get(), &cs);
    }
    vcontext->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vcontext->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
    const RECT rect{0, 0, w, h};  // the decoder's surface is larger (aligned): only its top-left is the picture
    vcontext->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &rect);
    vcontext->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &rect);
    vcontext->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &rect);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    check(hw->device->CreateTexture2D(&d, nullptr, &target), "the BGRA texture");
    vpWidth = w;
    vpHeight = h;
  }

  Slot& freeSlot() {
    Slot* oldest = &slots[0];
    for (Slot& s : slots) {
      if (!s.pending) return s;
      if (s.order < oldest->order) oldest = &s;
    }
    oldest->pending = false;  // all in flight: the oldest is dropped, it would never be handed out
    return *oldest;
  }

  void push(const AVFrame* frame, const VideoFormat& format, int64_t timestamp, bool bgra) {
    attach(frame);
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    const UINT index = static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    const int w = frame->width, h = frame->height;
    Slot& s = freeSlot();
    if (bgra) {
      ensureProcessor(w, h);
      ensureStaging(s, static_cast<UINT>(w), static_cast<UINT>(h), DXGI_FORMAT_B8G8R8A8_UNORM);
      D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in = {};
      in.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
      in.Texture2D.ArraySlice = index;  // the decoder's surfaces are one texture array
      ComPtr<ID3D11VideoProcessorInputView> inView;
      check(video->CreateVideoProcessorInputView(texture, enumerator.Get(), &in, &inView), "the input view");
      D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC o = {};
      o.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
      ComPtr<ID3D11VideoProcessorOutputView> outView;
      check(video->CreateVideoProcessorOutputView(target.Get(), enumerator.Get(), &o, &outView), "the output view");
      D3D11_VIDEO_PROCESSOR_STREAM stream = {};
      stream.Enable = TRUE;
      stream.pInputSurface = inView.Get();
      Lock lock(hw);
      check(vcontext->VideoProcessorBlt(processor.Get(), outView.Get(), 0, 1, &stream), "VideoProcessorBlt");
      hw->device_context->CopyResource(s.staging.Get(), target.Get());
      hw->device_context->Flush();  // start now, not at the Map
    } else {
      ensureStaging(s, desc.Width, desc.Height, desc.Format);
      s.planeRows = desc.Height;
      Lock lock(hw);
      hw->device_context->CopySubresourceRegion(s.staging.Get(), 0, 0, 0, 0, texture, index, nullptr);
      hw->device_context->Flush();
    }
    s.pending = true;
    s.order = ++next;
    s.width = w;
    s.height = h;
    s.picture = Picture{format, timestamp, bgra};
    s.queued = std::chrono::steady_clock::now();
  }

  // Waits about 200 us. Sleep() waits whole timer ticks (up to 15.6 ms); this timer wakes within ~0.5 ms.
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  void nap() {
    LARGE_INTEGER due{};
    due.QuadPart = -2'000;  // 100 ns units; negative is relative
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
    } else {
      SwitchToThread();
    }
  }

  bool take(bool wait, Nv12& nv12, std::vector<uint8_t>& bgra, Picture& out) {
    // Pictures go out in order and none is skipped here: skipping stale frames is receiveVideo's job, before the
    // decode. Skipping here dropped every picture the copy left behind: 1080p60 received 48 fps on a GTX 1650.
    Slot* oldest = nullptr;
    Slot* newest = nullptr;
    for (Slot& s : slots) {
      if (!s.pending) continue;
      if (!oldest || s.order < oldest->order) oldest = &s;
      if (!newest || s.order > newest->order) newest = &s;
    }
    if (!oldest) return false;
    auto* ctx = hw->device_context;
    D3D11_MAPPED_SUBRESOURCE m = {};
    Slot* pick = oldest;
    // Without wait and one copy in flight: wait up to half a frame for it, then keep it for the next call.
    const VideoFormat& f = oldest->picture.format;
    const auto half = std::chrono::microseconds(f.fpsN > 0 && f.fpsD > 0 ? 500'000LL * f.fpsD / f.fpsN : 8'333);
    const bool mayKeep = oldest == newest && !wait;
    // Polled, never a blocking Map: that one holds the decoder's lock and, on a GTX 1650, took 17 ms for a 4K NV12
    // copy and up to 70 ms for BGRA (2026-11-04), against 1.3 and 3 ms of GPU work.
    HRESULT hr;
    for (;;) {
      {
        Lock lock(hw);
        hr = ctx->Map(oldest->staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
      }
      if (hr != DXGI_ERROR_WAS_STILL_DRAWING) break;
      if (mayKeep && std::chrono::steady_clock::now() - oldest->queued >= half) return false;
      nap();
    }
    Lock lock(hw);
    pick->pending = false;
    check(hr, "Map of the staging texture");
    const auto* p = static_cast<const uint8_t*>(m.pData);
    const int w = pick->width, h = pick->height;
    if (pick->picture.bgra) {
      bgra.resize(static_cast<size_t>(w) * h * 4);
      for (int r = 0; r < h; ++r)
        std::memcpy(bgra.data() + static_cast<size_t>(r) * w * 4, p + static_cast<size_t>(r) * m.RowPitch,
                    static_cast<size_t>(w) * 4);
    } else {
      nv12.width = w;
      nv12.height = h;
      nv12.y.resize(static_cast<size_t>(w) * h);
      nv12.uv.resize(static_cast<size_t>(w) * h / 2);
      const uint8_t* uv = p + static_cast<size_t>(m.RowPitch) * pick->planeRows;
      for (int r = 0; r < h; ++r)
        std::memcpy(nv12.y.data() + static_cast<size_t>(r) * w, p + static_cast<size_t>(r) * m.RowPitch,
                    static_cast<size_t>(w));
      for (int r = 0; r < h / 2; ++r)
        std::memcpy(nv12.uv.data() + static_cast<size_t>(r) * w, uv + static_cast<size_t>(r) * m.RowPitch,
                    static_cast<size_t>(w));
    }
    ctx->Unmap(pick->staging.Get(), 0);
    out = pick->picture;
    return true;
  }
};

Readback::Readback() : d_(std::make_unique<Impl>()) {}
Readback::~Readback() = default;

bool Readback::accepts(const AVFrame* frame) {
  return frame && frame->format == AV_PIX_FMT_D3D11 && frame->hw_frames_ctx &&
         reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data)->sw_format == AV_PIX_FMT_NV12 &&
         frame->width % 2 == 0 && frame->height % 2 == 0;
}

void Readback::push(const AVFrame* frame, const VideoFormat& format, int64_t timestamp, bool bgra) {
  d_->push(frame, format, timestamp, bgra);
}

bool Readback::take(bool wait, Nv12& nv12, std::vector<uint8_t>& bgra, Picture& out) {
  return d_->take(wait, nv12, bgra, out);
}

bool Readback::pending() const {
  for (const auto& s : d_->slots)
    if (s.pending) return true;
  return false;
}

void Readback::nap() { d_->nap(); }

}  // namespace omthx

#else

namespace omthx {
struct BgraToNv12::Impl {};
BgraToNv12::BgraToNv12() = default;
BgraToNv12::~BgraToNv12() = default;
void BgraToNv12::convert(void*, int, int, GpuPicture&) {
  throw std::runtime_error("D3D11 textures are Windows only");
}
struct UyvyToNv12::Impl {};
UyvyToNv12::UyvyToNv12() = default;
UyvyToNv12::~UyvyToNv12() = default;
void UyvyToNv12::convert(const uint8_t*, int, int, int, GpuPicture&) {
  throw std::runtime_error("D3D11 textures are Windows only");
}
struct Readback::Impl {};
Readback::Readback() = default;
Readback::~Readback() = default;
bool Readback::accepts(const AVFrame*) { return false; }
void Readback::push(const AVFrame*, const VideoFormat&, int64_t, bool) {
  throw std::runtime_error("D3D11 readback is Windows only");
}
bool Readback::take(bool, Nv12&, std::vector<uint8_t>&, Picture&) { return false; }
void Readback::nap() {}
bool Readback::pending() const { return false; }
}  // namespace omthx

#endif
