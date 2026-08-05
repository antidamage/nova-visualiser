// NVENC encode rung.
//
// One instance per output: the Apple TV takes 4K60 HEVC Main10, browsers take
// 1080p60 H.264 High. Both are driven from the same rendered frame, so there is
// exactly one simulation and one render regardless of how many clients watch.
//
// Turing NVENC has no AV1 encoder and the Apple TV 4K (AppleTV6,2, A10X) has no
// AV1 decoder, so HEVC is the ceiling for the high rung and not a compromise.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVBufferRef;
struct AVFrame;
struct AVPacket;
typedef struct CUctx_st* CUcontext;

namespace nova::encode {

enum class Codec { Hevc, H264 };

struct EncoderSettings {
  Codec codec = Codec::Hevc;
  int width = 3840;
  int height = 2160;
  int frameRate = 60;
  int64_t bitRate = 30'000'000;
  // Seconds between IDRs. Short enough that a reconnecting client recovers
  // quickly, long enough not to spend the bitrate on keyframes.
  double keyframeIntervalSeconds = 2.0;
  bool tenBit = true;
  // Where the parameter sets live.
  //
  //   false — in-band: every IDR repeats VPS/SPS/PPS, so the elementary stream
  //           is self-describing and a client can join mid-stream. Required for
  //           the Apple TV rung, which is a raw Annex-B feed.
  //   true  — out-of-band in extradata, stripped from the bitstream. Required
  //           for the RTSP publisher, which needs them for the SDP.
  //
  // Setting the global-header flag does not merely *add* extradata, it removes
  // the parameter sets from the packets, so this genuinely is either/or.
  bool globalHeaderParameterSets = false;
};

struct EncodedFrame {
  std::vector<uint8_t> data;  // Annex-B
  int64_t pts = 0;
  bool keyframe = false;
};

class Encoder {
 public:
  Encoder() = default;
  ~Encoder();

  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;

  bool initialise(CUcontext cudaContext, const EncoderSettings& settings, std::string& error);
  void shutdown();

  const EncoderSettings& settings() const { return settings_; }

  // Plane geometry the renderer must match when it allocates its GL buffers.
  int lumaStrideBytes() const;
  int chromaStrideBytes() const;
  void* lumaPlane() const;
  void* chromaPlane() const;

  // Forces the next submitted frame to be an IDR. Used when a client connects
  // so it can start decoding immediately instead of waiting up to the GOP.
  void requestKeyframe() { keyframeRequested_ = true; }

  // Encodes whatever is currently in the reusable input frame.
  bool encode(int64_t pts, std::vector<EncodedFrame>& out, std::string& error);

  // The parameter sets, as Annex-B. tvOS needs these to build an hvcC/avcC
  // format description before it can decode anything.
  const std::vector<uint8_t>& parameterSets() const { return parameterSets_; }

 private:
  EncoderSettings settings_;
  AVCodecContext* context_ = nullptr;
  AVBufferRef* deviceRef_ = nullptr;
  AVBufferRef* framesRef_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  std::vector<uint8_t> parameterSets_;
  bool keyframeRequested_ = true;
};

}  // namespace nova::encode
