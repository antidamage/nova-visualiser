#include "net/control_server.h"

#include "net/socket_activation.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sstream>

namespace nova::net {

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start(int port, Handler handler, std::string& error) {
  handler_ = std::move(handler);
  // Socket activation (specs/idle-exit.md): when nova-visualiser.socket started
  // this process for a connection on the control port, that connection is
  // waiting on the listener systemd passed in. Adopt it; bind only when run by
  // hand.
  listenSocket_ = inheritedTcpListener(port);
  if (listenSocket_ >= 0) {
    inherited_ = true;
    running_.store(true);
    thread_ = std::thread(&ControlServer::run, this);
    return true;
  }
  listenSocket_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listenSocket_ < 0) {
    error = "could not create the control socket";
    return false;
  }
  const int one = 1;
  ::setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(listenSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    error = "could not bind control port " + std::to_string(port);
    ::close(listenSocket_);
    listenSocket_ = -1;
    return false;
  }
  if (::listen(listenSocket_, 8) < 0) {
    error = "listen failed on control port " + std::to_string(port);
    ::close(listenSocket_);
    listenSocket_ = -1;
    return false;
  }

  running_.store(true);
  thread_ = std::thread(&ControlServer::run, this);
  return true;
}

void ControlServer::run() {
  while (running_.load(std::memory_order_relaxed)) {
    // An inherited listener is shared with systemd, so stop() must not
    // shutdown() it (that would break the socket unit too). Poll instead, so
    // the loop still notices `running_` going false within a quarter second.
    if (inherited_) {
      pollfd ready{listenSocket_, POLLIN, 0};
      if (::poll(&ready, 1, 250) <= 0) continue;
      if (!(ready.revents & POLLIN)) {
        // POLLERR/POLLNVAL: accept would fail at once; back off, never spin.
        ::poll(nullptr, 0, 250);
        continue;
      }
    }
    const int socketFd = ::accept(listenSocket_, nullptr, nullptr);
    if (socketFd < 0) {
      if (!running_.load(std::memory_order_relaxed)) break;
      continue;
    }
    timeval timeout{5, 0};
    ::setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::string raw;
    char buffer[8192];
    size_t headerEnd = std::string::npos;
    while (true) {
      const ssize_t received = ::recv(socketFd, buffer, sizeof(buffer), 0);
      if (received <= 0) break;
      raw.append(buffer, static_cast<size_t>(received));
      headerEnd = raw.find("\r\n\r\n");
      if (headerEnd == std::string::npos) continue;
      // Read the rest of the body when one was announced.
      size_t contentLength = 0;
      const size_t marker = raw.find("Content-Length:");
      if (marker != std::string::npos && marker < headerEnd) {
        contentLength = static_cast<size_t>(std::atol(raw.c_str() + marker + 15));
      }
      if (raw.size() >= headerEnd + 4 + contentLength) break;
    }

    ControlResponse response;
    if (headerEnd == std::string::npos) {
      response.status = 400;
      response.body = R"({"error":"malformed request"})";
    } else {
      ControlRequest request;
      std::istringstream headerStream(raw.substr(0, headerEnd));
      std::string requestLine;
      std::getline(headerStream, requestLine);
      std::istringstream lineStream(requestLine);
      std::string version;
      lineStream >> request.method >> request.path >> version;
      request.body = raw.substr(headerEnd + 4);
      response = handler_ ? handler_(request) : ControlResponse{404, "application/json", "{}"};
    }

    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << " OK\r\n";
    out << "Content-Type: " << response.contentType << "\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Cache-Control: no-store\r\n";
    // The dashboard fetches this from the browser for the diagnostics panel.
    out << "Access-Control-Allow-Origin: *\r\n";
    out << "Connection: close\r\n\r\n";
    out << response.body;
    const std::string text = out.str();
    ::send(socketFd, text.data(), text.size(), MSG_NOSIGNAL);
    ::close(socketFd);
  }
}

void ControlServer::stop() {
  if (!running_.exchange(false)) return;
  if (inherited_) {
    // Join first (the poll loop exits on its own), then close only our copy.
    if (thread_.joinable()) thread_.join();
    if (listenSocket_ >= 0) ::close(listenSocket_);
    listenSocket_ = -1;
    return;
  }
  if (listenSocket_ >= 0) {
    ::shutdown(listenSocket_, SHUT_RDWR);
    ::close(listenSocket_);
    listenSocket_ = -1;
  }
  if (thread_.joinable()) thread_.join();
}

}  // namespace nova::net
