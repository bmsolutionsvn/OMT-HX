// BGRA or RGBA texture -> NV12 D3D11 frame on the GPU, so a captured picture reaches the encoder without leaving video memory.
#include "codec.h"

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
    default: return "an unknown format";
  }
}
}  // namespace

struct BgraToNv12::Impl {
  ID3D11Device* device = nullptr;  // not owned: identifies the device the objects below belong to
  int width = 0, height = 0;
  ComPtr<ID3D11VideoDevice> video;
  ComPtr<ID3D11VideoContext> context;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
  ComPtr<ID3D11VideoProcessor> processor;
  unsigned formatsChecked = 0;  // a bit per kTextureFormats entry the enumerator has accepted
  AVBufferRef* hwDevice = nullptr;
  AVBufferRef* frames = nullptr;

  ~Impl() { reset(); }

  void reset() {
    av_buffer_unref(&frames);
    av_buffer_unref(&hwDevice);
    processor.Reset();
    enumerator.Reset();
    context.Reset();
    video.Reset();
    formatsChecked = 0;  // the enumerator went with them
    device = nullptr;
  }

  // The video processor and the NV12 frame pool for this device and size, made once.
  void setup(ID3D11Device* dev, int w, int h) {
    if (dev == device && w == width && h == height) return;
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

    // Full-range BT.709 both sides, the matrix and range the encoder signals.
    ComPtr<ID3D11VideoContext1> context1;
    if (SUCCEEDED(context.As(&context1))) {
      context1->VideoProcessorSetStreamColorSpace1(processor.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
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
  }

  // Whether the video processor can read this texture format, asked once per format and remembered.
  void checkInputFormat(DXGI_FORMAT format) {
    const int index = textureFormatIndex(format);
    if (formatsChecked & (1u << index)) return;
    UINT support = 0;
    check(enumerator->CheckVideoProcessorFormat(format, &support), "CheckVideoProcessorFormat");
    if (!(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
      throw std::runtime_error(std::string("the video processor cannot read ") + formatName(format));
    formatsChecked |= 1u << index;
  }

  void convert(ID3D11Texture2D* texture, int w, int h, AVFrame* out) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    if (textureFormatIndex(desc.Format) < 0 || desc.Width < static_cast<UINT>(w) ||
        desc.Height < static_cast<UINT>(h))
      throw std::invalid_argument("the texture must be BGRA or RGBA and at least the picture's size");
    ComPtr<ID3D11Device> dev;
    texture->GetDevice(&dev);
    setup(dev.Get(), w, h);
    checkInputFormat(desc.Format);

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

BgraToNv12::BgraToNv12() : d_(std::make_unique<Impl>()) {}
BgraToNv12::~BgraToNv12() = default;

void BgraToNv12::convert(void* texture, int width, int height, GpuPicture& out) {
  av_frame_unref(out.frame);
  d_->convert(static_cast<ID3D11Texture2D*>(texture), width, height, out.frame);
}

}  // namespace omthx

#else

namespace omthx {
struct BgraToNv12::Impl {};
BgraToNv12::BgraToNv12() = default;
BgraToNv12::~BgraToNv12() = default;
void BgraToNv12::convert(void*, int, int, GpuPicture&) {
  throw std::runtime_error("D3D11 textures are Windows only");
}
}  // namespace omthx

#endif
