#include "net/socket_activation.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

namespace nova::net {

namespace {
constexpr int kFirstListenFd = 3;  // SD_LISTEN_FDS_START
}

bool addressHasPort(const sockaddr* address, socklen_t length, int port) {
  if (!address || port <= 0) return false;
  if (address->sa_family == AF_INET && length >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
    return ntohs(reinterpret_cast<const sockaddr_in*>(address)->sin_port) == port;
  }
  if (address->sa_family == AF_INET6 && length >= static_cast<socklen_t>(sizeof(sockaddr_in6))) {
    return ntohs(reinterpret_cast<const sockaddr_in6*>(address)->sin6_port) == port;
  }
  return false;
}

int inheritedTcpListener(int port) {
  const char* pid = std::getenv("LISTEN_PID");
  const char* fds = std::getenv("LISTEN_FDS");
  if (!pid || !fds) return -1;
  // The variables are meant for exactly one process; a child that inherited
  // them must not take its parent's sockets.
  if (std::strtol(pid, nullptr, 10) != static_cast<long>(::getpid())) return -1;
  const long count = std::strtol(fds, nullptr, 10);
  for (int fd = kFirstListenFd; fd < kFirstListenFd + count; ++fd) {
    int type = 0;
    socklen_t typeLength = sizeof(type);
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &typeLength) != 0 || type != SOCK_STREAM) {
      continue;
    }
    int listening = 0;
    socklen_t listeningLength = sizeof(listening);
    if (::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &listeningLength) != 0 ||
        !listening) {
      continue;
    }
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) continue;
    if (!addressHasPort(reinterpret_cast<sockaddr*>(&address), length, port)) continue;
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
  }
  return -1;
}

}  // namespace nova::net
