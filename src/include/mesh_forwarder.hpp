#pragma once

// MeshForwarder — the port-forward engine for a mesh backend. Binds a loopback
// (default) local TCP listener and, per accepted connection, dials host:port over
// the mesh (MeshBackend::Dial -> fd) and runs a bidirectional fd<->fd pump. Same
// lifecycle contract as the SSH path: loopback-by-default bind (FR-2), tracked
// workers joined on Close (FR-5). This is the mesh half of ADR-003's uniform
// Dial(host,port)->Stream engine; SSH streams are libssh2 channels, mesh streams
// are OS fds, but the accept/forward shape is the same.

#include "mesh_backend.hpp"
#include "tunnel_connection.hpp" // TunnelConnectionAttributes

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace duckdb {

class MeshForwarder {
public:
    MeshForwarder(std::shared_ptr<MeshBackend> backend, std::string remote_host, int remote_port,
                  int local_port, bool bind_all);
    ~MeshForwarder();

    MeshForwarder(const MeshForwarder &) = delete;
    MeshForwarder &operator=(const MeshForwarder &) = delete;

    // Bring the backend up, bind the listener, start accepting. Throws actionably
    // (bad key / control unreachable / port in use). Waits up to timeout_seconds for
    // the listener to become connectable.
    void Start(int timeout_seconds);
    void Close();

    TunnelConnectionAttributes GetAttributes() const;
    bool IsRunning() const { return running_.load(); }

private:
    void AcceptLoop();
    void Pump(int client_fd, int mesh_fd);

    std::shared_ptr<MeshBackend> backend_;
    TunnelConnectionAttributes attrs_;
    int listen_sock_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::thread> workers_;
    mutable std::mutex mu_;
};

} // namespace duckdb
