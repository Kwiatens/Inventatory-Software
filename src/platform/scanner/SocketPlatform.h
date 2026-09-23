// Small Winsock/POSIX socket compatibility boundary for the Scan R1 listener.
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

namespace inventatory {
using NativeSocket = SOCKET;
using SocketLength = int;
inline constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
inline constexpr int kSocketShutdownBoth = SD_BOTH;
inline constexpr int kSocketSendFlags = 0;

inline int closeSocket(NativeSocket socket) { return closesocket(socket); }
inline int setSocketNonBlocking(NativeSocket socket, bool enabled) {
  u_long value = enabled ? 1UL : 0UL;
  return ioctlsocket(socket, FIONBIO, &value);
}
inline int socketLastError() { return WSAGetLastError(); }
inline bool socketWouldBlock(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
}  // namespace inventatory
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace inventatory {
using NativeSocket = int;
using SocketLength = socklen_t;
inline constexpr NativeSocket kInvalidSocket = -1;
inline constexpr int kSocketShutdownBoth = SHUT_RDWR;
inline constexpr int kSocketSendFlags = MSG_NOSIGNAL;

inline int closeSocket(NativeSocket socket) { return close(socket); }
inline int setSocketNonBlocking(NativeSocket socket, bool enabled) {
  const int flags = fcntl(socket, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(socket, F_SETFL, enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
inline int socketLastError() { return errno; }
inline bool socketWouldBlock(int error) { return error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS; }
}  // namespace inventatory
#endif
