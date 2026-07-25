#include "tunnel_manager.hpp"
#include "tunnel_connection.hpp"
#include "tunnel_secret.hpp"
#ifdef ERPL_TUNNEL_HAS_MESH
#include "mesh_backend.hpp"
#include "mesh_forwarder.hpp"
#endif
#include "duckdb/common/exception.hpp"
#include <thread>
#include <chrono>

namespace duckdb {



TunnelManager::TunnelManager() : next_tunnel_id(1) {
}

TunnelManager::~TunnelManager() {
    CloseAllTunnels();
}

int64_t TunnelManager::CreateTunnel(const TunnelAuthParams &auth_params, 
                                   const string &remote_host, int remote_port, int local_port,
                                   const string &ssh_host, int ssh_port,
                                   const string &ssh_user,
                                   int timeout_seconds,
                                   bool bind_all) {
    // Reject unsupported auth up front, before opening a socket, so the failure is
    // fast and actionable rather than surfacing as a generic post-handshake error
    // (FR-8). SSH-agent auth is declared but not implemented.
    if (auth_params.auth_method == kAuthMethodAgent) {
        throw InvalidInputException(
            "Tunnel: SSH agent authentication is not supported. Use auth_method 'key' or "
            "'password' instead (set private_key_path/passphrase, or password, on the secret).");
    }

    int64_t tunnel_id = GenerateTunnelId();

    try {
        // Create tunnel connection
        auto connection = std::make_shared<TunnelConnection>();
        
        // Loopback bind by default; all-interfaces only on explicit opt-in (FR-2).
        connection->SetBindAll(bind_all);

        // Connect to SSH server
        connection->Connect(ssh_host, ssh_port, ssh_user,
                           remote_host, remote_port, local_port);
        
        // Authenticate based on auth method
        bool auth_success = false;
        if (auth_params.auth_method == "password") {
            if (!auth_params.password.empty()) {
                auth_success = connection->AuthenticateWithPassword(auth_params.password);
            }
        } else if (auth_params.auth_method == "key") {
            if (!auth_params.private_key_path.empty()) {
                auth_success = connection->AuthenticateWithKey(auth_params.private_key_path, auth_params.passphrase);
            }
        } else if (auth_params.auth_method == "agent") {
            auth_success = connection->AuthenticateWithAgent();
        }
        
        if (!auth_success) {
            throw IOException("SSH authentication failed: " + connection->GetErrorMessage());
        }
        
        // Start the tunnel worker thread
        connection->StartWorker();
        
        // Give the worker thread a moment to start up
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Wait for the tunnel to be actually ready by testing the connection
        if (!connection->TestTunnelConnection(timeout_seconds)) {
            connection->Close();
            throw IOException(
                "Tunnel creation timed out after " + std::to_string(timeout_seconds) +
                "s waiting for the local listener. Check that local_port is free (not already in "
                "use), that loopback connections aren't blocked, and consider raising timeout.");
        }
        
        // Store the connection (acquire mutex only for this operation)
        {
            std::lock_guard<std::mutex> lock(tunnels_mutex);
            tunnels[tunnel_id] = std::make_shared<SshTunnelHandle>(connection);
        }
        
        return tunnel_id;
        
    } catch (const std::exception &e) {
        throw IOException("Failed to create tunnel: " + string(e.what()));
    }
}

#ifdef ERPL_TUNNEL_HAS_MESH
int64_t TunnelManager::CreateMeshTunnel(std::shared_ptr<MeshBackend> backend,
                                        const string &remote_host, int remote_port, int local_port,
                                        int timeout_seconds, bool bind_all) {
    int64_t tunnel_id = GenerateTunnelId();
    auto fwd = std::make_shared<MeshForwarder>(std::move(backend), remote_host, remote_port,
                                               local_port, bind_all);
    fwd->Start(timeout_seconds); // throws actionably on failure
    {
        std::lock_guard<std::mutex> lock(tunnels_mutex);
        tunnels[tunnel_id] = std::move(fwd);
    }
    return tunnel_id;
}
#endif

#ifdef ERPL_TUNNEL_HAS_MESH
int64_t TunnelManager::CreateMeshExport(std::shared_ptr<MeshBackend> backend, int mesh_port,
                                        const string &local_host, int local_port) {
    int64_t tunnel_id = GenerateTunnelId();
    auto exporter = std::make_shared<MeshExporter>(std::move(backend), mesh_port,
                                                   local_host, local_port);
    exporter->Start(); // throws actionably on failure
    {
        std::lock_guard<std::mutex> lock(tunnels_mutex);
        tunnels[tunnel_id] = std::move(exporter);
    }
    return tunnel_id;
}
#endif

bool TunnelManager::CloseTunnel(int64_t tunnel_id) {
    // One lookup covers SSH, mesh and exported listeners alike.
    std::shared_ptr<TunnelHandle> to_close;
    {
        std::lock_guard<std::mutex> lock(tunnels_mutex);
        auto it = tunnels.find(tunnel_id);
        if (it == tunnels.end()) {
            return false; // unknown id is not an error, just "nothing to do"
        }
        to_close = it->second;
        tunnels.erase(it); // remove first so nothing else can act on it
    }
    // Close outside the lock: Close() joins threads and must not block the manager.
    try {
        to_close->Close();
    } catch (const std::exception &) {
        // Already unregistered; a failure to tear down cleanly must not propagate.
    }
    return true;
}

bool TunnelManager::IsTunnelActive(int64_t tunnel_id) const {
    std::lock_guard<std::mutex> lock(tunnels_mutex);
    auto it = tunnels.find(tunnel_id);
    return it != tunnels.end() && it->second->IsActive();
}

std::vector<std::pair<int64_t, string>> TunnelManager::ListTunnels() const {
    // try_to_lock: listing must never block a create/close.
    std::unique_lock<std::mutex> lock(tunnels_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return {};
    }
    std::vector<std::pair<int64_t, string>> result;
    result.reserve(tunnels.size());
    for (const auto &pair : tunnels) {
        result.emplace_back(pair.first, pair.second->GetStatus());
    }
    return result;
}

std::vector<std::pair<int64_t, TunnelConnectionAttributes>> TunnelManager::ListTunnelsWithDetails() const {
    std::unique_lock<std::mutex> lock(tunnels_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return {};
    }
    std::vector<std::pair<int64_t, TunnelConnectionAttributes>> result;
    result.reserve(tunnels.size());
    for (const auto &pair : tunnels) {
        result.emplace_back(pair.first, pair.second->GetAttributes());
    }
    return result;
}

std::string TunnelManager::GetTunnelStatus(int64_t tunnel_id) const {
    std::lock_guard<std::mutex> lock(tunnels_mutex);
    auto it = tunnels.find(tunnel_id);
    return it == tunnels.end() ? "Not found" : it->second->GetStatus();
}

std::string TunnelManager::GetTunnelError(int64_t tunnel_id) const {
    std::lock_guard<std::mutex> lock(tunnels_mutex);
    auto it = tunnels.find(tunnel_id);
    return it == tunnels.end() ? "Tunnel not found" : it->second->GetErrorMessage();
}

void TunnelManager::CloseAllTunnels() {
    // Move the handles out under the lock, then close them without holding it —
    // Close() joins threads and could otherwise deadlock against anything that
    // needs the manager while shutting down.
    std::vector<std::shared_ptr<TunnelHandle>> to_close;
    {
        std::lock_guard<std::mutex> lock(tunnels_mutex);
        to_close.reserve(tunnels.size());
        for (auto &pair : tunnels) {
            to_close.push_back(pair.second);
        }
        tunnels.clear();
    }
    for (auto &h : to_close) {
        try {
            h->Close();
        } catch (const std::exception &) {
            // Best effort; keep closing the rest.
        }
    }
}

void TunnelManager::CleanupInactiveTunnels() {
    std::lock_guard<std::mutex> lock(tunnels_mutex);
    for (auto it = tunnels.begin(); it != tunnels.end();) {
        // IsActive() means "still doing its job", not "has live connections" — an
        // idle exported listener is healthy and must not be reaped here.
        if (!it->second->IsActive()) {
            it->second->Close();
            it = tunnels.erase(it);
        } else {
            ++it;
        }
    }
}

int64_t TunnelManager::GenerateTunnelId() {
    return next_tunnel_id++;
}

void TunnelManager::RemoveTunnel(int64_t tunnel_id) {
    auto it = tunnels.find(tunnel_id);
    if (it != tunnels.end()) {
        it->second->Close();
        tunnels.erase(it);
    }
}



} // namespace duckdb 