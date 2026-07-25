#pragma once

// MeshExporter — the C++ owner of a mesh-side listener published by tunnel_export.
//
// Deliberately thin. Unlike MeshForwarder, which owns a local socket, an accept
// loop and a byte pump, everything an export actually does happens inside Go: the
// listener, the accepts, the dial to the local service and the copying. This class
// exists only to (a) tie that listener's lifetime to a tunnel id so tunnel_close
// works, and (b) render a row for tunnels(). See shim/mesh_shim.h for why the proxy
// lives in Go — an accepted connection is born there, and surfacing it to C++ would
// need SCM_RIGHTS, which does not exist on Windows.

#include "mesh_backend.hpp"
#include "tunnel_handle.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace duckdb {

class MeshExporter : public TunnelHandle {
public:
    MeshExporter(std::shared_ptr<MeshBackend> backend, int mesh_port,
                 std::string local_host, int local_port);
    ~MeshExporter() override;

    // Bring the node up and start listening. Throws actionably on failure.
    void Start();

    void Close() override;
    TunnelConnectionAttributes GetAttributes() const override;
    // An export with no connections is still healthy — "active" means "listening",
    // otherwise CleanupInactiveTunnels would reap an idle but perfectly good export.
    bool IsActive() const override;

private:
    std::shared_ptr<MeshBackend> backend_;
    long export_handle_{0};
    mutable std::mutex mu_;
    TunnelConnectionAttributes attrs_;
    bool listening_{false};
};

} // namespace duckdb
