#pragma once

// 把 Windows / POSIX 的 socket 差异收在一个头文件里，
// 上层只认 socket_t / kInvalidSocket / sock_read 这几个名字。

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>

using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;

inline int sock_close(socket_t s) { return closesocket(s); }
inline int sock_last_error() { return WSAGetLastError(); }
inline bool sock_would_block() {
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK;
}
inline int sock_read(socket_t s, char* buf, int len) {
    return ::recv(s, buf, len, 0);
}
inline int sock_write(socket_t s, const char* buf, int len) {
    return ::send(s, buf, len, 0);
}
inline bool sock_set_nonblocking(socket_t s) {
    u_long mode = 1;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
}
inline bool net_init() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}
inline void net_cleanup() { WSACleanup(); }
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

using socket_t = int;
constexpr socket_t kInvalidSocket = -1;

inline int sock_close(socket_t s) { return ::close(s); }
inline int sock_last_error() { return errno; }
inline bool sock_would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}
inline int sock_read(socket_t s, char* buf, int len) {
    return static_cast<int>(::recv(s, buf, static_cast<size_t>(len), 0));
}
inline int sock_write(socket_t s, const char* buf, int len) {
    return static_cast<int>(::send(s, buf, static_cast<size_t>(len), MSG_NOSIGNAL));
}
inline bool sock_set_nonblocking(socket_t s) {
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
inline bool net_init() { return true; }
inline void net_cleanup() {}
#endif
