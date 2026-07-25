#include "mesh_forwarder.hpp"

#include "duckdb/common/exception.hpp"

#include "socket_compat.hpp"

#include <chrono>
#include <cstring>

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

    EnsureSocketSubsystem();
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (!SocketValid(sock)) {
        throw IOException("Tunnel: could not create listening socket.");
    }
#ifdef _WIN32
    // SO_REUSEADDR on Windows lets another local process hijack the port, which
    // would defeat the loopback-bind posture (FR-2/ADR-006). SO_EXCLUSIVEADDRUSE is
    // the Windows way to say "quick rebind, but exclusively mine".
    SetSocketOptInt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
    SetSocketOptInt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(attrs_.local_port));
    addr.sin_addr.s_addr = inet_addr(attrs_.bind_addr.c_str());
    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        CloseSocketHandle(sock);
        throw IOException("Tunnel: local port " + std::to_string(attrs_.local_port) +
                          " is already in use. Choose a free local_port.");
    }
    if (listen(sock, 16) != 0) {
        CloseSocketHandle(sock);
        throw IOException("Tunnel: failed to listen on port " + std::to_string(attrs_.local_port) + ".");
    }
    // Non-blocking accept so the loop can observe running_ and shut down promptly.
    SetSocketNonBlocking(sock);

    listen_sock_ = sock;
    running_.store(true);
    attrs_.status = "Listening on " + attrs_.bind_addr + ":" + std::to_string(attrs_.local_port);
    accept_thread_ = std::thread([this]() { AcceptLoop(); });

    // Probe: wait until the listener is connectable (FR-6).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        socket_t probe = socket(AF_INET, SOCK_STREAM, 0);
        if (!SocketValid(probe)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        sockaddr_in pa{};
        pa.sin_family = AF_INET;
        pa.sin_port = htons(static_cast<uint16_t>(attrs_.local_port));
        pa.sin_addr.s_addr = inet_addr("127.0.0.1");
        bool ok = (connect(probe, reinterpret_cast<sockaddr *>(&pa), sizeof(pa)) == 0);
        CloseSocketHandle(probe);
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
        int sel = select(SelectNfds(listen_sock_, listen_sock_), &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) {
            continue;
        }
        socket_t client = accept(listen_sock_, nullptr, nullptr);
        if (!SocketValid(client)) {
            continue;
        }
        if (!running_.load()) {
            CloseSocketHandle(client);
            break;
        }
        socket_t mesh_sock = kInvalidSocket;
        try {
            mesh_sock = static_cast<socket_t>(backend_->Dial(attrs_.remote_host, attrs_.remote_port));
        } catch (const std::exception &e) {
            attrs_.error_message = e.what();
            CloseSocketHandle(client);
            continue;
        }
        std::lock_guard<std::mutex> lock(mu_);
        workers_.emplace_back([this, client, mesh_sock]() { Pump(client, mesh_sock); });
    }
}

void MeshForwarder::Pump(socket_t client_sock, socket_t mesh_sock) {
    // Bidirectional byte pump between the local client and the mesh stream. Both
    // are OS sockets, so recv/send (never read/write, which on Windows only work on
    // CRT file descriptors) and a symmetric select loop.
    char buf[16384];
    fd_set rfds;
    while (running_.load()) {
        FD_ZERO(&rfds);
        FD_SET(client_sock, &rfds);
        FD_SET(mesh_sock, &rfds);
        timeval tv{0, 100000};
        int sel = select(SelectNfds(client_sock, mesh_sock), &rfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            break;
        }
        if (sel == 0) {
            continue;
        }
        if (FD_ISSET(client_sock, &rfds)) {
            const int n = SocketRecv(client_sock, buf, sizeof(buf));
            if (n == 0) {
                // The local client is done sending; let the peer see EOF rather
                // than tearing the whole tunnel down mid-response.
                ShutdownWrite(mesh_sock);
                break;
            }
            if (n < 0 || !SendAll(mesh_sock, buf, static_cast<size_t>(n), running_)) {
                break;
            }
        }
        if (FD_ISSET(mesh_sock, &rfds)) {
            const int n = SocketRecv(mesh_sock, buf, sizeof(buf));
            if (n == 0) {
                ShutdownWrite(client_sock);
                break;
            }
            if (n < 0 || !SendAll(client_sock, buf, static_cast<size_t>(n), running_)) {
                break;
            }
        }
    }
    CloseSocketHandle(client_sock);
    CloseSocketHandle(mesh_sock);
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
    if (SocketValid(listen_sock_)) {
        CloseSocketHandle(listen_sock_);
        listen_sock_ = kInvalidSocket;
    }
    attrs_.status = "Closed";
}

TunnelConnectionAttributes MeshForwarder::GetAttributes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return attrs_;
}

} // namespace duckdb
