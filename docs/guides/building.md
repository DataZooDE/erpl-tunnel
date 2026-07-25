# Building from source

```sh
git clone --recurse-submodules https://github.com/DataZooDE/erpl-tunnel
cd erpl-tunnel
make release
```

That produces the `both` extension (SSH + Tailscale + NetBird) at
`build/release/repository/<duckdb-version>/<platform>/erpl_tunnel.duckdb_extension`.

## Choosing the backend set

`MESH_BACKEND` selects which transports are bundled:

```sh
MESH_BACKEND=ssh       make release   # SSH only — no Go, smallest, fastest build
MESH_BACKEND=tailscale make release   # SSH + Tailscale
MESH_BACKEND=netbird   make release   # SSH + NetBird
MESH_BACKEND=both      make release   # all three (the published default)
```

The published extension is `both` on Linux/macOS/Windows and `ssh` on musl/wasm —
selected automatically per platform (`extension_config.cmake`). Override anytime
with the env var above.

## Requirements

- **CMake + vcpkg** (libssh2, OpenSSL) and a C++17 toolchain.
- **Go** — only for mesh builds. You don't need it preinstalled: the build
  **bootstraps** a pinned, checksum-verified Go if your system Go is missing or too
  old (`cmake/bootstrap_go.cmake`). SSH-only builds need no Go at all.
- **glibc Linux, macOS or Windows** for the mesh backends. musl is unsupported
  (Go cgo `c-shared` needs glibc), so musl and wasm build SSH-only.
- **Windows additionally needs a mingw-w64 gcc** — cgo cannot use MSVC, so the Go
  shims are built with mingw while the extension itself stays MSVC; the two meet
  only over a plain C ABI and OS socket handles. The build finds a toolchain
  automatically (or set `ERPL_MINGW_CC`), and falls back to an SSH-only build with
  a warning if none is present.

## Testing

```sh
make core_tests        # fast, DuckDB-free C++ unit tests (no docker)
make test_up           # bring up the docker SSH + HTTP test services
make sql_tests         # sqllogictests
make e2e               # real HTTP payload through an SSH tunnel
```

Mesh data-plane tests (real Headscale / self-hosted NetBird, no mocks) live in
`test/integration/`; see the Makefile targets `mesh_e2e`, `latch_test`,
`ts_dataplane`, `nb_dataplane`, `lazy_load_test`, `zero_dep`.
