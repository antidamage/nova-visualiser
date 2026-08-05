// Real-time transport for the Apple TV thin client, over SRT.
//
// WHY NOT THE TCP RUNG
//
// `stream_server.h` carries the same payload over TCP, and TCP is the wrong
// tool for live video: a single lost packet blocks every frame behind it while
// the congestion window collapses, which is precisely what a flaky WiFi link
// produces. Nothing that does this well uses TCP -- Steam Remote Play runs over
// UDP via Valve's Game Networking Sockets, Moonlight/Sunshine uses RTP over UDP
// with Reed-Solomon FEC, WebRTC uses SRTP over UDP.
//
// SRT is the smallest change that fixes the *class* of problem rather than its
// symptoms, and it hands us at the transport layer everything the alternative
// was to hand-build:
//
//   * TSBPD (timestamp-based packet delivery) releases packets on their
//     original timing. That IS the jitter buffer, so the client no longer
//     receives no raw network bursts. The tvOS display layer adds one decoded
//     frame paced by access-unit PTS, so a 4K IDR burst cannot reach the panel
//     as a hitch.
//   * SRTO_LATENCY is an explicit budget. ARQ retransmits inside it and gives
//     up outside it, so head-of-line blocking is bounded rather than unbounded.
//   * ARQ recovers loss inside the 60 ms budget. The measured LAN RTT is under
//     a millisecond, so it has many opportunities without a costly FEC matrix.
//   * srt_bstats reports RTT, loss, retransmission and send-drop directly, so
//     adaptive bitrate runs on measurements instead of the fabricated
//     `presentationDelayMs` the TCP rung had to invent.
//
// WebRTC would be the more complete answer and would delete more code, but
// HEVC-over-WebRTC on tvOS specifically is unproven, and the Apple TV 4K reaches
// 4K60 only through its HEVC decoder. SRT keeps the payload and the whole
// VideoToolbox path byte-identical, so there is no codec risk at all. The
// household's first-generation Apple TV 4K drains this packetised path at 60
// Hz at 16 Mbps; 30 Mbps exceeded its SRT message cadence even though the
// hardware decoder itself supports 4K60.
//
// WIRE FORMAT
//
// Identical to the TCP rung -- the same `NOVAVIS1` handshake and the same
// 24-byte access-unit envelope -- with one addition. SRT live mode delivers
// messages, and a message must fit inside one packet (SRTO_PAYLOADSIZE, 1316
// bytes), so anything larger is fragmented with a 4-byte header of the same
// shape as RTP's FU-A:
//
//     uint16 sequence      access unit ordinal, wraps
//     uint8  flags         bit0 first fragment, bit1 last fragment
//     uint8  reserved
//     payload              up to kPayloadBytes - 4 bytes
//
// MPEG-TS is the conventional wrapper here, but we own both ends, so this
// avoids putting a TS demuxer on the Apple TV and keeps the per-frame metadata
// envelope that motion vectors will want later.
//
// The TCP rung stays. It is the fallback when SRT is unavailable, and the
// shipped tvOS build keeps using it until it is rebuilt.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "encode/encoder.h"
#include "net/stream_server.h"

namespace nova::net {

// Live transport statistics, straight from srt_bstats. These replace the
// client-reported telemetry the TCP rung relied on: the client had no way to
// measure most of it and estimated `presentationDelayMs` from a two-valued
// step function.
struct SrtTransportStats {
  bool valid = false;
  double rttMs = 0;
  double bandwidthMbps = 0;
  int64_t packetsLost = 0;
  int64_t packetsRetransmitted = 0;
  int64_t packetsDropped = 0;
  int64_t bytesSent = 0;
  int sendBufferMs = 0;
};

class SrtServer {
 public:
  using KeyframeRequest = std::function<void()>;

  SrtServer() = default;
  ~SrtServer();

  SrtServer(const SrtServer&) = delete;
  SrtServer& operator=(const SrtServer&) = delete;

  // `latencyMs` is the ARQ budget. On a LAN 40-60 ms leaves room for two or
  // three retransmission round trips while staying well inside the render-ahead
  // the engine already applies.
  bool start(int port, int latencyMs, StreamHeader header, KeyframeRequest requestKeyframe,
             std::string& error);
  void stop();

  bool available() const { return running_.load(std::memory_order_relaxed); }
  void updateHeader(StreamHeader header);
  void broadcast(const encode::EncodedFrame& frame, int64_t ptsMicroseconds);

  int clientCount() const { return clientCount_.load(std::memory_order_relaxed); }
  int latencyMs() const { return latencyMs_; }
  int64_t framesSent() const { return framesSent_.load(std::memory_order_relaxed); }
  int64_t framesDropped() const { return framesDropped_.load(std::memory_order_relaxed); }

  // Worst-case across connected clients, so the bitrate controller reacts to
  // the client having the hardest time rather than to an average that hides it.
  SrtTransportStats worstStats() const;

 private:
  struct Client;

  void acceptLoop();
  void pollStats();
  void serviceClient(std::shared_ptr<Client> client);
  bool sendHandshake(Client& client);
  bool sendAccessUnit(Client& client, const std::vector<uint8_t>& unit, uint16_t sequence);

  int listenSocket_ = -1;
  int port_ = 0;
  int latencyMs_ = 60;
  std::atomic<bool> running_{false};
  std::thread acceptThread_;
  std::thread statsThread_;

  mutable std::mutex mutex_;
  StreamHeader header_;
  std::vector<std::shared_ptr<Client>> clients_;
  KeyframeRequest requestKeyframe_;
  SrtTransportStats worstStats_;

  std::atomic<int> clientCount_{0};
  std::atomic<int64_t> framesSent_{0};
  std::atomic<int64_t> framesDropped_{0};
  uint16_t sequence_ = 0;
};

}  // namespace nova::net
