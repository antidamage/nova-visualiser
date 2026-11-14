#pragma once

#include <sys/socket.h>

// systemd socket activation (specs/idle-exit.md): nova-visualiser.socket holds
// the control port and starts the service on the first connection; the service
// adopts that listening socket instead of binding its own.

namespace nova::net {

// True when `address` is an IPv4 or IPv6 address on `port`.
bool addressHasPort(const sockaddr* address, socklen_t length, int port);

// The listening TCP socket systemd passed for `port` (LISTEN_PID/LISTEN_FDS,
// descriptors from 3), or -1 when there is none -- run by hand or in a test,
// where the caller binds as before. The returned descriptor is marked
// close-on-exec and owned by the caller.
int inheritedTcpListener(int port);

}  // namespace nova::net
