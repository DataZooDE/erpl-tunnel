# Architecture decisions

The decisions the code cites by number. Extracted from the original implementation
plan so that an `ADR-00N` reference in a source comment resolves to something after
that working document was retired.

Windows-specific decisions live in their own file: [ADR-013](ADR-013-windows-mesh.md).

---

## ADR-003 — one uniform port-forward engine, backends behind a `Dial`

`tunnel_create` is backend-agnostic. It routes on the secret's `backend` field and
hands off to a transport that exposes `Dial(host, port) -> stream`; SSH streams are
libssh2 channels, mesh streams come from the shim. Everything above that — the local
listener, the byte pump, `tunnels()`, close semantics — is shared.

*Why:* the alternative was a separate pragma and forwarder per backend, which would
have tripled the surface users learn and the code paths that can drift.

*Cited by:* `src/pragma_tunnel_create.cpp`, `src/tunnel_manager.cpp`.

## ADR-005 — one secret type with a `backend` discriminator

A single `tunnel` secret type carries all backends, selected by
`backend = 'ssh' | 'tailscale' | 'netbird'` (absent means `ssh`). `ssh_tunnel`
remains as an alias so existing `CREATE SECRET (TYPE ssh_tunnel, …)` keeps working.

*Why:* one type to document and one place to enforce redaction, without breaking
the SSH-only secrets that predate the mesh backends.

*Cited by:* `src/tunnel_secret.cpp`.

## ADR-006 — the local listener binds loopback unless told otherwise

`tunnel_create` binds `127.0.0.1`; reaching all interfaces requires an explicit
`bind_all = true`. A tunnel is a hole through a firewall, so the default must be the
one that cannot accidentally expose an internal service to the network.

On Windows this also means `SO_EXCLUSIVEADDRUSE` rather than `SO_REUSEADDR` on the
listener: Windows' `SO_REUSEADDR` lets another local process bind the same address
and hijack connections, which would quietly undo this decision.

*Cited by:* `src/mesh_forwarder.cpp`, `src/tunnel_connection.cpp`.

## ADR-008 — one mesh node per identity, reused across tunnels

A `MeshBackend` is keyed by the secret that configured it and shared by every tunnel
using that secret. `EnsureUp()` is idempotent, so the node enrolls once no matter how
many tunnels are created.

*Why:* enrollment is slow and, on a mesh, each node is a visible peer. One tunnel per
node would spam the tailnet/network with duplicates and pay the enrollment cost every
time.

*Cited by:* `src/mesh_backend.cpp`.

## ADR-011 — one mesh per process (the single-mesh latch)

The first mesh activated in a process wins; a second, *different* mesh is refused
with an actionable message. Not a conservative guardrail: two Go runtimes in one
process is unsupported upstream and corrupts the garbage collector
([golang/go#36628](https://github.com/golang/go/issues/36628),
[#65050](https://github.com/golang/go/issues/65050)), with Windows worst affected.
For the same reason a shim is never unloaded once activated — its Go runtime has
already started threads.

SSH is unaffected and uses no Go.

*See:* [the user-facing guide](../guides/one-mesh-per-process.md).
*Cited by:* `src/mesh_loader.cpp`, `src/mesh_backend.cpp`.

## ADR-012 — backends chosen at build time, one extension name

`MESH_BACKEND=ssh|tailscale|netbird|both` selects which shims are embedded. There is
always exactly one artifact called `erpl_tunnel`; the published build is `both` on
glibc Linux, macOS and Windows, and `ssh` on musl/wasm. Mesh code is compiled out
entirely (`ERPL_TUNNEL_HAS_MESH`) when not bundled, so an SSH-only build carries no
Go at all — not merely unused Go.

Asking for a backend the build does not contain fails with an install hint rather
than a symbol-resolution error.

*See:* [FOOTPRINT.md](FOOTPRINT.md).
