#pragma once
// OMT-HX: one sender and one receiver for plain OMT (VMX) and OMT-HX (HEVC or H.264 inside OMT).
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

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

// One OMT source. sendVideo and sendAudio may run on two threads, but each must stay on its own.
// requestKeyframe, tally and stats are safe from any thread.
class Sender {
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

struct VideoFrame {
  VideoFormat format;
  bool premultiplied = false;
  int64_t timestamp = 0;
  std::vector<uint8_t> bgra;  // width * height * 4
};

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
  std::string decoder;           // "vmx", "hevc (d3d11va)", "h264 (software)", ...
  uint64_t framesReceived = 0;   // pictures handed to the caller
  uint64_t framesDiscarded = 0;  // Hx frames dropped while waiting for a keyframe
  uint64_t bytesReceived = 0;    // wire bytes
  double lastDecodeMs = 0;       // Hx only: decode and unpack of the last frame
};

// Reads one OMT source in either mode. receiveVideo and receiveAudio may run on two threads,
// but each must stay on its own.
class Receiver {
 public:
  explicit Receiver(const std::string& address);
  ~Receiver();
  Receiver(const Receiver&) = delete;
  Receiver& operator=(const Receiver&) = delete;

  // False means no picture yet: a timeout, or a frame discarded while waiting for a keyframe. Call again.
  bool receiveVideo(VideoFrame& out, int timeoutMs);
  bool receiveAudio(AudioFrame& out, int timeoutMs);
  void setTally(Tally tally);
  ReceiverStats stats();

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

// Names of the OMT sources seen on the network so far. Discovery needs a second or two after start.
std::vector<std::string> discover();

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
bool probe(const std::string& address, int timeoutMs, SourceInfo& out);

}  // namespace omthx
