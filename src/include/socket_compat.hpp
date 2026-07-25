#pragma once

// Cross-platform socket primitives shared by the SSH (tunnel_connection) and mesh
// (mesh_forwarder) data planes.
//
// Winsock diverges from BSD sockets in six ways that matter here, and every one of
// them is a silent bug rather than a compile error if you get it wrong:
//   1. SOCKET is an unsigned handle, so the classic `fd < 0` error test is ALWAYS
//      false on Windows — compare against kInvalidSocket instead.
//   2. setsockopt/getsockopt take char*, not void*.
//   3. socket I/O must use recv/send; read/write only work on CRT file descriptors.
//   4. closing a socket is closesocket(), not close().
//   5. non-blocking is ioctlsocket(FIONBIO), there is no fcntl.
//   6. errors come from WSAGetLastError() as WSAE* codes, not errno.
//
// Winsock also needs an explicit per-process startup, which EnsureSocketSubsystem
// handles.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <atomic>
#include <cstddef>
#include <mutex>

namespace duckdb {

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET; // (SOCKET)~0 — NOT -1
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

inline bool SocketValid(socket_t s) {
    return s != kInvalidSocket;
}

inline int LastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// True when a non-blocking connect() reports "in progress / would block".
inline bool SocketConnectInProgress(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return err == EINPROGRESS || err == EWOULDBLOCK;
#endif
}

// True when a non-blocking accept()/recv() reports "nothing available right now".
inline bool SocketWouldBlock(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

inline void CloseSocketHandle(socket_t s) noexcept {
    if (!SocketValid(s)) {
        return;
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

inline bool SetSocketNonBlocking(socket_t s) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// The inverse of SetSocketNonBlocking. Needed because a timed connect() has to be
// non-blocking to be bounded, but the socket is handed to libssh2 afterwards, which
// manages blocking mode itself and expects to start from a blocking socket.
inline bool SetSocketBlocking(socket_t s) {
#ifdef _WIN32
    u_long off = 0;
    return ioctlsocket(s, FIONBIO, &off) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(s, F_SETFL, flags & ~O_NONBLOCK) == 0;
#endif
}

// Wraps the char*/void* optval divergence.
inline bool SetSocketOptInt(socket_t s, int level, int opt, int value) {
#ifdef _WIN32
    return setsockopt(s, level, opt, reinterpret_cast<const char *>(&value), sizeof(value)) == 0;
#else
    return setsockopt(s, level, opt, &value, sizeof(value)) == 0;
#endif
}

// recv/send rather than read/write, and an int length, so these are correct on both
// platforms. Return <0 on error, 0 on orderly shutdown by the peer.
inline int SocketRecv(socket_t s, char *buf, size_t len) {
    return static_cast<int>(recv(s, buf, static_cast<int>(len), 0));
}

inline int SocketSend(socket_t s, const char *buf, size_t len) {
    return static_cast<int>(send(s, buf, static_cast<int>(len), 0));
}

// Write the whole buffer, tolerating short sends. A single send() may transfer
// fewer bytes than asked; treating that as fatal silently drops the remainder and
// corrupts the stream, which is exactly the bug this replaced in the mesh pump.
// Returns false if the socket failed or `running` went false mid-write.
inline bool SendAll(socket_t s, const char *buf, size_t len, const std::atomic<bool> &running);

// Half-close the write side so the peer observes EOF. This is what carries the
// mesh bridge's CloseWrite through to the local client.
inline void ShutdownWrite(socket_t s) noexcept {
#ifdef _WIN32
    shutdown(s, SD_SEND);
#else
    shutdown(s, SHUT_WR);
#endif
}

// select()'s first argument is ignored on Winsock but must be max(fd)+1 on POSIX.
//
// NOTE: on POSIX, FD_SET with a descriptor >= FD_SETSIZE (1024) is undefined
// behaviour, and a busy DuckDB process can exceed that. On Windows FD_SETSIZE (64)
// bounds the COUNT of sockets in a set, not their values, and we only ever place
// one or two. Migrating these loops to poll()/WSAPoll() would remove the POSIX UB
// entirely and is worth doing separately from the Windows port.
// Guard for the UB documented above: on POSIX, FD_SET with a descriptor >=
// FD_SETSIZE writes outside the fd_set. Callers that cannot use poll() yet should
// refuse the socket loudly instead of corrupting the stack.
inline bool SocketFitsInFdSet(socket_t s) {
#ifdef _WIN32
    (void)s; // Winsock bounds the COUNT, not the value, and we add one or two.
    return true;
#else
    return s >= 0 && s < FD_SETSIZE;
#endif
}

inline int SelectNfds(socket_t a, socket_t b) {
#ifdef _WIN32
    (void)a;
    (void)b;
    return 0;
#else
    return (a > b ? a : b) + 1;
#endif
}

// Winsock requires an explicit per-process startup. Deliberately process-wide and
// deliberately never paired with WSACleanup: mesh tunnels and their Go runtime
// outlive any individual connection object, so a matched cleanup on some other
// object's destructor would pull the subsystem out from under them.
inline void EnsureSocketSubsystem() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
    });
#endif
}

inline bool SendAll(socket_t s, const char *buf, size_t len, const std::atomic<bool> &running) {
    size_t sent = 0;
    while (sent < len) {
        if (!running.load()) {
            return false;
        }
        const int n = SocketSend(s, buf + sent, len - sent);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && SocketWouldBlock(LastSocketError())) {
            // WAIT for writability rather than retrying immediately. The socket may
            // be non-blocking (the SSH export sets it so), and a peer that sends
            // faster than the local service reads would otherwise spin a whole core
            // here until the buffer drains. The bounded timeout keeps `running`
            // responsive so shutdown is not delayed by a stalled reader.
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(s, &wfds);
            timeval tv{0, 50000}; // 50ms
            select(SelectNfds(s, s), nullptr, &wfds, nullptr, &tv);
            continue;
        }
        return false;
    }
    return true;
}

} // namespace duckdb
