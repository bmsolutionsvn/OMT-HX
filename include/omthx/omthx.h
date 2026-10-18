#pragma once
// OMT-HX: one sender and one receiver for plain OMT (VMX) and OMT-HX (HEVC or H.264 inside OMT).
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

// omthx.dll by default, as in the binary release. Building omthx from source into your program, define
// OMTHX_STATIC (OMT-HX's CMake target omthx does it for you).
#if defined(OMTHX_STATIC)
#define OMTHX_API
#elif defined(OMTHX_BUILD_DLL)
#define OMTHX_API __declspec(dllexport)
#else
#define OMTHX_API __declspec(dllimport)
#endif
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)  // std:: members of exported classes: callers use the same MSVC runtime
#endif

namespace omthx {

enum class Mode { Full, Hx };  // Full = plain OMT with VMX; Hx = HEVC or H.264 inside OMT
enum class Quality { Low, Medium, High };
enum class Codec { Hevc, H264 };  // the video codec of an Hx source

struct VideoFormat {
  int width = 0;  // the picture as displayed
  int height = 0;
  int fpsN = 60;
  int fpsD = 1;
  bool alpha = false;
  bool operator==(const VideoFormat&) const = default;
};

struct Tally {
  bool preview = false;
  bool program = false;
  bool operator==(const Tally&) const = default;
};

struct SenderStats {
  std::string encoder;         // "vmx", "hevc_amf", "h264_nvenc", ...
  uint64_t framesSent = 0;
  uint64_t keyframes = 0;      // Hx only
  uint64_t bytesSent = 0;      // wire bytes, summed over all receivers
  uint64_t framesDropped = 0;  // a receiver was too slow to take a frame
  double lastEncodeMs = 0;     // Hx only: pack and encode of the last frame
  int connections = 0;
};

// What a Receiver hands back.
// Bgra and Uyvy: decoded pictures in memory. Uyvy keeps the 4:2:2 picture VMX and NDI carry, for an app that passes
// video on. Gpu: an HX picture stays in video memory (VideoFrame::gpu) and a plain OMT picture arrives as Uyvy;
// Sender::sendVideo(frame) re-codes it without a copy to memory. Coded: nothing is decoded; read the frames as the
// source sent them with receiveCoded, and send them on with Sender::sendCoded.
enum class PixelFormat { Bgra, Uyvy, Gpu, Coded };

struct GpuPicture;  // a decoded HX picture in video memory, alpha packed as on the wire; opaque

struct VideoFrame {
  VideoFormat format;
  bool premultiplied = false;
  int64_t timestamp = 0;
  std::vector<uint8_t> bgra;  // Bgra receivers: width * height * 4
  // Uyvy receivers, and plain OMT on Gpu receivers: UYVY (BT.709, 16-235), rows of width * 2 bytes, then with
  // alpha the width * height alpha plane: OMT's and NDI's UYVA layout.
  std::vector<uint8_t> uyvy;
  // Gpu receivers, HX sources: the picture, set; otherwise null. Hold only a few at a time: they are the
  // decoder's own surfaces.
  std::shared_ptr<const GpuPicture> gpu;
};

// One coded video frame as the source sent it, for an app that republishes a source without re-coding it.
struct CodedFrame {
  Mode mode = Mode::Full;     // Full: one VMX frame; Hx: one HEVC or H.264 access unit
  Codec codec = Codec::Hevc;  // Hx only
  VideoFormat format;
  bool keyframe = true;       // VMX frames are all keyframes
  uint64_t seq = 0;           // Hx: the frame's number; the next Sender passes it on, so a gap stays visible
  int64_t timestamp = 0;
  int flags = 0;              // OMT's video flags and colour space, as they came: pass them on untouched
  int colorSpace = 0;
  std::vector<uint8_t> data;
};

// One OMT source. sendVideo and sendAudio may run on two threads, but each must stay on its own.
// requestKeyframe, keyframeRequested, tally and stats are safe from any thread.
class OMTHX_API Sender {
 public:
  // codec picks the Hx video codec and is ignored in Mode::Full. The receiver needs no setting.
  Sender(const std::string& name, Mode mode, Quality quality, Codec codec = Codec::Hevc);
  ~Sender();
  Sender(const Sender&) = delete;
  Sender& operator=(const Sender&) = delete;

  // bgra: width * height * 4 bytes, top row first. premultiplied says how the colour relates to its alpha.
  // timestamp: OMT units, 10,000,000 per second. A frame the app skips leaves a larger step, which is fine:
  // the receiver keeps playing, as the coded stream has no hole.
  void sendVideo(const VideoFormat& format, std::span<const uint8_t> bgra, bool premultiplied, int64_t timestamp);
  // Hx without alpha only: an NV12 picture (BT.709) goes to the encoder with no colour conversion, the
  // cheap path for an app that already has NV12. Strides are in bytes; fullRange false means 16-235.
  void sendVideoNv12(const VideoFormat& format, const uint8_t* y, int yStride, const uint8_t* uv, int uvStride,
                     bool fullRange, int64_t timestamp);
  // Either mode: 8-bit 4:2:2 UYVY (BT.709, 16-235), even width, rows `stride` bytes apart. With format.alpha,
  // `alpha` is a width * height plane, rows `alphaStride` bytes apart, full range; otherwise it is ignored.
  // Full hands it to VMX as it is (no conversion; zero copy when the alpha plane follows the UYVY plane with
  // stride / 2, as in OMT's and NDI's UYVA). Hx makes the NV12 picture without a colour matrix.
  void sendVideoUyvy(const VideoFormat& format, const uint8_t* uyvy, int stride, const uint8_t* alpha,
                     int alphaStride, bool premultiplied, int64_t timestamp);
  // Sends what a Receiver handed back, whichever picture it holds, at frame.timestamp. A Gpu picture to an Hx
  // sender is encoded where it is, in video memory, when the GPU encoder takes it, and through memory otherwise.
  void sendVideo(const VideoFrame& frame);
  // Sends a coded frame on untouched, whatever this sender's mode (no encoder runs). Feed a sender either coded
  // frames or pictures, not both. Keyframe requests from this sender's receivers show in keyframeRequested.
  void sendCoded(const CodedFrame& frame);
  // True once after a receiver of this source asked for a keyframe, or lost a frame on its link: a relay passes
  // it to its own source with Receiver::requestKeyframe. Only sendCoded collects them.
  bool keyframeRequested();
  // planar: all samples of channel 0, then all of channel 1, and so on.
  void sendAudio(std::span<const float> planar, int sampleRate, int channels, int64_t timestamp);
  void requestKeyframe();  // the next frame is coded as a keyframe (at most one per 500 ms)
  Tally tally();
  SenderStats stats();
  std::string address() const;  // "HOSTNAME (name)", the string a Receiver connects to

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

// Fills frame.bgra or frame.uyvy (pixels Bgra or Uyvy) from frame.gpu, for a Gpu receiver's app that also
// needs the picture in memory. False when frame.gpu is null or the copy fails.
OMTHX_API bool download(VideoFrame& frame, PixelFormat pixels);

struct AudioFrame {
  int sampleRate = 0;
  int channels = 0;
  int samples = 0;  // per channel
  int64_t timestamp = 0;
  std::vector<float> planar;
};

struct ReceiverStats {
  Mode mode = Mode::Full;        // what the source is sending
  Codec codec = Codec::Hevc;     // Hx only: which codec the source is sending
  std::string decoder;           // "vmx", "hevc (d3d11va)", "h264 (software)", "none" (Coded), ...
  uint64_t framesReceived = 0;   // pictures or coded frames handed to the caller
  uint64_t framesDiscarded = 0;  // Hx frames dropped while waiting for a keyframe
  uint64_t bytesReceived = 0;    // wire bytes
  double lastDecodeMs = 0;       // Hx only: decode and unpack of the last frame
};

// Reads one OMT source in either mode. receiveVideo (or receiveCoded) and receiveAudio may run on two threads,
// but each must stay on its own. requestKeyframe is safe from any thread.
class OMTHX_API Receiver {
 public:
  explicit Receiver(const std::string& address, PixelFormat pixels = PixelFormat::Bgra);
  ~Receiver();
  Receiver(const Receiver&) = delete;
  Receiver& operator=(const Receiver&) = delete;

  // False means no picture yet: a timeout, or a frame discarded while waiting for a keyframe. Call again.
  // Coded receivers: always false; use receiveCoded.
  bool receiveVideo(VideoFrame& out, int timeoutMs);
  // Coded receivers only. HX frames start at a keyframe and, after a lost frame, wait for the next one, as
  // receiveVideo does; so the frames handed back always decode.
  bool receiveCoded(CodedFrame& out, int timeoutMs);
  bool receiveAudio(AudioFrame& out, int timeoutMs);
  void requestKeyframe();  // asks an Hx source for a keyframe, at most once per 500 ms
  void setTally(Tally tally);
  ReceiverStats stats();

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

// Names of the OMT sources seen on the network so far. Discovery needs a second or two after start.
OMTHX_API std::vector<std::string> discover();

// What a source sends, read from the header of one of its video frames.
struct SourceInfo {
  Mode mode = Mode::Full;
  Codec codec = Codec::Hevc;  // Hx only
  VideoFormat format;          // the picture at its full size
};

// Connects to one source for at most timeoutMs, reads the header of its first video frame, and disconnects.
// Nothing is decoded and no keyframe is asked for: a plain OMT sender sends this receiver only its small
// preview, an Hx sender its stream as it is. The sender's tally is not touched.
// False when no picture arrived in time. libomt drops a frame in a format it does not know, so a source
// sending one also gives false.
OMTHX_API bool probe(const std::string& address, int timeoutMs, SourceInfo& out);

}  // namespace omthx

#ifdef _MSC_VER
#pragma warning(pop)
#endif
