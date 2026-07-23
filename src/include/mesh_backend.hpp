#pragma once

// MeshBackend — the C++ transport backend that drives whichever mesh shim the
// loader activated, via the C ABI (mesh_shim.h / MeshApi). One node per identity
// (secret), reused across tunnels (ADR-008). Dial returns an OS fd the port-forward
// engine pumps exactly like an SSH channel; Peers/Self return status JSON.

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "mesh_loader.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace duckdb {

class MeshBackend; // defined below

// Per-backend options parsed from a tunnel secret (backend != 'ssh').
struct MeshOptions {
    MeshKind kind = MeshKind::None;
    std::string auth_key;    // tailscale
    std::string setup_key;   // netbird
    std::string hostname;
    std::string tags;        // tailscale
    std::string groups;      // netbird
    std::string control_url; // tailscale (Headscale)
    std::string mgmt_url;    // netbird
    std::string state_dir;
    bool ephemeral = false;
};

// Parse a mesh backend string ('tailscale'|'netbird') to a MeshKind, or None.
MeshKind ParseMeshKind(const std::string &backend);

// The mesh kind declared by a tunnel secret's 'backend' field (None for ssh/absent
// or a missing secret). Cheap: reads the secret, no activation.
MeshKind SecretMeshKind(ClientContext &context, const std::string &secret_name);

// Build (but do not activate) a MeshBackend from a mesh tunnel secret. Throws if the
// secret is missing or not a mesh secret.
std::shared_ptr<MeshBackend> MeshBackendFromSecret(ClientContext &context,
                                                   const std::string &secret_name);

class MeshBackend {
public:
    explicit MeshBackend(MeshOptions opts);
    ~MeshBackend();

    MeshBackend(const MeshBackend &) = delete;
    MeshBackend &operator=(const MeshBackend &) = delete;

    // Activate the shim (lazy dlopen + latch) and bring the node up. Idempotent.
    // Throws an actionable exception on failure (bad key, control unreachable, …).
    void EnsureUp();

    // Dial host:port on the mesh; returns a connected OS fd (caller owns/closes).
    int Dial(const std::string &host, int port);

    // Peer-local status as JSON (array for peers, object for self). Brings the
    // node up if needed.
    std::string PeersJson();
    std::string SelfJson();

    MeshKind Kind() const { return opts_.kind; }

private:
    std::string LastError();

    MeshOptions opts_;
    const MeshApi *api_ = nullptr;
    long node_ = 0;
    bool up_ = false;
    std::mutex mu_;
};

// Registered SQL surface for mesh discovery.
TableFunction CreateTunnelPeersFunction();
TableFunction CreateTunnelSelfFunction();

} // namespace duckdb
