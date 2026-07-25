#include "mesh_forwarder.hpp"

#include "duckdb/common/exception.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace duckdb {

MeshForwarder::MeshForwarder(std::shared_ptr<MeshBackend> backend, std::string remote_host,
                             int remote_port, int local_port, bool bind_all)
    : backend_(std::move(backend)) {
    attrs_.backend = (backend_->Kind() == MeshKind::Tailscale) ? "tailscale"
                     : (backend_->Kind() == MeshKind::NetBird)  ? "netbird"
                                                                : "mesh";
    attrs_.remote_host = std::move(remote_host);
    attrs_.remote_port = remote_port;
    attrs_.local_port = local_port;
    attrs_.bind_addr = bind_all ? "0.0.0.0" : "127.0.0.1";
    attrs_.status = "Configuring";
}

MeshForwarder::~MeshForwarder() { Close(); }

void MeshForwarder::Start(int timeout_seconds) {
    // Bring the mesh node up first — surfaces auth/control errors before we bind.
    backend_->EnsureUp();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        throw IOException("Tunnel: could not create listening socket.");
    }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(attrs_.local_port));
    addr.sin_addr.s_addr = inet_addr(attrs_.bind_addr.c_str());
    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(sock);
        throw IOException("Tunnel: local port " + std::to_string(attrs_.local_port) +
                          " is already in use. Choose a free local_port.");
    }
    if (listen(sock, 16) != 0) {
        close(sock);
        throw IOException("Tunnel: failed to listen on port " + std::to_string(attrs_.local_port) + ".");
    }
    // Non-blocking accept so the loop can observe running_ and shut down promptly.
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    listen_sock_ = sock;
    running_.store(true);
    attrs_.status = "Listening on " + attrs_.bind_addr + ":" + std::to_string(attrs_.local_port);
    accept_thread_ = std::thread([this]() { AcceptLoop(); });

    // Probe: wait until the listener is connectable (FR-6).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        int probe = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in pa{};
        pa.sin_family = AF_INET;
        pa.sin_port = htons(static_cast<uint16_t>(attrs_.local_port));
        pa.sin_addr.s_addr = inet_addr("127.0.0.1");
        bool ok = (connect(probe, reinterpret_cast<sockaddr *>(&pa), sizeof(pa)) == 0);
        close(probe);
        if (ok) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw IOException("Tunnel: listener on port " + std::to_string(attrs_.local_port) +
                      " did not become connectable within " + std::to_string(timeout_seconds) + "s.");
}

void MeshForwarder::AcceptLoop() {
    while (running_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_sock_, &rfds);
        timeval tv{0, 100000}; // 100ms
        int sel = select(listen_sock_ + 1, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) {
            continue;
        }
        int client = accept(listen_sock_, nullptr, nullptr);
        if (client < 0) {
            continue;
        }
        if (!running_.load()) {
            close(client);
            break;
        }
        // MeshStream is an fd here; the Winsock port replaces this with socket_t.
        int mesh_fd = -1;
        try {
            mesh_fd = static_cast<int>(backend_->Dial(attrs_.remote_host, attrs_.remote_port));
        } catch (const std::exception &e) {
            attrs_.error_message = e.what();
            close(client);
            continue;
        }
        std::lock_guard<std::mutex> lock(mu_);
        workers_.emplace_back([this, client, mesh_fd]() { Pump(client, mesh_fd); });
    }
}

void MeshForwarder::Pump(int client_fd, int mesh_fd) {
    // Bidirectional byte pump between the local client and the mesh fd. Both are
    // plain OS fds, so this is a symmetric select/read/write loop.
    char buf[16384];
    fd_set rfds;
    int maxfd = (client_fd > mesh_fd ? client_fd : mesh_fd) + 1;
    while (running_.load()) {
        FD_ZERO(&rfds);
        FD_SET(client_fd, &rfds);
        FD_SET(mesh_fd, &rfds);
        timeval tv{0, 100000};
        int sel = select(maxfd, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            break;
        }
        if (sel == 0) {
            continue;
        }
        if (FD_ISSET(client_fd, &rfds)) {
            ssize_t n = read(client_fd, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            if (write(mesh_fd, buf, static_cast<size_t>(n)) != n) {
                break;
            }
        }
        if (FD_ISSET(mesh_fd, &rfds)) {
            ssize_t n = read(mesh_fd, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            if (write(client_fd, buf, static_cast<size_t>(n)) != n) {
                break;
            }
        }
    }
    close(client_fd);
    close(mesh_fd);
}

void MeshForwarder::Close() {
    running_.store(false);
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto &t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
        workers_.clear();
    }
    if (listen_sock_ >= 0) {
        close(listen_sock_);
        listen_sock_ = -1;
    }
    attrs_.status = "Closed";
}

TunnelConnectionAttributes MeshForwarder::GetAttributes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return attrs_;
}

} // namespace duckdb
