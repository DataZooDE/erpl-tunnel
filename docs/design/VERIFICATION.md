# What is verified, and where

A single place to answer "is this actually tested?" without reading the CI YAML.
Kept honest on purpose: an empty cell means *not covered*, not *probably fine*.

## Coverage

| | Linux | macOS | Windows |
|---|:--:|:--:|:--:|
| Builds all three backends | ✅ | ✅ | ✅ |
| Extension loads; SQL surface present | ✅ | ✅ | ✅ |
| Stream pair semantics (bytes, half-close, concurrency, leaks) | ✅ | ✅ | ✅ |
| Mesh shim extracts, loads, Go runtime starts | ✅ | ✅ | ✅ |
| Single-mesh latch refuses a second mesh | ✅ | ✅ | ✅ |
| Lazy load — SSH-only sessions map no Go | ✅ | ✅ | — |
| **SSH data plane** — real payload through a tunnel | ✅ | — | — |
| **Mesh data plane** — real WireGuard payload | ✅ | — | — |
| Zero-dependency (loads in a bare container) | ✅ | — | — |

## The gaps, stated plainly

- **No mesh data plane on macOS or Windows.** The Linux data-plane tests exercise
  the shared C++ pump and Go bridge, and the Windows/macOS runtime jobs prove the
  shim loads and Go starts — but no WireGuard payload has crossed a tunnel from a
  Windows or macOS node in CI. Doing it needs a control plane reachable from a
  Windows/macOS runner to a Linux service container, which is impractical as a
  per-PR gate. Treat as a manual/nightly check, not as covered.
- **No SSH data plane on macOS or Windows.** The harness is docker-based and runs
  on the Linux runners only. The SSH path is the same C++ on all platforms and the
  Winsock port is exercised by the build plus the unit tests, but the bytes have
  only flowed on Linux.
- **No lazy-load assertion on Windows.** `lazy_mesh_load.sh` inspects process
  mappings via procfs, with an `lsof` fallback for macOS; the Windows equivalent
  would need `Get-Process ... .Modules`. The Windows runtime job proves activation
  and caching but not the *absence* of Go in an SSH-only session.

## Jobs

`mesh-tests.yml`

| Job | Runner | Proves |
|---|---|---|
| `core-tests` | ubuntu | mesh-peers JSON parser, DuckDB-free |
| `meshpair` | ubuntu, macos, windows | the local stream pair on each OS |
| `ssh-e2e` | ubuntu | real payload through the docker SSH bastion + the four live-SSH tests |
| `zero-dep` | ubuntu | loads in a bare glibc container (NFR-1) |
| `spike` | ubuntu | Go runs under `dlopen` (BRD R1) |
| `spike-windows` | windows | Go runs under `LoadLibrary` from an MSVC host; no non-system DLL deps |
| `blob-embed` | windows | the blob survives static linking as a COFF object |
| `mesh-runtime-windows` | windows | activation inside DuckDB: extract → `%LOCALAPPDATA%` cache → `LoadLibraryW` → Go → latch |
| `mesh-runtime-macos` | macos | the Mach-O equivalent: lazy load + latch |
| `mesh-e2e` | ubuntu | lazy load, latch, real Headscale enrollment |
| `tailscale-dataplane` | ubuntu | real WireGuard payload to a kernel-TUN Tailscale peer |
| `netbird-dataplane` | ubuntu | real WireGuard payload, self-hosted no-IdP NetBird |

`MainDistributionPipeline.yml` builds and tests the release matrix for DuckDB
v1.5.x and v1.4 LTS across linux amd64/arm64/musl, osx amd64/arm64 and
windows amd64. It sets `ERPL_REQUIRE_MESH`, so a Windows build that cannot find a
mingw toolchain fails loudly instead of silently publishing an SSH-only artifact.

## Running it locally

The live tests self-skip without their harness, which is why a green local run
means less than it looks. To actually exercise them:

```sh
make test_up && make e2e && make test_down   # SSH data plane
make lazy_load_test                          # no Go for SSH-only sessions
make latch_test                              # one mesh per process
make ts_dataplane                            # real Tailscale WireGuard payload
make nb_dataplane                            # real NetBird WireGuard payload
make zero_dep                                # bare-container load (needs a release build)
```

A note worth internalising if you develop here: a dev box tends to be *richer* than
CI — a system `duckdb` on `PATH`, system DuckDB headers under `/usr/include` — and
several tests have silently passed locally for the wrong reason as a result. When a
test passes locally but fails in CI, suspect what it resolved against before
suspecting CI.
