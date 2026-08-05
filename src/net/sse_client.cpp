#include "net/sse_client.h"

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <sstream>

#include "net/http_client.h"

namespace nova::net {

SseClient::~SseClient() { stop(); }

void SseClient::start(const std::string& url, Handler handler) {
  if (running_.exchange(true)) return;
  thread_ = std::thread(&SseClient::run, this, url, std::move(handler));
}

void SseClient::stop() {
  if (!running_.exchange(false)) return;
  // Shut the socket down so a blocked recv returns immediately instead of
  // holding shutdown hostage for the read timeout.
  const int socketFd = socket_.exchange(-1);
  if (socketFd >= 0) {
    ::shutdown(socketFd, SHUT_RDWR);
    ::close(socketFd);
  }
  if (thread_.joinable()) thread_.join();
}

void SseClient::run(std::string url, Handler handler) {
  Url target;
  if (!Url::parse(url, target)) return;

  int backoffSeconds = 1;
  while (running_.load(std::memory_order_relaxed)) {
    std::string error;
    const int socketFd = connectTo(target.host, target.port, 5, error);
    if (socketFd < 0) {
      std::this_thread::sleep_for(std::chrono::seconds(backoffSeconds));
      backoffSeconds = std::min(backoffSeconds * 2, 15);
      continue;
    }
    socket_.store(socketFd);

    std::ostringstream request;
    request << "GET " << target.path << " HTTP/1.1\r\n";
    request << "Host: " << target.host << "\r\n";
    request << "Accept: text/event-stream\r\n";
    request << "Cache-Control: no-cache\r\n";
    request << "Connection: keep-alive\r\n";
    request << "User-Agent: nova-visualiser/1\r\n\r\n";
    const std::string requestText = request.str();
    if (::send(socketFd, requestText.data(), requestText.size(), MSG_NOSIGNAL) < 0) {
      socket_.store(-1);
      ::close(socketFd);
      continue;
    }

    connected_.store(true);
    backoffSeconds = 1;

    std::string pending;
    std::string eventName;
    std::string data;
    bool headersDone = false;
    char buffer[8192];

    while (running_.load(std::memory_order_relaxed)) {
      const ssize_t received = ::recv(socketFd, buffer, sizeof(buffer), 0);
      if (received <= 0) break;
      pending.append(buffer, static_cast<size_t>(received));

      if (!headersDone) {
        const size_t headerEnd = pending.find("\r\n\r\n");
        if (headerEnd == std::string::npos) continue;
        pending = pending.substr(headerEnd + 4);
        headersDone = true;
      }

      size_t newline;
      while ((newline = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.empty()) {
          // Blank line terminates an event.
          if (!data.empty()) handler(eventName.empty() ? "message" : eventName, data);
          eventName.clear();
          data.clear();
          continue;
        }
        if (line[0] == ':') continue;  // comment / keep-alive
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string field = line.substr(0, colon);
        size_t valueStart = colon + 1;
        if (valueStart < line.size() && line[valueStart] == ' ') ++valueStart;
        const std::string value = line.substr(valueStart);
        if (field == "event") {
          eventName = value;
        } else if (field == "data") {
          if (!data.empty()) data.push_back('\n');
          data += value;
        }
      }
    }

    connected_.store(false);
    const int previous = socket_.exchange(-1);
    if (previous >= 0) ::close(previous);
    if (running_.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
}

}  // namespace nova::net
