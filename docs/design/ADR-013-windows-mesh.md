# ADR-013 — Mesh backends on Windows

Status: accepted (2026-07-25) · supersedes the "Windows deferred" note in BRD NG5/R6

## Context

The mesh backends shipped on glibc Linux and macOS only. `extension_config.cmake`
forced `MESH_BACKEND=ssh` on Windows because the Go `c-shared` shims used an
`AF_UNIX` socketpair and `dlopen`, neither of which exists there.

## Decision 1 — keep Go; a native C/C++ mesh is not viable

Researched before writing any code, and independently seconded by a second model.

Reimplementing Tailscale or NetBird natively means reimplementing their **control
planes** — TS2021/Noise registration, DERP, disco/NAT traversal, peer maps,
MagicDNS; NetBird's gRPC management + signal + ICE. That is a VPN-client project,
not an extension feature, and both are moving targets.

The alternatives do not help:

- `libtailscale`, Tailscale's own C binding, is itself a cgo wrapper over `tsnet`
  and is POSIX-fd/`SCM_RIGHTS`-based; its community Windows port has been open
  since 2024. It also does nothing for NetBird.
- wireguard-nt, kernel WireGuard and boringtun give a **data plane only** — no peer
  discovery, NAT traversal or identity, which is the entire value here.
- ZeroTier `libzt` is genuinely native C++, but it is a **different, incompatible
  network** (it cannot join a tailnet or a NetBird network) and is BSL 1.1.

So: keep the Go shims and make them cross-platform.

## Decision 2 — the stream pair, not a callback ABI

`mesh_dial` hands the C++ pump one end of a local stream pair. On Unix that is an
`AF_UNIX` socketpair, unchanged. Windows has no `socketpair()`, so it uses a
loopback TCP pair.

A `mesh_read`/`mesh_write` callback ABI was considered and **rejected**: blocking
reads would pin C++ threads inside cgo, cancellation and half-close would become
explicit ABI state machinery, and it pays cgo overhead per I/O. Instead the handle
type widened from `int` to `uintptr_t` (`mesh_stream`) so a Win64 `SOCKET` fits.

Four things the Windows pair must get right, all verified by
`shim/meshpair/pair_test.go` on every OS:

1. **C's end is a raw `windows.Socket`, never a `net.Conn`.** `net.FileConn` does
   not accept a Windows SOCKET at all, and any socket the `net` package owns is
   closed when its conn is closed or collected — which would yank the handle out
   from under C.
2. **`SO_EXCLUSIVEADDRUSE` on the listener.** On Windows another local process can
   otherwise bind the same loopback port and steal connections. No Unix analogue.
3. **The accept is gated on the full TCP 4-tuple.** Our socket already owns
   `(127.0.0.1:P → 127.0.0.1:L)` and no second socket can form an identical tuple,
   so an impostor necessarily arrives from a different source port and is dropped.
   This needs no handshake bytes, which keeps the handle we give C free of any
   preamble for C++ to drain.
4. **`TCP_NODELAY` on both ends.** Loopback TCP applies Nagle plus delayed ACK,
   adding ~40 ms stalls to request/response traffic. AF_UNIX has no such behaviour,
   so without this Windows would be mysteriously slower than Unix.

## Decision 3 — embed the blob as a COFF object, not an RCDATA resource

An RCDATA resource is the idiomatic Windows answer and it was tried first. It
**does not survive being linked through a static library**: nothing references the
`.res` symbolically, so the linker never pulls that archive member in. Measured —
`test/win/blob_embed` built the `.rc`, the static lib and the exe, and
`FindResourceW` still returned NULL.

That failure mode is the worst kind: silent and asymmetric. The shipped loadable
DLL would have worked while every mesh SQL test inside `unittest.exe` failed, with
no obvious connection between cause and symptom.

The blob is instead assembled into a **COFF object** with `.incbin` — the same
mechanism the Unix build already uses for ELF and Mach-O. It works because the
object defines the symbols `mesh_loader.cpp` references, which is exactly the
"reason to pull it in" that a resource lacks, and MSVC's `link.exe` accepts a
mingw-produced pure-data object. All three platforms now share one path.

## Consequences

- **Toolchain.** cgo cannot use MSVC, so the shim is built with mingw-w64 while the
  extension stays MSVC; they meet only over a plain C ABI and OS socket handles —
  no allocation crossing the boundary, no `FILE*`, no C++ objects.
  `cmake/bootstrap_mingw.cmake` finds a toolchain and verifies `-dumpmachine`
  really reports a mingw triple, and degrades to an SSH-only build if none exists
  so third-party builders are not blocked. Our own CI sets `ERPL_REQUIRE_MESH` so
  that fallback can never silently publish a mesh-less artifact.
- **Go >= 1.26 is mandatory on Windows.** Go 1.25.x loads a `c-shared` DLL and
  resolves its symbols but never runs `init()`, leaving the runtime dead
  ([golang/go#75949](https://github.com/golang/go/issues/75949), fixed in 1.26).
  The spike therefore asserts Go *behaviour* — that `mesh_new` returns a handle out
  of a package-level map and `mesh_up` produces a Go-formatted error — not merely
  that the library loaded. Never lower the floor.
- **The single-mesh latch is load-bearing**, not a nicety — see
  [ADR-011 / the guide](../guides/one-mesh-per-process.md).
- **Artifact size.** Windows grows to roughly the Linux/macOS footprint. Accepted;
  see [FOOTPRINT.md](FOOTPRINT.md).

## What is verified, and what is not

Verified in CI: both shims build for Windows; the blob survives static linking; Go
runs under an MSVC host; the shim extracts, loads and activates inside DuckDB, with
the latch refusing a second mesh; and the stream pair's semantics on all three OSes.

**Not** verified: a real WireGuard payload from a Windows or macOS node. The Linux
data-plane tests cover that path for the shared C++ and Go code, but cross-OS
runners make a per-PR gate impractical. Tracked as a nightly/manual runbook rather
than silently treated as covered.
