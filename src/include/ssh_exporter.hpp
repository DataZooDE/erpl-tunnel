#pragma once

// SshExporter — `ssh -R` for tunnel_export: ask the bastion to listen on a port and
// forward every connection back to a service running here.
//
// Three things drive this design, all verified against libssh2 1.11.1 rather than
// assumed:
//
//  1. The session must be NON-BLOCKING once the accept loop starts. In blocking
//     mode libssh2_channel_forward_accept parks inside _libssh2_wait_socket with
//     the default infinite timeout, which is unusable under a mutex and makes
//     prompt shutdown impossible. Handshake, auth and forward_listen run blocking
//     (with a timeout) so failures surface in the pragma; everything after is
//     non-blocking and driven by select() on the SSH socket.
//
//  2. ONE dedicated session per export. libssh2 sessions are not thread-safe, and
//     the existing import path calls a blocking libssh2_channel_read; sharing a
//     session between that and an accept loop would deadlock on the first
//     connection. So an export never reuses an import's session.
//
//  3. A keepalive is mandatory. An export is idle by definition until a peer
//     connects, so without one nobody notices a dead session until the moment it
//     is needed — and the sshd used in testing sets ClientAliveInterval 60.
//
// Locking: session_mu_ guards EVERY libssh2 call. It is never held across select()
// or across socket I/O to the local service, or a slow backend would stall the
// accept loop and every other connection sharing the session.

#include "socket_compat.hpp"
#include "tunnel_connection.hpp"
#include "tunnel_handle.hpp"

#include <libssh2.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace duckdb {

class SshExporter : public TunnelHandle {
public:
    SshExporter(TunnelAuthParams auth, std::string local_host, int local_port,
                std::string remote_bind_host, int remote_port);
    ~SshExporter() override;

    // Connect, authenticate and ask the server to bind remote_port. Throws
    // actionably on failure. Returns once the listener is established.
    void Start(int timeout_seconds);

    void Close() override;
    TunnelConnectionAttributes GetAttributes() const override;
    bool IsActive() const override { return running_.load(); }

    // The port the server actually bound. Differs from what was requested when
    // remote_port=0 asked the server to choose.
    int BoundRemotePort() const;

private:
    void AcceptLoop();
    void Pump(LIBSSH2_CHANNEL *channel);
    socket_t DialLocalService();
    void RecordError(const std::string &message);

    TunnelAuthParams auth_;
    std::string remote_bind_host_; // empty => let libssh2 use its default
    int requested_remote_port_{0};

    LIBSSH2_SESSION *session_{nullptr};
    LIBSSH2_LISTENER *listener_{nullptr};
    socket_t ssh_socket_{kInvalidSocket};
    mutable std::mutex session_mu_; // guards every libssh2 call

    mutable std::mutex mu_; // guards attrs_
    TunnelConnectionAttributes attrs_;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    // A finished connection must be reapable, and Close() must be able to join a
    // worker that is itself blocked taking mu_ (RecordError / DialLocalService do).
    // So workers get their OWN mutex, which is never held while joining, and each
    // carries a done-flag the accept loop sweeps — otherwise a long-lived export
    // accumulates one dead std::thread per connection it ever served.
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    mutable std::mutex workers_mu_; // guards workers_ only
    std::vector<Worker> workers_;
    void ReapFinishedWorkers();
};

} // namespace duckdb
