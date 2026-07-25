# Footprint & zero-dependency notes (NFR-1, NFR-7, NFR-8)

## Artifact sizes (release, linux/amd64, DuckDB v1.5.4)

| Build (`MESH_BACKEND=`) | Loadable extension | Notes |
|-------------------------|--------------------|-------|
| `ssh` (default)         | ~49 MB             | static libssh2 + OpenSSL; **no Go** |
| `tailscale`             | ssh + ~34 MB blob  | embeds `ts_shim` (tsnet) |
| `netbird`               | ssh + ~57 MB blob  | embeds `nb_shim` (client/embed) |
| `both`                  | ssh + ~91 MB blobs | runtime picks one via the latch |

Each bundled mesh shim adds its `.so` as an embedded byte blob (NFR-8: ~25–45 MB
budget per mesh; NetBird is at the high end). The blob is **not mapped** until the
backend is first activated — SSH-only sessions on a mesh build pay zero Go cost.

## Zero-dependency (NFR-1 / M5)

The shipped file is self-contained. `ldd` on the loadable extension shows only the
C/C++ runtime:

```
libstdc++.so.6, libm.so.6, libgcc_s.so.1, libc.so.6, ld-linux
```

No `libssh2`, `libssl`, `libcrypto`, `libgo`, `tailscale`, or `netbird` — all
statically linked or embedded. Verified end-to-end by
`test/integration/zero_dependency.sh` (`make zero_dep`): the extension loads and its
functions work inside a **network-isolated** (`--network none`) container that has
no Go toolchain and no mesh daemon installed.

## Portability (NFR-7)

- **glibc only.** Go's cgo `c-shared` runtime requires glibc + the dynamic loader;
  musl/Alpine static is explicitly unsupported (BRD C3/R1).
- **glibc floor.** A build's minimum glibc is set by the toolchain it is compiled
  on. Release CI builds on the `extension-ci-tools` manylinux image (old glibc) for
  broad compatibility. A local build on a rolling distro inherits that distro's
  (newer) glibc floor — so the local `zero_dep` test uses a base image whose glibc
  matches (override with `BASE_IMAGE=`); CI's manylinux artifact runs anywhere.
- **Platforms.** linux/amd64, linux/arm64, osx/amd64, osx/arm64 and
  windows/amd64 all ship the mesh backends. musl and wasm are SSH-only (Go cgo
  `c-shared` needs glibc). Windows is no longer deferred — see below.
- **Windows.** The shim is a `.dll` built with mingw-w64 (cgo cannot use MSVC)
  while the extension itself stays MSVC; they meet only over a plain C ABI and OS
  socket handles. Built with `-static-libgcc`, so `dumpbin /dependents` shows only
  `KERNEL32` plus the `api-ms-win-crt-*` UCRT forwarders — no libgcc,
  libwinpthread or libstdc++. That matters because the shim is extracted **alone**
  into its cache directory, with nothing beside it to satisfy a dependency.
- **Windows shim cache.** A loaded DLL cannot be deleted on Windows, so the Unix
  "unlink immediately after mapping" trick is impossible. The shim is instead
  written to a content-addressed per-user cache,
  `%LOCALAPPDATA%\DataZoo\erpl-tunnel\shims\<hash>\{ts,nb}_shim.dll`, and
  reused across runs. Deliberately not `%TEMP%`: writing an executable there and
  immediately loading it is the canonical dropper pattern that endpoint protection
  blocks, and `%LOCALAPPDATA%` is already per-user ACL'd (which is what NFR-5
  wanted the 0700 temp file for on Unix).
- macOS embedded `.dylib` must be code-signed to `dlopen` under Gatekeeper
  (BRD R7) — handled at build/sign time.

## One mesh per process

Only one mesh shim is ever `dlopen`'d per process (the single-mesh latch, ADR-011),
so only one Go runtime is ever loaded and the two-Go-runtimes crash is impossible by
construction. To use a different mesh, start a new DuckDB session.
