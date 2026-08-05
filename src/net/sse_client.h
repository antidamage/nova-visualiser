// Server-sent-events client for the dashboard's existing `/api/events` stream.
//
// Realtime config push reuses the bus the browsers already use rather than
// inventing a second one: `lib/dashboard-events.ts` gains a phonoscope
// publisher, and this consumes it. Polling remains as the fallback, so a
// dropped stream degrades to "up to one poll interval late" rather than
// "stuck on stale settings".
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace nova::net {

class SseClient {
 public:
  using Handler = std::function<void(const std::string& event, const std::string& data)>;

  SseClient() = default;
  ~SseClient();

  SseClient(const SseClient&) = delete;
  SseClient& operator=(const SseClient&) = delete;

  void start(const std::string& url, Handler handler);
  void stop();

  bool connected() const { return connected_.load(std::memory_order_relaxed); }

 private:
  void run(std::string url, Handler handler);

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::atomic<int> socket_{-1};
};

}  // namespace nova::net
