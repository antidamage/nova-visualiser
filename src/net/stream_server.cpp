#include "net/stream_server.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>

#include "core/json.h"

namespace nova::net {
namespace {

constexpr size_t kMaxQueuedFrames = 8;

int64_t monotonicMicroseconds() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch())
      .count();
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

uint32_t readBigEndian32(const uint8_t* in) {
  return (static_cast<uint32_t>(in[0]) << 24) | (static_cast<uint32_t>(in[1]) << 16) |
         (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
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

std::string encodeHeader(const StreamHeader& header) {
  std::string json = "{";
  json += "\"protocol\":1,";
  json += "\"codec\":\"" + header.codec + "\",";
  json += "\"profile\":\"" + header.profile + "\",";
  json += "\"width\":" + std::to_string(header.width) + ",";
  json += "\"height\":" + std::to_string(header.height) + ",";
  json += "\"frameRate\":" + std::to_string(header.frameRate) + ",";
  json += "\"parameterSets\":\"" + base64(header.parameterSets) + "\"";
  json += "}";
  return json;
}

}  // namespace

struct StreamServer::Client {
  int socket = -1;
  // No std::thread member. The writer thread owns a shared_ptr to this object,
  // so when a client disconnects the last reference is released *by that very
  // thread* — destroying a joinable std::thread there calls std::terminate.
  // The thread is detached instead, and shutdown waits on `alive`/count.
  std::mutex mutex;
  std::condition_variable signal;
  // Shared, immutable packets. `broadcast` used to copy the whole access unit
  // per client, which for a 4K IDR is megabytes per viewer per frame.
  std::deque<std::shared_ptr<const std::vector<uint8_t>>> queue;
  std::atomic<bool> alive{true};
  // Set when the queue overflowed: everything is dropped until the next IDR,
  // because resuming mid-GOP would only produce corrupt output.
  bool awaitingKeyframe = false;
  ClientTelemetry telemetry;
};

StreamServer::~StreamServer() { stop(); }

bool StreamServer::start(int port, StreamHeader header, KeyframeRequest requestKeyframe,
                         std::string& error) {
  port_ = port;
  header_ = std::move(header);
  requestKeyframe_ = std::move(requestKeyframe);

  listenSocket_ = ::socket(AF_INET6, SOCK_STREAM, 0);
  bool dualStack = listenSocket_ >= 0;
  if (!dualStack) listenSocket_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listenSocket_ < 0) {
    error = "could not create the stream listen socket";
    return false;
  }

  const int one = 1;
  ::setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (dualStack) {
    const int off = 0;
    ::setsockopt(listenSocket_, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    address.sin6_port = htons(static_cast<uint16_t>(port));
    if (::bind(listenSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
      error = "could not bind stream port " + std::to_string(port);
      ::close(listenSocket_);
      listenSocket_ = -1;
      return false;
    }
  } else {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(listenSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
      error = "could not bind stream port " + std::to_string(port);
      ::close(listenSocket_);
      listenSocket_ = -1;
      return false;
    }
  }

  if (::listen(listenSocket_, 8) < 0) {
    error = "listen failed on stream port " + std::to_string(port);
    ::close(listenSocket_);
    listenSocket_ = -1;
    return false;
  }

  running_.store(true);
  acceptThread_ = std::thread(&StreamServer::acceptLoop, this);
  return true;
}

void StreamServer::updateHeader(StreamHeader header) {
  std::lock_guard<std::mutex> lock(mutex_);
  header_ = std::move(header);
}

void StreamServer::acceptLoop() {
  while (running_.load(std::memory_order_relaxed)) {
    const int socketFd = ::accept(listenSocket_, nullptr, nullptr);
    if (socketFd < 0) {
      if (!running_.load(std::memory_order_relaxed)) break;
      continue;
    }
    const int one = 1;
    // Latency beats throughput here: a 40 ms Nagle delay is over two frames.
    ::setsockopt(socketFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // Without these a client that vanishes mid-send (WiFi dropping, the
    // television being unplugged) leaves its writer thread blocked in send()
    // indefinitely and its slot occupied forever.
    const timeval timeout{2, 0};
    ::setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(socketFd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    const int keepIdle = 5;
    const int keepInterval = 2;
    const int keepCount = 3;
    ::setsockopt(socketFd, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(keepIdle));
    ::setsockopt(socketFd, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(keepInterval));
    ::setsockopt(socketFd, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(keepCount));
    const unsigned int userTimeout = 8000;
    ::setsockopt(socketFd, IPPROTO_TCP, TCP_USER_TIMEOUT, &userTimeout, sizeof(userTimeout));

    auto client = std::make_shared<Client>();
    client->socket = socketFd;
    client->awaitingKeyframe = true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      clients_.push_back(client);
    }
    clientCount_.store(static_cast<int>(clients_.size()), std::memory_order_relaxed);
    std::thread(&StreamServer::serviceClient, this, client).detach();
    if (requestKeyframe_) requestKeyframe_();
  }
}

void StreamServer::serviceClient(std::shared_ptr<Client> client) {
  // Handshake.
  std::string headerJson;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    headerJson = encodeHeader(header_);
  }
  std::vector<uint8_t> handshake;
  handshake.reserve(12 + headerJson.size());
  const char* magic = "NOVAVIS1";
  handshake.insert(handshake.end(), magic, magic + 8);
  handshake.resize(12);
  writeBigEndian32(handshake.data() + 8, static_cast<uint32_t>(headerJson.size()));
  handshake.insert(handshake.end(), headerJson.begin(), headerJson.end());
  if (::send(client->socket, handshake.data(), handshake.size(), MSG_NOSIGNAL) < 0) {
    client->alive.store(false);
  }

  // A reader thread for the client's latency reports. Kept separate so a client
  // that never sends one cannot stall the writer.
  std::thread reader([this, client]() {
    uint8_t head[5];
    while (client->alive.load(std::memory_order_relaxed)) {
      size_t got = 0;
      while (got < sizeof(head)) {
        const ssize_t received =
            ::recv(client->socket, head + got, sizeof(head) - got, 0);
        if (received <= 0) {
          client->alive.store(false);
          client->signal.notify_all();
          return;
        }
        got += static_cast<size_t>(received);
      }
      // Every message is `type` + a big-endian length, so the body must be
      // consumed whatever the type is. Skipping the header alone -- which is
      // what this did -- left the reader re-reading payload bytes as a header
      // and permanently desynchronised, which would break the moment a second
      // client->server message type existed.
      const uint32_t length = readBigEndian32(head + 1);
      if (length > 65536) {
        // Unrecoverable framing: there is no way to find the next boundary.
        client->alive.store(false);
        client->signal.notify_all();
        return;
      }
      std::string payload(length, '\0');
      size_t read = 0;
      while (read < length) {
        const ssize_t received =
            ::recv(client->socket, payload.data() + read, length - read, 0);
        if (received <= 0) {
          client->alive.store(false);
          client->signal.notify_all();
          return;
        }
        read += static_cast<size_t>(received);
      }

      if (head[0] == 0x81) {
        // Explicit keyframe request: the client's decoder failed and it needs a
        // stream access point to resume from.
        if (requestKeyframe_) requestKeyframe_();
        continue;
      }
      if (head[0] != 0x80) continue;  // consumed and ignored

      auto value = json::Value::parse(payload);
      if (!value) continue;
      ClientTelemetry telemetry;
      if (const json::Value* delay = value->find("presentationDelayMs")) {
        telemetry.presentationDelayMs = delay->numberOr(0);
      }
      if (const json::Value* decoded = value->find("decodedFrames")) {
        telemetry.decodedFrames = static_cast<int64_t>(decoded->numberOr(0));
      }
      if (const json::Value* dropped = value->find("droppedFrames")) {
        telemetry.droppedFrames = static_cast<int64_t>(dropped->numberOr(0));
      }
      telemetry.valid = true;
      {
        std::lock_guard<std::mutex> lock(client->mutex);
        client->telemetry = telemetry;
      }
      // Smooth hard: this feeds a clock offset, and a jumpy offset would make
      // the visualiser stutter against the beat rather than lock to it.
      const double previous = presentationDelaySeconds_.load(std::memory_order_relaxed);
      const double sample = telemetry.presentationDelayMs / 1000.0;
      const double smoothed = previous <= 0 ? sample : previous * 0.9 + sample * 0.1;
      presentationDelaySeconds_.store(std::min(0.75, std::max(0.0, smoothed)),
                                      std::memory_order_relaxed);
    }
  });

  // Writer.
  while (client->alive.load(std::memory_order_relaxed)) {
    std::shared_ptr<const std::vector<uint8_t>> payload;
    {
      std::unique_lock<std::mutex> lock(client->mutex);
      client->signal.wait(lock, [&client]() {
        return !client->queue.empty() || !client->alive.load(std::memory_order_relaxed);
      });
      if (!client->alive.load(std::memory_order_relaxed)) break;
      payload = std::move(client->queue.front());
      client->queue.pop_front();
    }
    size_t sent = 0;
    while (sent < payload->size()) {
      const ssize_t written =
          ::send(client->socket, payload->data() + sent, payload->size() - sent, MSG_NOSIGNAL);
      if (written <= 0) {
        // SO_SNDTIMEO makes this reachable on a wedged link rather than
        // blocking forever; either way the client is gone.
        client->alive.store(false);
        break;
      }
      sent += static_cast<size_t>(written);
    }
  }

  ::shutdown(client->socket, SHUT_RDWR);
  client->alive.store(false);
  client->signal.notify_all();
  if (reader.joinable()) reader.join();
  ::close(client->socket);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
    clientCount_.store(static_cast<int>(clients_.size()), std::memory_order_relaxed);
    if (clients_.empty()) presentationDelaySeconds_.store(0.0, std::memory_order_relaxed);
  }
}

void StreamServer::broadcast(const encode::EncodedFrame& frame, int64_t ptsMicroseconds) {
  std::vector<std::shared_ptr<Client>> targets;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    targets = clients_;
  }
  if (targets.empty()) return;

  constexpr size_t kHeaderBytes = 24;
  auto packet = std::make_shared<std::vector<uint8_t>>(kHeaderBytes + frame.data.size());
  (*packet)[0] = 0;  // video access unit
  (*packet)[1] = frame.keyframe ? 1 : 0;
  (*packet)[2] = 0;
  (*packet)[3] = 0;
  writeBigEndian32(packet->data() + 4, static_cast<uint32_t>(frame.data.size()));
  writeBigEndian64(packet->data() + 8, static_cast<uint64_t>(ptsMicroseconds));
  // One send timestamp for the whole broadcast. Per-client stamping would be
  // more precise in theory, but the clients are on the same LAN and the client
  // only uses this to estimate a smoothed pipeline delay.
  writeBigEndian64(packet->data() + 16, static_cast<uint64_t>(monotonicMicroseconds()));
  std::memcpy(packet->data() + kHeaderBytes, frame.data.data(), frame.data.size());

  const std::shared_ptr<const std::vector<uint8_t>> shared = std::move(packet);
  bool overflowed = false;
  for (const std::shared_ptr<Client>& client : targets) {
    std::lock_guard<std::mutex> lock(client->mutex);
    if (client->awaitingKeyframe) {
      if (!frame.keyframe) continue;
      client->awaitingKeyframe = false;
    }
    if (client->queue.size() >= kMaxQueuedFrames) {
      // The client cannot keep up. Dropping to the next IDR is the only honest
      // recovery: partial GOPs decode into garbage, and an unbounded queue
      // would turn a slow client into growing latency for everyone.
      client->queue.clear();
      client->awaitingKeyframe = true;
      overflowed = true;
      continue;
    }
    client->queue.push_back(shared);
    client->signal.notify_one();
  }

  // Ask for the IDR the dropped clients are now waiting on. This used to be
  // missing entirely: the flag was set and nothing ever requested a keyframe,
  // so a client that hiccuped stared at a frozen frame until the next scheduled
  // GOP boundary -- up to two seconds. Fired outside every client lock, and
  // rate-limited by the callee so a flapping client cannot force all-IDR.
  if (overflowed && requestKeyframe_) requestKeyframe_();
}

double StreamServer::presentationDelaySeconds() const {
  return presentationDelaySeconds_.load(std::memory_order_relaxed);
}

void StreamServer::stop() {
  if (!running_.exchange(false)) return;
  if (listenSocket_ >= 0) {
    ::shutdown(listenSocket_, SHUT_RDWR);
    ::close(listenSocket_);
    listenSocket_ = -1;
  }
  if (acceptThread_.joinable()) acceptThread_.join();

  std::vector<std::shared_ptr<Client>> targets;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    targets = clients_;
  }
  for (const std::shared_ptr<Client>& client : targets) {
    client->alive.store(false);
    client->signal.notify_all();
    ::shutdown(client->socket, SHUT_RDWR);
  }
  // Writer threads are detached, so wait for them to drop out of `clients_`
  // rather than joining. Bounded: shutdown must not hang on a wedged socket.
  for (int attempt = 0; attempt < 100; ++attempt) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (clients_.empty()) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    clients_.clear();
  }
  clientCount_.store(0, std::memory_order_relaxed);
}

}  // namespace nova::net
