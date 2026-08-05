// Low-latency video transport for the Apple TV thin client.
//
// Why not HLS: even low-latency HLS adds segmenting, a playlist round trip and
// AVPlayer's own buffering -- seconds of latency for a visualiser that has to
// look like it is reacting to the music playing in the room. This feeds
// `AVSampleBufferDisplayLayer` directly, which is hardware decode with roughly
// one frame of pipeline and no player in the way.
//
// Wire format (server -> client), all integers big-endian:
//
//   Handshake, once per connection:
//     "NOVAVIS1"                       8 bytes magic
//     uint32 headerLength
//     headerLength bytes of JSON       codec, size, fps, profile, parameter sets
//
//   Then repeating:
//     uint8  type        0 video access unit, 1 heartbeat
//     uint8  flags       bit0 keyframe
//     uint16 reserved
//     uint32 payloadLength
//     uint64 ptsMicroseconds           presentation time on the server clock
//     uint64 sentAtMicroseconds        server monotonic send time
//     payload                          Annex-B access unit
//
// Client -> server, after the handshake. Every message is length-prefixed and
// the body is always consumed, so an unknown type is skipped rather than
// desynchronising the reader:
//     uint8  type        0x80 latency report, 0x81 keyframe request
//     uint32 length
//     length bytes of JSON             presentationDelayMs, decoded, dropped
//
// The latency report closes the render-ahead loop: audio plays on the Apple TV
// while video takes an encode + network + decode path, so the renderer must
// draw the state the room will hear when the frame is actually presented.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "encode/encoder.h"

namespace nova::net {

struct StreamHeader {
  std::string codec = "hevc";
  std::string profile = "main10";
  int width = 3840;
  int height = 2160;
  int frameRate = 60;
  std::vector<uint8_t> parameterSets;
};

struct ClientTelemetry {
  double presentationDelayMs = 0;
  int64_t decodedFrames = 0;
  int64_t droppedFrames = 0;
  bool valid = false;
};

class StreamServer {
 public:
  // Raised whenever a client needs a stream access point: on connect, when the
  // send queue overflowed and the client was dropped to the next IDR, and when
  // a client asks explicitly after a decode failure. The callee is expected to
  // rate-limit; a client flapping on bad WiFi must not turn the stream into an
  // unbroken run of IDRs.
  using KeyframeRequest = std::function<void()>;

  StreamServer() = default;
  ~StreamServer();

  StreamServer(const StreamServer&) = delete;
  StreamServer& operator=(const StreamServer&) = delete;

  bool start(int port, StreamHeader header, KeyframeRequest requestKeyframe, std::string& error);
  void stop();

  // Replaces the advertised header (after a resolution or codec change).
  void updateHeader(StreamHeader header);

  void broadcast(const encode::EncodedFrame& frame, int64_t ptsMicroseconds);

  int clientCount() const { return clientCount_.load(std::memory_order_relaxed); }

  // Smoothed presentation delay across connected clients, in seconds.
  double presentationDelaySeconds() const;

 private:
  struct Client;

  void acceptLoop();
  void serviceClient(std::shared_ptr<Client> client);

  int listenSocket_ = -1;
  int port_ = 0;
  std::atomic<bool> running_{false};
  std::thread acceptThread_;

  mutable std::mutex mutex_;
  StreamHeader header_;
  std::vector<std::shared_ptr<Client>> clients_;
  KeyframeRequest requestKeyframe_;

  std::atomic<int> clientCount_{0};
  std::atomic<double> presentationDelaySeconds_{0.0};
};

}  // namespace nova::net
