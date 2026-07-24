#pragma once

#include "duckdb.hpp"
#include "tunnel_connection.hpp"
#ifdef ERPL_TUNNEL_HAS_MESH
#include "mesh_forwarder.hpp"
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
    std::unordered_map<int64_t, std::shared_ptr<TunnelConnection>> active_tunnels;
#ifdef ERPL_TUNNEL_HAS_MESH
    std::unordered_map<int64_t, std::shared_ptr<MeshForwarder>> mesh_tunnels;
#endif
    int64_t next_tunnel_id;
    
    // Helper methods
    int64_t GenerateTunnelId();
    void RemoveTunnel(int64_t tunnel_id);
};

// Global tunnel manager instance (defined in tunnel_extension.cpp)
extern std::unique_ptr<TunnelManager> g_tunnel_manager;

} // namespace duckdb 