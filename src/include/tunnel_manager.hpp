#pragma once

#include "duckdb.hpp"
#include "tunnel_connection.hpp"
#include <atomic>

#include "tunnel_handle.hpp"
#include "ssh_exporter.hpp"
#ifdef ERPL_TUNNEL_HAS_MESH
#include "mesh_forwarder.hpp"
#include "mesh_exporter.hpp"
#endif
#include <unordered_map>
#include <mutex>
#include <memory>

namespace duckdb {

#ifdef ERPL_TUNNEL_HAS_MESH
class MeshBackend; // forward decl for the mesh-tunnel factory
#endif

/**
 * @brief Manages multiple SSH tunnel connections.
 * 
 * This class provides thread-safe management of multiple tunnel connections,
 * including creation, monitoring, and cleanup of tunnels.
 */
class TunnelManager {
public:
    TunnelManager();
    ~TunnelManager();

    // Tunnel management
    int64_t CreateTunnel(const TunnelAuthParams &auth_params,
                         const string &remote_host, int remote_port, int local_port,
                         const string &ssh_host, int ssh_port,
                         const string &ssh_user,
                         int timeout_seconds = 60,
                         bool bind_all = false);
#ifdef ERPL_TUNNEL_HAS_MESH
    // Create a tunnel whose transport is a mesh backend (tailscale/netbird). The
    // backend is dialed per connection; shares the tunnel id space with SSH tunnels.
    int64_t CreateMeshTunnel(std::shared_ptr<MeshBackend> backend,
                             const string &remote_host, int remote_port, int local_port,
                             int timeout_seconds = 60, bool bind_all = false);
#endif
#ifdef ERPL_TUNNEL_HAS_MESH
    // Publish local_host:local_port on the mesh at mesh_port. Shares the tunnel id
    // space with imports, so tunnel_close/tunnels() treat both alike.
    int64_t CreateMeshExport(std::shared_ptr<MeshBackend> backend, int mesh_port,
                             const string &local_host, int local_port);
#endif
    // SSH remote-forward: ask the server to bind remote_port and forward back to
    // local_host:local_port. remote_port 0 lets the server choose.
    int64_t CreateSshExport(const TunnelAuthParams &auth_params,
                            const string &local_host, int local_port,
                            const string &remote_bind_host, int remote_port,
                            int timeout_seconds = 60);
    // The port actually bound (matters when remote_port was 0).
    int GetTunnelBoundPort(int64_t tunnel_id) const;
    bool CloseTunnel(int64_t tunnel_id);
    bool IsTunnelActive(int64_t tunnel_id) const;
    
    // Tunnel information
    std::vector<std::pair<int64_t, string>> ListTunnels() const;
    std::vector<std::pair<int64_t, TunnelConnectionAttributes>> ListTunnelsWithDetails() const;
    std::string GetTunnelStatus(int64_t tunnel_id) const;
    std::string GetTunnelError(int64_t tunnel_id) const;
    
    // Cleanup
    void CloseAllTunnels();
    void CleanupInactiveTunnels();

private:
    mutable std::mutex tunnels_mutex;
    // ONE map over TunnelHandle, not one per transport. Two maps meant six of the
    // methods below only ever consulted the SSH one, so mesh tunnels were invisible
    // to IsTunnelActive/ListTunnels/GetTunnelStatus/GetTunnelError/
    // CleanupInactiveTunnels/RemoveTunnel. Keep it one map.
    std::unordered_map<int64_t, std::shared_ptr<TunnelHandle>> tunnels;
    // Atomic: ids are handed out from Create*() without holding tunnels_mutex, so a
    // plain ++ let two concurrent connections receive the SAME id — the second
    // map insert then silently replaced (and destroyed) the first tunnel.
    std::atomic<int64_t> next_tunnel_id;
    
    // Helper methods
    int64_t GenerateTunnelId();
    void RemoveTunnel(int64_t tunnel_id);
};

// Global tunnel manager instance (defined in tunnel_extension.cpp)
extern std::unique_ptr<TunnelManager> g_tunnel_manager;

} // namespace duckdb 