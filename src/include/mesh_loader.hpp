#pragma once

// Lazy mesh-shim loader + process-global single-mesh latch (HLD §6.5, ADR-011).
//
// A mesh backend's Go c-shared shim is embedded in the extension as a byte blob
// and is NOT loaded at extension load. On the first activation of a mesh backend,
// MeshLoader extracts the matching blob to a 0700 temp file, dlopen's it, and
// resolves the mesh_* C ABI symbols (mesh_shim.h). A process-global latch records
// which mesh kind is active: the first mesh activated wins; a second, different
// mesh in the same process is refused with an actionable message. SSH never
// reaches this loader, so SSH-only sessions map no Go into the process (FR-24).

#include "duckdb.hpp"
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace duckdb {

enum class MeshKind { None = 0, Tailscale = 1, NetBird = 2 };

const char *MeshKindName(MeshKind kind);

// An OS stream handle from the shim: a file descriptor on Unix, a Winsock SOCKET
// on Windows. uintptr_t because a Win64 SOCKET is UINT_PTR and will not fit an
// int. Mirrors `mesh_stream` in shim/mesh_shim.h.
using MeshStream = uintptr_t;
static constexpr MeshStream kMeshStreamInvalid = static_cast<MeshStream>(-1);

// Resolved C ABI function pointers from a loaded shim (mesh_shim.h).
struct MeshApi {
    int (*kind)();
    long (*node_new)();
    int (*set_str)(long, const char *, const char *);
    int (*set_bool)(long, const char *, int);
    int (*up)(long);
    int (*dial)(long, const char *, int, MeshStream *);
    // Inbound: publish local_host:local_port on the mesh at mesh_port. Returns an
    // opaque export handle; no stream crosses the ABI (the proxy lives in Go).
    int (*mesh_export)(long, int, const char *, int, long *);
    int (*mesh_unexport)(long, long);
    int (*peers_json)(long, char *, size_t, size_t *);
    int (*self_json)(long, char *, size_t, size_t *);
    int (*errmsg)(long, char *, size_t);
    int (*close)(long);
};

class MeshLoader {
public:
    // Activate the given mesh kind, loading its shim on first use. Throws an
    // actionable exception if:
    //   - the kind is not compiled into this build (MESH_BACKEND flag), or
    //   - a different mesh is already active in this process (single-mesh latch), or
    //   - the shim blob fails to extract / dlopen / resolve.
    // Returns the resolved API (owned by the loader; valid for process lifetime).
    static const MeshApi &Activate(MeshKind kind);

    // The mesh kind currently latched in this process (None if none yet).
    static MeshKind ActiveKind();

    // True if this build bundled the shim for `kind` (per MESH_BACKEND).
    static bool IsCompiledIn(MeshKind kind);

private:
    static std::mutex mutex_;
    static MeshKind active_kind_;
    static MeshApi api_;
    static void *handle_;
    static std::string extracted_path_;
};

} // namespace duckdb
