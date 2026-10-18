// Proves the libomtnet fork's change: HEVC- and H264-tagged frames pass through libomt untouched, and plain
// OMT is unchanged.
#include <doctest/doctest.h>

#include <chrono>
#include <cstring>
#include <vector>

#include <libomt.h>

namespace {
constexpr int kHevc = 0x43564548;
constexpr int kH264 = 0x34363248;
constexpr char kAsk[] = "<OMTHX Keyframe=\"true\" />";
}  // namespace

TEST_CASE("libomt: HEVC and H264 frames pass through and plain OMT still works") {
  omt_send_t* tx = omt_send_create("omthx-test-libomt", OMTQuality_Default);
  REQUIRE(tx != nullptr);
  char address[OMT_MAX_STRING_LENGTH] = {};
  omt_send_getaddress(tx, address, sizeof(address));
  omt_receive_t* rx =
      omt_receive_create(address, static_cast<OMTFrameType>(OMTFrameType_Video | OMTFrameType_Metadata),
                         OMTPreferredVideoFormat_UYVYorBGRA, OMTReceiveFlags_None);
  REQUIRE(rx != nullptr);

  std::vector<uint8_t> payload(1000);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 7);
  std::vector<uint8_t> uyvy(1280 * 720 * 2, 128);
  const int alphaFlags = OMTVideoFlags_Alpha | OMTVideoFlags_PreMultiplied;

  bool hevc = false, h264 = false, plain = false, asked = false, tally = false;
  int64_t timestamp = 1;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline && !(hevc && h264 && plain && asked && tally)) {
    OMTMediaFrame f = {};
    f.Type = OMTFrameType_Video;
    f.Timestamp = timestamp;
    timestamp += 166667;
    f.Width = 1280;
    f.Height = 720;
    f.FrameRateN = 60;
    f.FrameRateD = 1;
    f.AspectRatio = 16.0f / 9.0f;
    f.ColorSpace = OMTColorSpace_BT709;
    if (!hevc || !h264) {  // first HEVC until it arrives, then H264 until it arrives
      f.Codec = static_cast<OMTCodec>(hevc ? kH264 : kHevc);
      f.Flags = static_cast<OMTVideoFlags>(alphaFlags);
      f.Data = payload.data();
      f.DataLength = static_cast<int>(payload.size());
    } else {
      f.Codec = OMTCodec_UYVY;
      f.Stride = 1280 * 2;
      f.Data = uyvy.data();
      f.DataLength = static_cast<int>(uyvy.size());
    }
    omt_send(tx, &f);

    if (OMTMediaFrame* r = omt_receive(rx, OMTFrameType_Video, 20)) {
      const int codec = static_cast<int>(r->Codec);
      if (codec == kHevc || codec == kH264) {
        const bool intact = r->Data == nullptr && r->CompressedLength == static_cast<int>(payload.size()) &&
                            std::memcmp(r->CompressedData, payload.data(), payload.size()) == 0 && r->Width == 1280 &&
                            r->Height == 720 && static_cast<int>(r->Flags) == alphaFlags;
        if (codec == kHevc) hevc = intact;
        else h264 = intact;
        OMTTally program = {0, 1};
        omt_receive_settally(rx, &program);
        OMTMediaFrame m = {};
        m.Type = OMTFrameType_Metadata;
        m.Data = const_cast<char*>(kAsk);
        m.DataLength = sizeof(kAsk);
        omt_receive_send(rx, &m);
      } else if (r->Codec == OMTCodec_UYVY) {
        plain = r->DataLength == 1280 * 720 * 2;
      }
    }
    while (OMTMediaFrame* m = omt_send_receive(tx, 0)) {
      if (m->Data && std::strstr(static_cast<const char*>(m->Data), "<OMTHX Keyframe")) asked = true;
    }
    OMTTally t = {};
    omt_send_gettally(tx, 0, &t);
    if (t.program == 1) tally = true;
  }
  omt_receive_destroy(rx);
  omt_send_destroy(tx);

  CHECK(hevc);   // the patch, first tag
  CHECK(h264);   // the patch, second tag
  CHECK(plain);  // VMX in, UYVY out, as before
  CHECK(asked);  // a receiver's message reaches the sender
  CHECK(tally);
}