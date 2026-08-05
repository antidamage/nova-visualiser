#include "net/http_client.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace nova::net {
namespace {

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool writeAll(int socketFd, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t written = ::send(socketFd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (written <= 0) return false;
    sent += static_cast<size_t>(written);
  }
  return true;
}

HttpResponse perform(const std::string& method, const std::string& url, const std::string& body,
                     const std::string& contentType,
                     const std::map<std::string, std::string>& headers, int timeoutSeconds) {
  HttpResponse response;
  Url target;
  if (!Url::parse(url, target)) {
    response.error = "malformed url: " + url;
    return response;
  }

  const int socketFd = connectTo(target.host, target.port, timeoutSeconds, response.error);
  if (socketFd < 0) return response;

  std::ostringstream request;
  request << method << " " << target.path << " HTTP/1.1\r\n";
  request << "Host: " << target.host << "\r\n";
  request << "Connection: close\r\n";
  request << "User-Agent: nova-visualiser/1\r\n";
  for (const auto& [key, value] : headers) request << key << ": " << value << "\r\n";
  if (!body.empty()) {
    request << "Content-Type: " << contentType << "\r\n";
    request << "Content-Length: " << body.size() << "\r\n";
  }
  request << "\r\n" << body;

  if (!writeAll(socketFd, request.str())) {
    response.error = "failed to send request";
    ::close(socketFd);
    return response;
  }

  std::string raw;
  char buffer[16384];
  while (true) {
    const ssize_t received = ::recv(socketFd, buffer, sizeof(buffer), 0);
    if (received <= 0) break;
    raw.append(buffer, static_cast<size_t>(received));
  }
  ::close(socketFd);

  const size_t headerEnd = raw.find("\r\n\r\n");
  if (headerEnd == std::string::npos) {
    response.error = "truncated response";
    return response;
  }

  std::istringstream headerStream(raw.substr(0, headerEnd));
  std::string statusLine;
  std::getline(headerStream, statusLine);
  const size_t firstSpace = statusLine.find(' ');
  if (firstSpace != std::string::npos) {
    response.status = std::atoi(statusLine.c_str() + firstSpace + 1);
  }
  std::string line;
  while (std::getline(headerStream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = lowercase(line.substr(0, colon));
    size_t valueStart = colon + 1;
    while (valueStart < line.size() && line[valueStart] == ' ') ++valueStart;
    response.headers[key] = line.substr(valueStart);
  }

  std::string payload = raw.substr(headerEnd + 4);
  auto encoding = response.headers.find("transfer-encoding");
  if (encoding != response.headers.end() && lowercase(encoding->second).find("chunked") !=
                                                std::string::npos) {
    // Next.js route handlers stream chunked responses; decode rather than
    // handing the caller a body full of hex length markers.
    std::string decoded;
    size_t cursor = 0;
    while (cursor < payload.size()) {
      const size_t lineEnd = payload.find("\r\n", cursor);
      if (lineEnd == std::string::npos) break;
      const size_t chunkSize =
          static_cast<size_t>(std::strtoul(payload.substr(cursor, lineEnd - cursor).c_str(), nullptr, 16));
      if (chunkSize == 0) break;
      const size_t chunkStart = lineEnd + 2;
      if (chunkStart + chunkSize > payload.size()) break;
      decoded.append(payload, chunkStart, chunkSize);
      cursor = chunkStart + chunkSize + 2;
    }
    payload = std::move(decoded);
  }
  response.body = std::move(payload);
  return response;
}

}  // namespace

bool Url::parse(const std::string& text, Url& out) {
  std::string remainder = text;
  const std::string prefix = "http://";
  if (remainder.rfind(prefix, 0) == 0) {
    remainder = remainder.substr(prefix.size());
  } else if (remainder.rfind("https://", 0) == 0) {
    // Everything the renderer talks to is on localhost; TLS would only be a way
    // to fail in a new place.
    return false;
  }

  const size_t slash = remainder.find('/');
  std::string authority = slash == std::string::npos ? remainder : remainder.substr(0, slash);
  out.path = slash == std::string::npos ? "/" : remainder.substr(slash);
  if (out.path.empty()) out.path = "/";

  const size_t colon = authority.find(':');
  if (colon == std::string::npos) {
    out.host = authority;
    out.port = 80;
  } else {
    out.host = authority.substr(0, colon);
    out.port = std::atoi(authority.c_str() + colon + 1);
  }
  return !out.host.empty() && out.port > 0;
}

int connectTo(const std::string& host, int port, int timeoutSeconds, std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0 || results == nullptr) {
    error = "could not resolve " + host;
    return -1;
  }

  int socketFd = -1;
  for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
    socketFd = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (socketFd < 0) continue;

    timeval timeout{timeoutSeconds, 0};
    ::setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    const int one = 1;
    ::setsockopt(socketFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (::connect(socketFd, entry->ai_addr, entry->ai_addrlen) == 0) break;
    ::close(socketFd);
    socketFd = -1;
  }
  ::freeaddrinfo(results);
  if (socketFd < 0) error = "could not connect to " + host + ":" + std::to_string(port);
  return socketFd;
}

HttpResponse httpGet(const std::string& url, const std::map<std::string, std::string>& headers,
                     int timeoutSeconds) {
  return perform("GET", url, "", "", headers, timeoutSeconds);
}

HttpResponse httpPost(const std::string& url, const std::string& body,
                      const std::string& contentType,
                      const std::map<std::string, std::string>& headers, int timeoutSeconds) {
  return perform("POST", url, body, contentType, headers, timeoutSeconds);
}

}  // namespace nova::net
