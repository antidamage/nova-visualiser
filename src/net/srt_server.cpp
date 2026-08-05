#include "net/srt_server.h"

#include <netinet/in.h>
#include <srt/srt.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>

namespace nova::net {
namespace {

// SRT live mode's default payload. A message must fit in one packet, so access
// units are fragmented to this minus the 4-byte fragment header.
constexpr int kPayloadBytes = 1316;
constexpr size_t kFragmentHeaderBytes = 4;
// A slow reader may never add latency to the renderer. Keep only a handful of
// access units per reader, then make that reader wait for a clean IDR.
constexpr size_t kMaxQueuedFrames = 8;

int64_t monotonicMicroseconds() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch())
      .count();
}

void writeBigEndian16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value >> 8);
  out[1] = static_cast<uint8_t>(value);
}

void writeBigEndian32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value >> 24);
  out[1] = static_cast<uint8_t>(value >> 16);
  out[2] = static_cast<uint8_t>(value >> 8);
  out[3] = static_cast<uint8_t>(value);
}

void writeBigEndian64(uint8_t* out, uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    out[index] = static_cast<uint8_t>(value >> (56 - index * 8));
  }
}

std::string base64(const std::vector<uint8_t>& data) {
  static const char* table =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  for (size_t index = 0; index < data.size(); index += 3) {
    const uint32_t a = data[index];
    const uint32_t b = index + 1 < data.size() ? data[index + 1] : 0;
    const uint32_t c = index + 2 < data.size() ? data[index + 2] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;
    out.push_back(table[(triple >> 18) & 0x3F]);
    out.push_back(table[(triple >> 12) & 0x3F]);
    out.push_back(index + 1 < data.size() ? table[(triple >> 6) & 0x3F] : '=');
    out.push_back(index + 2 < data.size() ? table[triple & 0x3F] : '=');
  }
  return out;
}

// Protocol 2 advertises the capabilities a v1 client does not have. The
// deployed tvOS client reads only `parameterSets` and ignores every other key,
// so raising this is safe in the server-first deploy order.
std::string encodeHeader(const StreamHeader& header, int latencyMs) {
  std::string json = "{";
  json += "\"protocol\":2,";
  json += "\"transport\":\"srt\",";
  json += "\"latencyMs\":" + std::to_string(latencyMs) + ",";
  json += "\"capabilities\":[\"keyframe-request\",\"fragmented\",\"tsbpd\"],";
  json += "\"codec\":\"" + header.codec + "\",";
  json += "\"profile\":\"" + header.profile + "\",";
  json += "\"width\":" + std::to_string(header.width) + ",";
  json += "\"height\":" + std::to_string(header.height) + ",";
  json += "\"frameRate\":" + std::to_string(header.frameRate) + ",";
  json += "\"parameterSets\":\"" + base64(header.parameterSets) + "\"";
  json += "}";
  return json;
}

// One-time library init. srt_startup is refcounted internally but calling it
// from one place keeps the teardown ordering obvious.
struct SrtLibrary {
  SrtLibrary() { srt_startup(); }
  ~SrtLibrary() { srt_cleanup(); }
};

}  // namespace

struct SrtServer::Client {
  SRTSOCKET socket = SRT_INVALID_SOCK;
  // libsrt permits work on different sockets concurrently, but send/stats/
  // close racing on the same socket has repeatedly faulted inside
  // libsrt-gnutls 1.5.4 on Iridium. Every operation on this socket is
  // serialised; the render thread still never blocks on network delivery
  // because sends themselves remain non-blocking.
  std::mutex socketMutex;
  std::mutex queueMutex;
  std::condition_variable signal;
  struct QueuedUnit {
    std::shared_ptr<const std::vector<uint8_t>> payload;
    uint16_t sequence = 0;
  };
  std::deque<QueuedUnit> queue;
  std::atomic<bool> alive{true};
  // Set when a send failed for want of buffer: everything is dropped until the
  // next IDR, because resuming mid-GOP decodes into garbage.
  bool awaitingKeyframe = true;
  SrtTransportStats stats;
};

SrtServer::~SrtServer() { stop(); }

bool SrtServer::start(int port, int latencyMs, StreamHeader header, KeyframeRequest requestKeyframe,
                      std::string& error) {
  static SrtLibrary library;
  (void)library;

  port_ = port;
  latencyMs_ = std::max(20, std::min(2000, latencyMs));
  header_ = std::move(header);
  requestKeyframe_ = std::move(requestKeyframe);

  listenSocket_ = srt_create_socket();
  if (listenSocket_ == SRT_INVALID_SOCK) {
    error = std::string("srt_create_socket: ") + srt_getlasterror_str();
    return false;
  }

  const int transtype = SRTT_LIVE;
  srt_setsockopt(listenSocket_, 0, SRTO_TRANSTYPE, &transtype, sizeof(transtype));
  const int payload = kPayloadBytes;
  srt_setsockopt(listenSocket_, 0, SRTO_PAYLOADSIZE, &payload, sizeof(payload));
  // Timestamp-based delivery is the whole point: without it SRT hands packets
  // over as they arrive and we are back to jitter reaching the panel directly.
  const int tsbpd = 1;
  srt_setsockopt(listenSocket_, 0, SRTO_TSBPDMODE, &tsbpd, sizeof(tsbpd));
  srt_setsockopt(listenSocket_, 0, SRTO_LATENCY, &latencyMs_, sizeof(latencyMs_));
  // Bounded ARQ is sufficient at the measured sub-millisecond household RTT.
  // The former 10x5 FEC matrix consumed about 30% extra packets and repeatedly
  // failed to rebuild on tvOS, making the receiver fall seconds behind.
  // Reap a client that stops acknowledging rather than holding its slot.
  const int peerIdle = 5000;
  srt_setsockopt(listenSocket_, 0, SRTO_PEERIDLETIMEO, &peerIdle, sizeof(peerIdle));
  // Non-blocking sends: a client that cannot keep up must be dropped to the
  // next IDR, never allowed to stall the render thread.
  const int sndSyn = 0;
  srt_setsockopt(listenSocket_, 0, SRTO_SNDSYN, &sndSyn, sizeof(sndSyn));
  // A blocking srt_accept interrupted by srt_close has crashed inside libsrt
  // on this driver/userspace build. Polling accept lets stop flip `running_`,
  // join the thread, and only then release the listener deterministically.
  const int rcvSyn = 0;
  srt_setsockopt(listenSocket_, 0, SRTO_RCVSYN, &rcvSyn, sizeof(rcvSyn));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(static_cast<uint16_t>(port));
  if (srt_bind(listenSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
      SRT_ERROR) {
    error = std::string("srt_bind(") + std::to_string(port) + "): " + srt_getlasterror_str();
    srt_close(listenSocket_);
    listenSocket_ = SRT_INVALID_SOCK;
    return false;
  }
  if (srt_listen(listenSocket_, 4) == SRT_ERROR) {
    error = std::string("srt_listen: ") + srt_getlasterror_str();
    srt_close(listenSocket_);
    listenSocket_ = SRT_INVALID_SOCK;
    return false;
  }

  running_.store(true);
  acceptThread_ = std::thread(&SrtServer::acceptLoop, this);
  statsThread_ = std::thread(&SrtServer::pollStats, this);
  return true;
}

void SrtServer::stop() {
  if (!running_.exchange(false)) return;
  if (acceptThread_.joinable()) acceptThread_.join();
  if (listenSocket_ != SRT_INVALID_SOCK) {
    srt_close(listenSocket_);
    listenSocket_ = SRT_INVALID_SOCK;
  }
  if (statsThread_.joinable()) statsThread_.join();

  std::vector<std::shared_ptr<Client>> targets;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    targets = clients_;
  }
  for (const std::shared_ptr<Client>& client : targets) {
    client->alive.store(false, std::memory_order_relaxed);
    client->signal.notify_all();
    std::lock_guard<std::mutex> socketLock(client->socketMutex);
    if (client->socket != SRT_INVALID_SOCK) {
      srt_close(client->socket);
      client->socket = SRT_INVALID_SOCK;
    }
  }
  // Writers are detached for the same lifetime reason as the TCP rung. Wait
  // for their self-removal before this server can be destroyed.
  for (int attempt = 0; attempt < 100; ++attempt) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (clients_.empty()) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::lock_guard<std::mutex> lock(mutex_);
  clients_.clear();
  clientCount_.store(0, std::memory_order_relaxed);
}

void SrtServer::updateHeader(StreamHeader header) {
  std::lock_guard<std::mutex> lock(mutex_);
  header_ = std::move(header);
}

bool SrtServer::sendHandshake(Client& client) {
  std::string headerJson;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    headerJson = encodeHeader(header_, latencyMs_);
  }
  std::vector<uint8_t> handshake;
  handshake.reserve(12 + headerJson.size());
  const char* magic = "NOVAVIS1";
  handshake.insert(handshake.end(), magic, magic + 8);
  handshake.resize(12);
  writeBigEndian32(handshake.data() + 8, static_cast<uint32_t>(headerJson.size()));
  handshake.insert(handshake.end(), headerJson.begin(), headerJson.end());

  // The handshake is small but not guaranteed to be under one payload once the
  // parameter sets are base64'd, so it goes through the same fragmenter.
  size_t offset = 0;
  uint16_t sequence = 0;
  while (offset < handshake.size()) {
    const size_t chunk =
        std::min(handshake.size() - offset, static_cast<size_t>(kPayloadBytes) - kFragmentHeaderBytes);
    std::vector<uint8_t> packet(kFragmentHeaderBytes + chunk);
    writeBigEndian16(packet.data(), sequence);
    packet[2] = static_cast<uint8_t>((offset == 0 ? 1 : 0) |
                                     (offset + chunk >= handshake.size() ? 2 : 0));
    packet[3] = 0;
    std::memcpy(packet.data() + kFragmentHeaderBytes, handshake.data() + offset, chunk);
    if (srt_sendmsg(client.socket, reinterpret_cast<const char*>(packet.data()),
                    static_cast<int>(packet.size()), -1, 1) == SRT_ERROR) {
      return false;
    }
    offset += chunk;
  }
  return true;
}

bool SrtServer::sendAccessUnit(Client& client, const std::vector<uint8_t>& unit, uint16_t sequence) {
  const size_t chunkSize = static_cast<size_t>(kPayloadBytes) - kFragmentHeaderBytes;
  size_t offset = 0;
  while (offset < unit.size()) {
    const size_t chunk = std::min(unit.size() - offset, chunkSize);
    std::vector<uint8_t> packet(kFragmentHeaderBytes + chunk);
    writeBigEndian16(packet.data(), sequence);
    packet[2] = static_cast<uint8_t>((offset == 0 ? 1 : 0) |
                                     (offset + chunk >= unit.size() ? 2 : 0));
    packet[3] = 0;
    std::memcpy(packet.data() + kFragmentHeaderBytes, unit.data() + offset, chunk);
    // This runs on a per-client writer thread. A full SRT send buffer abandons
    // only this reader's access unit; it can never stall render/encode or a
    // different reader's delivery.
    if (srt_sendmsg(client.socket, reinterpret_cast<const char*>(packet.data()),
                    static_cast<int>(packet.size()), latencyMs_, 1) == SRT_ERROR) {
      return false;
    }
    offset += chunk;
  }
  return true;
}

void SrtServer::serviceClient(std::shared_ptr<Client> client) {
  while (client->alive.load(std::memory_order_relaxed)) {
    Client::QueuedUnit unit;
    {
      std::unique_lock<std::mutex> lock(client->queueMutex);
      client->signal.wait(lock, [&client]() {
        return !client->queue.empty() || !client->alive.load(std::memory_order_relaxed);
      });
      if (!client->alive.load(std::memory_order_relaxed)) break;
      unit = std::move(client->queue.front());
      client->queue.pop_front();
    }

    bool sent = false;
    bool socketGone = false;
    int error = 0;
    {
      std::lock_guard<std::mutex> socketLock(client->socketMutex);
      socketGone = client->socket == SRT_INVALID_SOCK;
      if (!socketGone) {
        sent = sendAccessUnit(*client, *unit.payload, unit.sequence);
        if (!sent) error = srt_getlasterror(nullptr);
      }
    }
    if (sent) {
      framesSent_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    framesDropped_.fetch_add(1, std::memory_order_relaxed);
    if (error == SRT_ECONNLOST || error == SRT_EINVSOCK || error == SRT_ECONNREJ || socketGone) {
      client->alive.store(false, std::memory_order_relaxed);
      break;
    }
    // Partial delivery cannot be decoded. Discard this reader's backlog and
    // let broadcast resume it on the next IDR; other readers keep streaming.
    {
      std::lock_guard<std::mutex> lock(client->queueMutex);
      client->queue.clear();
      client->awaitingKeyframe = true;
    }
    if (requestKeyframe_) requestKeyframe_();
  }

  client->alive.store(false, std::memory_order_relaxed);
  client->signal.notify_all();
  {
    std::lock_guard<std::mutex> socketLock(client->socketMutex);
    if (client->socket != SRT_INVALID_SOCK) {
      srt_close(client->socket);
      client->socket = SRT_INVALID_SOCK;
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
    clientCount_.store(static_cast<int>(clients_.size()), std::memory_order_relaxed);
  }
}

void SrtServer::acceptLoop() {
  const int poller = srt_epoll_create();
  if (poller < 0) return;
  const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
  if (srt_epoll_add_usock(poller, listenSocket_, &events) == SRT_ERROR) {
    srt_epoll_release(poller);
    return;
  }

  while (running_.load(std::memory_order_relaxed)) {
    SRTSOCKET ready = SRT_INVALID_SOCK;
    int readyCount = 1;
    const int result = srt_epoll_wait(poller, &ready, &readyCount, nullptr, nullptr, 250,
                                      nullptr, nullptr, nullptr, nullptr);
    if (!running_.load(std::memory_order_relaxed)) break;
    if (result <= 0 || readyCount <= 0 || ready != listenSocket_) continue;

    sockaddr_storage peer{};
    int peerLength = sizeof(peer);
    const SRTSOCKET socket =
        srt_accept(listenSocket_, reinterpret_cast<sockaddr*>(&peer), &peerLength);
    if (socket == SRT_INVALID_SOCK) {
      if (!running_.load(std::memory_order_relaxed)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    auto client = std::make_shared<Client>();
    client->socket = socket;
    if (!sendHandshake(*client)) {
      srt_close(socket);
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      clients_.push_back(client);
      clientCount_.store(static_cast<int>(clients_.size()), std::memory_order_relaxed);
    }
    std::thread(&SrtServer::serviceClient, this, client).detach();
    // A joining client has no reference frames; without this it stares at
    // nothing until the next scheduled GOP boundary.
    if (requestKeyframe_) requestKeyframe_();
  }

  srt_epoll_remove_usock(poller, listenSocket_);
  srt_epoll_release(poller);
}

void SrtServer::broadcast(const encode::EncodedFrame& frame, int64_t ptsMicroseconds) {
  std::vector<std::shared_ptr<Client>> targets;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    targets = clients_;
  }
  if (targets.empty()) return;

  // Same 24-byte envelope as the TCP rung, so the client's parser is shared.
  constexpr size_t kHeaderBytes = 24;
  auto unit = std::make_shared<std::vector<uint8_t>>(kHeaderBytes + frame.data.size());
  (*unit)[0] = 0;  // video access unit
  (*unit)[1] = frame.keyframe ? 1 : 0;
  (*unit)[2] = 0;
  (*unit)[3] = 0;
  writeBigEndian32(unit->data() + 4, static_cast<uint32_t>(frame.data.size()));
  writeBigEndian64(unit->data() + 8, static_cast<uint64_t>(ptsMicroseconds));
  writeBigEndian64(unit->data() + 16, static_cast<uint64_t>(monotonicMicroseconds()));
  std::memcpy(unit->data() + kHeaderBytes, frame.data.data(), frame.data.size());

  const uint16_t sequence = sequence_++;
  bool overflowed = false;
  for (const std::shared_ptr<Client>& client : targets) {
    if (!client->alive.load(std::memory_order_relaxed)) continue;
    std::lock_guard<std::mutex> lock(client->queueMutex);
    if (client->awaitingKeyframe) {
      if (!frame.keyframe) continue;
      client->awaitingKeyframe = false;
    }
    if (client->queue.size() >= kMaxQueuedFrames) {
      // This client fell behind. It alone drops to a fresh IDR; the shared
      // encoder and all other client queues remain untouched.
      client->queue.clear();
      client->awaitingKeyframe = true;
      overflowed = true;
      continue;
    }
    client->queue.push_back({unit, sequence});
    client->signal.notify_one();
  }

  if (overflowed && requestKeyframe_) requestKeyframe_();
}

void SrtServer::pollStats() {
  while (running_.load(std::memory_order_relaxed)) {
    std::vector<std::shared_ptr<Client>> targets;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      targets = clients_;
    }

    SrtTransportStats worst;
    for (const std::shared_ptr<Client>& client : targets) {
      std::lock_guard<std::mutex> socketLock(client->socketMutex);
      if (client->socket == SRT_INVALID_SOCK) continue;
      SRT_TRACEBSTATS raw{};
      if (srt_bstats(client->socket, &raw, 1) == SRT_ERROR) continue;
      SrtTransportStats stats;
      stats.valid = true;
      stats.rttMs = raw.msRTT;
      stats.bandwidthMbps = raw.mbpsBandwidth;
      stats.packetsLost = raw.pktRcvLossTotal;
      stats.packetsRetransmitted = raw.pktRetransTotal;
      stats.packetsDropped = raw.pktSndDropTotal;
      stats.bytesSent = static_cast<int64_t>(raw.byteSentTotal);
      stats.sendBufferMs = raw.msSndBuf;
      client->stats = stats;

      // Worst case, not average: the controller should react to the client
      // having the hardest time, not to a mean that hides it.
      if (!worst.valid || stats.rttMs > worst.rttMs) worst.rttMs = stats.rttMs;
      worst.valid = true;
      worst.packetsLost = std::max(worst.packetsLost, stats.packetsLost);
      worst.packetsRetransmitted =
          std::max(worst.packetsRetransmitted, stats.packetsRetransmitted);
      worst.packetsDropped = std::max(worst.packetsDropped, stats.packetsDropped);
      worst.sendBufferMs = std::max(worst.sendBufferMs, stats.sendBufferMs);
      worst.bandwidthMbps = std::max(worst.bandwidthMbps, stats.bandwidthMbps);
      worst.bytesSent = std::max(worst.bytesSent, stats.bytesSent);
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      worstStats_ = worst;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

SrtTransportStats SrtServer::worstStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return worstStats_;
}

}  // namespace nova::net
