#include "mesh_loader.hpp"

#include "duckdb/common/exception.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace duckdb {

// Embedded shim blobs — provided by generated sources (scripts/embed_blob.cmake),
// linked in only for the mesh backends this build bundles (MESH_BACKEND flag).
#ifdef MESH_HAVE_TAILSCALE
extern "C" const unsigned char ts_shim_blob[];
extern "C" const uint64_t ts_shim_blob_len;
#endif
#ifdef MESH_HAVE_NETBIRD
extern "C" const unsigned char nb_shim_blob[];
extern "C" const uint64_t nb_shim_blob_len;
#endif

std::mutex MeshLoader::mutex_;
MeshKind MeshLoader::active_kind_ = MeshKind::None;
MeshApi MeshLoader::api_ = {};
void *MeshLoader::handle_ = nullptr;
std::string MeshLoader::extracted_path_;

const char *MeshKindName(MeshKind kind) {
    switch (kind) {
    case MeshKind::Tailscale:
        return "Tailscale";
    case MeshKind::NetBird:
        return "NetBird";
    default:
        return "none";
    }
}

bool MeshLoader::IsCompiledIn(MeshKind kind) {
    switch (kind) {
    case MeshKind::Tailscale:
#ifdef MESH_HAVE_TAILSCALE
        return true;
#else
        return false;
#endif
    case MeshKind::NetBird:
#ifdef MESH_HAVE_NETBIRD
        return true;
#else
        return false;
#endif
    default:
        return false;
    }
}

MeshKind MeshLoader::ActiveKind() {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_kind_;
}

static void GetBlob(MeshKind kind, const unsigned char *&data, uint64_t &len) {
    data = nullptr;
    len = 0;
#ifdef MESH_HAVE_TAILSCALE
    if (kind == MeshKind::Tailscale) {
        data = ts_shim_blob;
        len = ts_shim_blob_len;
        return;
    }
#endif
#ifdef MESH_HAVE_NETBIRD
    if (kind == MeshKind::NetBird) {
        data = nb_shim_blob;
        len = nb_shim_blob_len;
        return;
    }
#endif
    (void)kind;
}

const MeshApi &MeshLoader::Activate(MeshKind kind) {
#if !defined(__linux__) && !defined(__APPLE__)
    throw NotImplementedException("Tunnel: mesh backends are only supported on Linux and macOS.");
#else
    std::lock_guard<std::mutex> lock(mutex_);

    // Build-time bundle check (FR-27): fail with an install hint, not a dlsym error.
    if (!IsCompiledIn(kind)) {
        throw InvalidInputException(
            std::string("Tunnel: this build does not include the ") + MeshKindName(kind) +
            " backend. Install the erpl_tunnel build with MESH_BACKEND=" +
            (kind == MeshKind::Tailscale ? "tailscale" : "netbird") + " (or 'both').");
    }

    // Single-mesh latch (ADR-011): first mesh activated wins.
    if (active_kind_ != MeshKind::None) {
        if (active_kind_ == kind) {
            return api_; // same mesh, already loaded — idempotent
        }
        throw InvalidInputException(
            std::string("Tunnel: ") + MeshKindName(active_kind_) +
            " is active in this DuckDB process; " + MeshKindName(kind) +
            " cannot be loaded — start a new session for a different mesh.");
    }

    // First activation: extract the embedded blob to a private temp file and dlopen it.
    const unsigned char *blob = nullptr;
    uint64_t blob_len = 0;
    GetBlob(kind, blob, blob_len);
    if (blob == nullptr || blob_len == 0) {
        throw InternalException("Tunnel: mesh shim blob missing for " + std::string(MeshKindName(kind)));
    }

    char tmpl[] = "/tmp/erpl_tunnel_meshXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        throw IOException("Tunnel: could not create temp file for the mesh shim.");
    }
    // Restrictive perms: only this user may read/execute the extracted library (NFR-5).
    fchmod(fd, 0700);
    {
        std::ofstream out(tmpl, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(blob), static_cast<std::streamsize>(blob_len));
    }
    close(fd);

    void *h = dlopen(tmpl, RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
        // Note: dlerror() clears the error, so it must be read exactly once.
        const char *derr = dlerror();
        std::string err = derr ? derr : "unknown";
        ::remove(tmpl);
        throw IOException("Tunnel: failed to load the " + std::string(MeshKindName(kind)) +
                          " shim: " + err);
    }

    // Resolve the C ABI (mesh_shim.h). Any missing symbol is a build error.
    MeshApi api{};
    auto resolve = [&](const char *name) -> void * {
        void *sym = dlsym(h, name);
        if (sym == nullptr) {
            dlclose(h);
            ::remove(tmpl);
            throw IOException("Tunnel: mesh shim is missing symbol '" + std::string(name) + "'.");
        }
        return sym;
    };
    api.kind = reinterpret_cast<int (*)()>(resolve("mesh_kind"));
    api.node_new = reinterpret_cast<long (*)()>(resolve("mesh_new"));
    api.set_str = reinterpret_cast<int (*)(long, const char *, const char *)>(resolve("mesh_set_str"));
    api.set_bool = reinterpret_cast<int (*)(long, const char *, int)>(resolve("mesh_set_bool"));
    api.up = reinterpret_cast<int (*)(long)>(resolve("mesh_up"));
    api.dial = reinterpret_cast<int (*)(long, const char *, int, MeshStream *)>(resolve("mesh_dial"));
    api.peers_json = reinterpret_cast<int (*)(long, char *, size_t, size_t *)>(resolve("mesh_peers_json"));
    api.self_json = reinterpret_cast<int (*)(long, char *, size_t, size_t *)>(resolve("mesh_self_json"));
    api.errmsg = reinterpret_cast<int (*)(long, char *, size_t)>(resolve("mesh_errmsg"));
    api.close = reinterpret_cast<int (*)(long)>(resolve("mesh_close"));

    // Sanity: the shim's self-reported kind must match what we asked for.
    const int reported = api.kind();
    if (reported != static_cast<int>(kind)) {
        dlclose(h);
        ::remove(tmpl);
        throw InternalException("Tunnel: mesh shim kind mismatch (asked " +
                                std::to_string(static_cast<int>(kind)) + ", shim reports " +
                                std::to_string(reported) + ").");
    }

    handle_ = h;
    api_ = api;
    extracted_path_ = tmpl;
    active_kind_ = kind;
    // The extracted file can be unlinked now; the mapping stays valid until process exit.
    ::remove(tmpl);
    return api_;
#endif
}

} // namespace duckdb
