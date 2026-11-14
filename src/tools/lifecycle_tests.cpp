// Unit tests for the idle exit and socket activation (specs/idle-exit.md).
// Run by ctest; no GPU, dashboard or systemd needed.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "net/socket_activation.h"
#include "service/idle_policy.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

void idlePolicy() {
  using nova::service::IdleExitPolicy;
  IdleExitPolicy p;
  p.idleExitSeconds = 300;
  p.start(0);
  check(!p.shouldExit(299, false, false), "not before the idle period");
  check(p.shouldExit(300, false, false), "exits once idle for the whole period");

  p.start(0);
  check(!p.shouldExit(200, true, false), "a viewer is activity");
  check(!p.shouldExit(450, false, false), "the period restarts from the last viewer");
  check(p.shouldExit(500, false, false), "and runs out 300 s after it");

  p.start(0);
  check(!p.shouldExit(400, false, true), "music playing keeps it alive");
  check(p.shouldExit(700, false, false), "and the period starts when the music stops");

  IdleExitPolicy off;
  off.idleExitSeconds = 0;
  off.start(0);
  check(!off.shouldExit(1e9, false, false), "0 disables the exit");

  IdleExitPolicy party;
  party.idleExitSeconds = 300;
  party.housePartyEnabled = true;
  party.start(0);
  check(!party.shouldExit(1e9, false, false), "house party disables the exit");
}

void addressPorts() {
  sockaddr_in v4{};
  v4.sin_family = AF_INET;
  v4.sin_port = htons(8771);
  check(nova::net::addressHasPort(reinterpret_cast<sockaddr*>(&v4), sizeof(v4), 8771),
        "IPv4 address on the port matches");
  check(!nova::net::addressHasPort(reinterpret_cast<sockaddr*>(&v4), sizeof(v4), 8770),
        "IPv4 address on another port does not");
  sockaddr_in6 v6{};
  v6.sin6_family = AF_INET6;
  v6.sin6_port = htons(8771);
  check(nova::net::addressHasPort(reinterpret_cast<sockaddr*>(&v6), sizeof(v6), 8771),
        "IPv6 address on the port matches");
  check(!nova::net::addressHasPort(nullptr, 0, 8771), "no address never matches");
}

void inheritedListener() {
  ::unsetenv("LISTEN_PID");
  ::unsetenv("LISTEN_FDS");
  check(nova::net::inheritedTcpListener(8771) == -1, "no LISTEN_FDS means bind as before");

  // A listening socket at descriptor 3, as systemd would pass it.
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  ::listen(listener, 1);
  socklen_t length = sizeof(address);
  ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
  const int port = ntohs(address.sin_port);
  ::dup2(listener, 3);

  ::setenv("LISTEN_PID", std::to_string(::getpid()).c_str(), 1);
  ::setenv("LISTEN_FDS", "1", 1);
  check(nova::net::inheritedTcpListener(port) == 3, "adopts the passed listener by port");
  check(nova::net::inheritedTcpListener(port + 1) == -1, "ignores a listener on another port");

  ::setenv("LISTEN_PID", std::to_string(::getpid() + 1).c_str(), 1);
  check(nova::net::inheritedTcpListener(port) == -1, "ignores sockets meant for another process");
  ::unsetenv("LISTEN_PID");
  ::unsetenv("LISTEN_FDS");
}

}  // namespace

int main() {
  idlePolicy();
  addressPorts();
  inheritedListener();
  std::printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
