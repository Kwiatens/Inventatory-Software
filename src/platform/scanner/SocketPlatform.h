// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

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
// Windows sockets are not inherited by default, so no close-on-exec step is needed.
inline NativeSocket createStreamSocket() { return ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); }
inline NativeSocket acceptConnection(NativeSocket listener, sockaddr* address, SocketLength* length) {
  return ::accept(listener, address, length);
}
inline int setSocketNonBlocking(NativeSocket socket, bool enabled) {
  u_long value = enabled ? 1UL : 0UL;
  return ioctlsocket(socket, FIONBIO, &value);
}
// Exclusive use: on Windows SO_REUSEADDR would let another socket bind the port while this one
// listens.
inline int setListenerAddressPolicy(NativeSocket socket) {
  const BOOL exclusive = TRUE;
  return setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                    sizeof(exclusive));
}
inline int socketLastError() { return WSAGetLastError(); }
inline bool socketWouldBlock(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
inline bool socketInterrupted(int error) { return error == WSAEINTR; }
// accept() errors for one aborted connection, not the listener; retry at once.
inline bool socketAcceptRetryable(int error) { return error == WSAEINTR || error == WSAECONNRESET; }
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
// Created close-on-exec so a process forked elsewhere (xdg-open, the updater) cannot inherit the
// listener or a client socket and keep the port bound.
inline NativeSocket createStreamSocket() { return ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP); }
inline NativeSocket acceptConnection(NativeSocket listener, sockaddr* address, SocketLength* length) {
  return ::accept4(listener, address, length, SOCK_CLOEXEC);
}
inline int setSocketNonBlocking(NativeSocket socket, bool enabled) {
  const int flags = fcntl(socket, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(socket, F_SETFL, enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
// SO_REUSEADDR only permits rebinding over TIME_WAIT; an active listener still blocks the port.
inline int setListenerAddressPolicy(NativeSocket socket) {
  const int reuse = 1;
  return setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
}
inline int socketLastError() { return errno; }
inline bool socketWouldBlock(int error) { return error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS; }
inline bool socketInterrupted(int error) { return error == EINTR; }
// accept() errors for one aborted connection, not the listener; retry at once.
inline bool socketAcceptRetryable(int error) { return error == EINTR || error == ECONNABORTED; }
}  // namespace inventatory
#endif
