#include "mesh_exporter.hpp"

#include "duckdb/common/exception.hpp"

#include <utility>

namespace duckdb {

MeshExporter::MeshExporter(std::shared_ptr<MeshBackend> backend, int mesh_port,
                           std::string local_host, int local_port)
    : backend_(std::move(backend)) {
    attrs_.backend = (backend_->Kind() == MeshKind::Tailscale) ? "tailscale"
                     : (backend_->Kind() == MeshKind::NetBird)  ? "netbird"
                                                                : "mesh";
    attrs_.direction = "export";
    attrs_.local_host = std::move(local_host);
    attrs_.local_port = local_port;
    // For an export, remote_port is the port peers connect to on our mesh address.
    attrs_.remote_port = mesh_port;
    attrs_.remote_host = ""; // filled in below with our own mesh address, once up
    // bind_addr describes a LOCAL listener, which an export does not have.
    attrs_.bind_addr = "";
    attrs_.status = "Configuring";
}

MeshExporter::~MeshExporter() { Close(); }

void MeshExporter::Start() {
    // Bring the node up first so auth/control failures surface here, in the pragma,
    // rather than as a listener that silently never receives anything.
    backend_->EnsureUp();

    const long handle = backend_->Export(attrs_.remote_port, attrs_.local_host, attrs_.local_port);

    std::lock_guard<std::mutex> lock(mu_);
    export_handle_ = handle;
    listening_ = true;
    // Tell the user the address a peer should actually dial. tunnel_self() carries
    // the node's mesh IP; without surfacing it here the user has to go looking.
    attrs_.status = "Exporting " + attrs_.local_host + ":" + std::to_string(attrs_.local_port) +
                    " on mesh port " + std::to_string(attrs_.remote_port);
}

void MeshExporter::Close() {
    long handle = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!listening_) {
            return; // idempotent
        }
        handle = export_handle_;
        export_handle_ = 0;
        listening_ = false;
    }
    // Outside the lock: this tears down the Go listener and every proxied
    // connection, and must not block anything reading our attributes.
    if (backend_ && handle != 0) {
        backend_->Unexport(handle);
    }
    std::lock_guard<std::mutex> lock(mu_);
    attrs_.status = "Closed";
}

bool MeshExporter::IsActive() const {
    std::lock_guard<std::mutex> lock(mu_);
    return listening_;
}

TunnelConnectionAttributes MeshExporter::GetAttributes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return attrs_;
}

} // namespace duckdb
