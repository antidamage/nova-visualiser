// Browser rung transport: publish the H.264 elementary stream to a MediaMTX
// sidecar over RTSP, which republishes it as WebRTC/WHEP and LL-HLS.
//
// Browsers cannot consume the Apple TV's bespoke Annex-B feed, and writing an
// SDP/ICE/DTLS/SRTP stack by hand for a household visualiser is not a good use
// of anyone's time. MediaMTX is a single static Go binary; if it is down or
// absent this publisher simply reports unavailable and the Apple TV path is
// completely unaffected.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "encode/encoder.h"

struct AVFormatContext;
struct AVStream;
struct AVPacket;

namespace nova::encode {

class RtspPublisher {
 public:
  RtspPublisher() = default;
  ~RtspPublisher();

  RtspPublisher(const RtspPublisher&) = delete;
  RtspPublisher& operator=(const RtspPublisher&) = delete;

  // `url` is typically rtsp://127.0.0.1:8554/visualiser.
  bool open(const std::string& url, const EncoderSettings& settings,
            const std::vector<uint8_t>& parameterSets, std::string& error);
  void close();

  bool connected() const { return format_ != nullptr; }
  const std::string& url() const { return url_; }

  bool write(const EncodedFrame& frame, std::string& error);

 private:
  std::string url_;
  AVFormatContext* format_ = nullptr;
  AVStream* stream_ = nullptr;
  AVPacket* packet_ = nullptr;
  int frameRate_ = 60;
};

}  // namespace nova::encode
