#pragma once
// A flat C API over omthx::Sender and omthx::Receiver, for a host program that cannot call C++ (the OBS plugin).
// One sender or receiver is one OMT source, in either mode, with video, audio and tally.
#include <stdint.h>

#if defined(OMTHX_STATIC)
#define OMTHX_C_API
#elif defined(OMTHX_BUILD_DLL)
#if defined(_WIN32)
#define OMTHX_C_API __declspec(dllexport)
#else
#define OMTHX_C_API __attribute__((visibility("default")))
#endif
#else
#if defined(_WIN32)
#define OMTHX_C_API __declspec(dllimport)
#else
#define OMTHX_C_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum { OMTHX_C_MODE_FULL = 0, OMTHX_C_MODE_HX = 1 };
enum { OMTHX_C_QUALITY_LOW = 0, OMTHX_C_QUALITY_MEDIUM = 1, OMTHX_C_QUALITY_HIGH = 2 };
enum { OMTHX_C_CODEC_HEVC = 0, OMTHX_C_CODEC_H264 = 1 };
enum { OMTHX_C_PIXEL_BGRA = 0, OMTHX_C_PIXEL_UYVY = 1 };

typedef struct omthx_c_sender omthx_c_sender;
typedef struct omthx_c_receiver omthx_c_receiver;

typedef struct omthx_c_video_format {
  int width;
  int height;
  int fps_n;
  int fps_d;
  int alpha;
} omthx_c_video_format;

// Why the last call returned 0. The string belongs to the library and is valid until the next call on this thread.
OMTHX_C_API const char* omthx_c_error(void);

// name: the OMT source name. codec is ignored in Full mode. Null when the source cannot be created.
OMTHX_C_API omthx_c_sender* omthx_c_sender_open(const char* name, int mode, int quality, int codec);
OMTHX_C_API void omthx_c_sender_close(omthx_c_sender* sender);
// BGRA (a stride other than width * 4 is copied) or UYVY. 1 on success, 0 with omthx_c_error() set.
OMTHX_C_API int omthx_c_sender_video(omthx_c_sender* sender, const omthx_c_video_format* format, int pixel_format,
                                     const uint8_t* data, int stride, int premultiplied, int64_t timestamp);
// An NV12 picture without a colour conversion: the cheap path for an output that already has NV12.
OMTHX_C_API int omthx_c_sender_video_nv12(omthx_c_sender* sender, const omthx_c_video_format* format,
                                          const uint8_t* y, int y_stride, const uint8_t* uv, int uv_stride,
                                          int full_range, int64_t timestamp);
// planar: all samples of channel 0, then channel 1, as OBS hands audio out.
OMTHX_C_API int omthx_c_sender_audio(omthx_c_sender* sender, const float* planar, int sample_rate, int channels,
                                     int samples_per_channel, int64_t timestamp);
// The next coded frame is a keyframe (at most one per 500 ms).
OMTHX_C_API void omthx_c_sender_keyframe(omthx_c_sender* sender);
// "HOSTNAME (name)", the string a receiver connects to.
OMTHX_C_API const char* omthx_c_sender_address(omthx_c_sender* sender);

// Counters for the stats panel. Port and subscriber counts are not in omthx's sender stats.
typedef struct omthx_c_sender_counters {
  uint64_t frames_sent;
  uint64_t keyframes;
  uint64_t bytes_sent;
  uint64_t frames_dropped;
  double encode_ms;  // Hx only: pack and encode of the last frame
  int connections;
} omthx_c_sender_counters;

typedef struct omthx_c_receiver_counters {
  int mode;  // OMTHX_C_MODE_*
  int codec;
  uint64_t frames_received;
  uint64_t frames_discarded;
  uint64_t bytes_received;
  double decode_ms;  // Hx only: decode and unpack of the last frame
} omthx_c_receiver_counters;

OMTHX_C_API int omthx_c_sender_get_counters(omthx_c_sender* sender, omthx_c_sender_counters* out);
OMTHX_C_API int omthx_c_receiver_get_counters(omthx_c_receiver* receiver, omthx_c_receiver_counters* out);

// What a source sends, read from the header of one of its video frames, without decoding.
// 1 when a picture arrived, 0 on timeout; format/mode/codec are written only on 1.
OMTHX_C_API int omthx_c_probe(const char* address, int timeout_ms, omthx_c_video_format* format, int* mode, int* codec);

// Reads one source in either mode. Pictures come back BGRA, premultiplied when the format has alpha.
// `preview` asks for the small multiview stream (NDI's lowest bandwidth): plain OMT's VMX preview,
// or an Hx source's second ~640-wide stream.
OMTHX_C_API omthx_c_receiver* omthx_c_receiver_open(const char* address, int preview);
OMTHX_C_API void omthx_c_receiver_close(omthx_c_receiver* receiver);
// 1 with a picture, 0 on timeout or a frame still waiting for a keyframe. `bgra` is valid until the next call.
OMTHX_C_API int omthx_c_receiver_video(omthx_c_receiver* receiver, int timeout_ms, omthx_c_video_format* format,
                                       int* premultiplied, const uint8_t** bgra, int64_t* timestamp);
// 1 with a block, 0 on timeout. `planar` is valid until the next call.
OMTHX_C_API int omthx_c_receiver_audio(omthx_c_receiver* receiver, int timeout_ms, const float** planar,
                                       int* sample_rate, int* channels, int* samples_per_channel, int64_t* timestamp);
OMTHX_C_API void omthx_c_receiver_tally(omthx_c_receiver* receiver, int preview, int program);
OMTHX_C_API void omthx_c_receiver_keyframe(omthx_c_receiver* receiver);
// What the source is sending: OMTHX_C_MODE_* and OMTHX_C_CODEC_*.
OMTHX_C_API int omthx_c_receiver_mode(omthx_c_receiver* receiver);
OMTHX_C_API int omthx_c_receiver_codec(omthx_c_receiver* receiver);
// "hevc (d3d11va)", "vmx", ... for a log line.
OMTHX_C_API const char* omthx_c_receiver_decoder(omthx_c_receiver* receiver);

#ifdef __cplusplus
}
#endif