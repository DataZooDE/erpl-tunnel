#include "mesh_loader.hpp"

#include "duckdb/common/exception.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace duckdb {

namespace {

#if defined(_WIN32)

// Human-readable text for a GetLastError() code, so load failures say what went
// wrong instead of just a number.
std::string Win32ErrorMessage(DWORD code) {
    LPWSTR buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string out;
    if (n != 0 && buf != nullptr) {
        const int need = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
        if (need > 0) {
            out.resize(static_cast<size_t>(need));
            WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), out.data(), need, nullptr, nullptr);
        }
    }
    if (buf != nullptr) {
        LocalFree(buf);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out.empty() ? ("error " + std::to_string(code)) : out;
}

std::string ToUtf8(const std::wstring &w) {
    if (w.empty()) {
        return {};
    }
    const int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(need > 0 ? need : 0), '\0');
    if (need > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), need, nullptr, nullptr);
    }
    return out;
}

// FNV-1a over the blob. This is a cache key, not a security boundary — the bytes
// already came from inside our own artifact. It makes the path self-invalidating
// across extension versions.
std::wstring BlobHashHex(const unsigned char *blob, uint64_t len) {
    uint64_t h = 1469598103934665603ULL;
    for (uint64_t i = 0; i < len; i++) {
        h ^= blob[i];
        h *= 1099511628211ULL;
    }
    wchar_t buf[17];
    swprintf(buf, 17, L"%016llx", static_cast<unsigned long long>(h));
    return std::wstring(buf);
}

// %LOCALAPPDATA%\DataZoo\erpl-tunnel\shims — a normal per-user application cache.
// Deliberately NOT %TEMP%: a loaded DLL cannot be deleted on Windows, so the Unix
// "unlink straight after load" trick is impossible and temp copies would pile up;
// and "write an executable to %TEMP% then immediately load it" is the canonical
// dropper signature that endpoint-protection rules block. %LOCALAPPDATA% is also
// already ACL'd to this user, which is what NFR-5 wants.
std::wstring ShimCacheRoot() {
    PWSTR p = nullptr;
    std::wstring root;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &p)) && p != nullptr) {
        root = p;
    }
    if (p != nullptr) {
        CoTaskMemFree(p);
    }
    if (root.empty()) {
        wchar_t tmp[MAX_PATH + 1];
        const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
        if (n == 0) {
            throw IOException("Tunnel: could not determine a directory for the mesh shim cache.");
        }
        root.assign(tmp, n);
        if (!root.empty() && root.back() == L'\\') {
            root.pop_back();
        }
    }
    return root + L"\\DataZoo\\erpl-tunnel\\shims";
}

void CreateDirectoryTree(const std::wstring &path) {
    for (size_t i = 0; i <= path.size(); i++) {
        if (i == path.size() || path[i] == L'\\') {
            if (i < 3) {
                continue; // skip the drive root, e.g. "C:\"
            }
            const std::wstring part = path.substr(0, i);
            if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                throw IOException("Tunnel: could not create the mesh shim cache directory '" +
                                  ToUtf8(part) + "': " + Win32ErrorMessage(GetLastError()));
            }
        }
    }
}

#endif // _WIN32

} // namespace

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

namespace {

#if defined(_WIN32)
using NativePath = std::wstring;
#else
using NativePath = std::string;
#endif

// Write the embedded blob somewhere the OS loader can load it from, and return
// that path. Unix uses a private temp file that is unlinked immediately after
// loading; Windows uses a content-addressed per-user cache (see ShimCacheRoot).
NativePath ExtractShim(MeshKind kind, const unsigned char *blob, uint64_t len) {
#if defined(_WIN32)
    const std::wstring dir = ShimCacheRoot() + L"\\" + BlobHashHex(blob, len);
    CreateDirectoryTree(dir);
    const std::wstring final_path =
        dir + (kind == MeshKind::Tailscale ? L"\\ts_shim.dll" : L"\\nb_shim.dll");

    // Already cached with the right size? Reuse it. Another process may have it
    // mapped and locked, which is fine and expected.
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(final_path.c_str(), GetFileExInfoStandard, &fad)) {
        const uint64_t have = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        if (have == len) {
            return final_path;
        }
    }

    // Write to a unique temp name, then move into place, so a concurrent process
    // never observes a half-written DLL.
    const std::wstring tmp_path = final_path + L".tmp." + std::to_wstring(GetCurrentProcessId());
    HANDLE fh = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) {
        throw IOException("Tunnel: could not write the mesh shim to '" + ToUtf8(tmp_path) +
                          "': " + Win32ErrorMessage(GetLastError()));
    }
    uint64_t written = 0;
    while (written < len) {
        const DWORD chunk = static_cast<DWORD>((len - written) > (1u << 20) ? (1u << 20) : (len - written));
        DWORD got = 0;
        if (!WriteFile(fh, blob + written, chunk, &got, nullptr) || got == 0) {
            const DWORD e = GetLastError();
            CloseHandle(fh);
            DeleteFileW(tmp_path.c_str());
            throw IOException("Tunnel: failed writing the mesh shim: " + Win32ErrorMessage(e));
        }
        written += got;
    }
    FlushFileBuffers(fh);
    CloseHandle(fh);

    if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD e = GetLastError();
        // A concurrent process may have won the race and already have it mapped,
        // which makes the replace fail. If the destination is correct, that is fine.
        WIN32_FILE_ATTRIBUTE_DATA cur{};
        bool ok = GetFileAttributesExW(final_path.c_str(), GetFileExInfoStandard, &cur) &&
                  (((static_cast<uint64_t>(cur.nFileSizeHigh) << 32) | cur.nFileSizeLow) == len);
        DeleteFileW(tmp_path.c_str());
        if (!ok) {
            throw IOException("Tunnel: could not place the mesh shim at '" + ToUtf8(final_path) +
                              "': " + Win32ErrorMessage(e));
        }
    }
    return final_path;
#else
    char tmpl[] = "/tmp/erpl_tunnel_meshXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        throw IOException("Tunnel: could not create temp file for the mesh shim.");
    }
    // Restrictive perms: only this user may read/execute the extracted library (NFR-5).
    fchmod(fd, 0700);
    {
        std::ofstream out(tmpl, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(blob), static_cast<std::streamsize>(len));
    }
    close(fd);
    (void)kind;
    return std::string(tmpl);
#endif
}

void *LoadShim(const NativePath &path, MeshKind kind) {
#if defined(_WIN32)
    // The shim sits alone in its hash directory, so searching that directory first
    // is both correct and what we want.
    HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (h == nullptr) {
        const DWORD e = GetLastError();
        std::string hint;
        if (e == ERROR_MOD_NOT_FOUND) {
            hint = " (a dependent DLL is missing — the shim must depend only on system DLLs; "
                   "rebuild it with -static-libgcc)";
        }
        throw IOException("Tunnel: failed to load the " + std::string(MeshKindName(kind)) +
                          " shim from '" + ToUtf8(path) + "': " + Win32ErrorMessage(e) + hint);
    }
    return reinterpret_cast<void *>(h);
#else
    void *h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
        // Note: dlerror() clears the error, so it must be read exactly once.
        const char *derr = dlerror();
        std::string err = derr ? derr : "unknown";
        ::remove(path.c_str());
        throw IOException("Tunnel: failed to load the " + std::string(MeshKindName(kind)) +
                          " shim: " + err);
    }
    return h;
#endif
}

void *ResolveSymbol(void *handle, const char *name) {
#if defined(_WIN32)
    void *sym = reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    void *sym = dlsym(handle, name);
#endif
    if (sym == nullptr) {
        // Deliberately no dlclose/FreeLibrary: by this point the shim's Go runtime
        // has started threads, and unmapping it would leave them running in freed
        // pages. A Go c-shared library is loaded for the life of the process.
        throw IOException("Tunnel: mesh shim is missing symbol '" + std::string(name) + "'.");
    }
    return sym;
}

// Unix unlinks the extracted file once it is mapped. Windows cannot delete a
// loaded DLL, and the cache entry is intentionally kept for reuse.
void AfterLoad(const NativePath &path) {
#if defined(_WIN32)
    (void)path;
#else
    ::remove(path.c_str());
#endif
}

} // namespace

const MeshApi &MeshLoader::Activate(MeshKind kind) {
#if !defined(__linux__) && !defined(__APPLE__) && !defined(_WIN32)
    throw NotImplementedException("Tunnel: mesh backends are not supported on this platform.");
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
