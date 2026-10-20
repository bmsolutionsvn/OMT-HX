// Alpha as one grey VMX plane, coded on the CPU, so the GPU codes only the fill.
#include "codec.h"

#include <stdexcept>
#include <string>

#include <vmxcodec.h>

namespace omthx {

// Fixed quality, no bitrate targeting. Below about 50 a frame gets hardly smaller (the floor is the cost of the
// flat blocks), while edges ring more; measured on a 1080p matte: about 14 to 25 KB a frame.
int AlphaEncoder::vmxQuality(Quality q) {
  static constexpr int kQuality[] = {52, 70, 90};
  return kQuality[static_cast<int>(q)];
}

AlphaEncoder::AlphaEncoder(int w, int h, Quality q) : width_(w), height_(h) {
  vmx_ = VMX_Create(VMX_SIZE{w, h}, VMX_PROFILE_OMT_LQ, VMX_COLORSPACE_BT709);
  if (!vmx_) throw std::runtime_error("VMX cannot code a " + std::to_string(w) + "x" + std::to_string(h) + " alpha plane");
  // Targets of 0 turn VMX's bitrate control off; DC shift 3 keeps 8-bit DC, all an 8-bit plane needs.
  VMX_SetEncodingParameters(vmx_, 0, 0, vmxQuality(q), 3);
  VMX_SetQuality(vmx_, vmxQuality(q));
}

AlphaEncoder::~AlphaEncoder() { VMX_Destroy(vmx_); }

void AlphaEncoder::encode(const uint8_t* plane, int stride, std::vector<uint8_t>& out) {
  if (VMX_EncodeY8(vmx_, const_cast<uint8_t*>(plane), stride) != VMX_ERR_OK)
    throw std::runtime_error("the VMX alpha encode failed");
  out.resize(static_cast<size_t>(width_) * height_ * 2 + 65536);  // far more than VMX writes for 8 bits a pixel
  const int len = VMX_SaveTo(vmx_, out.data(), static_cast<int>(out.size()));
  if (len <= 8) throw std::runtime_error("the VMX alpha encode failed");
  out.resize(static_cast<size_t>(len));
}

AlphaDecoder::~AlphaDecoder() { VMX_Destroy(vmx_); }

bool AlphaDecoder::decode(std::span<const uint8_t> data, int w, int h, std::vector<uint8_t>& out) {
  if (data.empty() || data.size() > 0x7FFFFFFF) return false;
  if (!vmx_ || w != width_ || h != height_) {
    VMX_Destroy(vmx_);
    vmx_ = VMX_Create(VMX_SIZE{w, h}, VMX_PROFILE_OMT_LQ, VMX_COLORSPACE_BT709);
    width_ = vmx_ ? w : 0;
    height_ = vmx_ ? h : 0;
    if (!vmx_) return false;
  }
  if (VMX_LoadFrom(vmx_, const_cast<uint8_t*>(data.data()), static_cast<int>(data.size())) != VMX_ERR_OK) return false;
  out.resize(static_cast<size_t>(w) * h);
  return VMX_DecodeY8(vmx_, out.data(), w) == VMX_ERR_OK;
}

}  // namespace omthx
