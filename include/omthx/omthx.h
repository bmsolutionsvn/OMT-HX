#pragma once
// OMT-HX: one sender and one receiver for plain OMT (VMX) and OMT-HX (HEVC inside OMT).
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace omthx {

enum class Mode { Full, Hx };  // Full = plain OMT with VMX; Hx = HEVC inside OMT
enum class Quality { Low, Medium, High };

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
  std::string encoder;         // "vmx", "hevc_amf", "hevc_nvenc", ...
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
  Sender(const std::string& name, Mode mode, Quality quality);
  ~Sender();
  Sender(const Sender&) = delete;
  Sender& operator=(const Sender&) = delete;

  // bgra: width * height * 4 bytes, top row first. premultiplied says how the colour relates to its alpha.
  // timestamp: OMT units, 10,000,000 per second, one frame duration apart from the previous frame.
  void sendVideo(const VideoFormat& format, std::span<const uint8_t> bgra, bool premultiplied, int64_t timestamp);
  // planar: all samples of channel 0, then all of channel 1, and so on.
  void sendAudio(std::span<const float> planar, int sampleRate, int channels, int64_t timestamp);
  void requestKeyframe();  // call when the app skipped a frame
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
  std::string decoder;           // "vmx", "hevc (d3d11va)", "hevc (software)"
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

}  // namespace omthx
