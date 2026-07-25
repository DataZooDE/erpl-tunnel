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

## ADR-014 — export: the mesh proxy lives entirely in Go, no fd crosses the C ABI

`tunnel_export` publishes a local port onto the network. For the mesh backends the
obvious design — accept in Go, hand the connection to C++, pump there — is the one
we deliberately did not take.

That is what `libtailscale` does: it delivers accepted fds to C over a socketpair
using `SCM_RIGHTS`. **`SCM_RIGHTS` has no Windows equivalent.** Adopting it would
have rebuilt exactly the cross-platform problem [ADR-013](ADR-013-windows-mesh.md)
had just escaped, in the one area where Windows had finally been brought level.

An export happens to need no fd passing at all. It is a port-forward from the mesh
to `127.0.0.1:local_port` — both ends are reachable from Go. So Go accepts on the
mesh listener, dials the local service itself, and reuses the existing `bridge()`
byte pump. The C ABI grows two plain synchronous calls carrying only integers and
strings:

```c
int mesh_export(mesh_node node, int mesh_port, const char *local_host,
                int local_port, long *export_out);
int mesh_unexport(mesh_node node, long export_handle);
```

This keeps the C++/Go boundary at the same width it already had, and the mesh
export path is identical on Linux, macOS and Windows.

The cost is that the pump is not shared with the SSH path: `MeshForwarder` (C++) and
`meshpair.Exporter` (Go) implement the same idea twice. That was accepted because
the alternative is a platform-specific fd channel, and because the Go side is
covered by its own `-race` tests (`shim/meshpair/export_test.go`).

**SSH export is unrelated** and uses `libssh2_channel_forward_listen_ex` — a real
`ssh -R`, no Go involved, so it also exists in musl/wasm SSH-only builds. Two
constraints there are worth recording because both fail silently otherwise: the
session must be non-blocking before the accept loop (`forward_accept` otherwise
parks in `_libssh2_wait_socket` forever), and `bound_port == 0` must be treated as
fatal (inbound channels are matched on `(host, port)`, so a listener stuck at port 0
can never receive anything).

*Cited by:* `shim/meshpair/export.go`, `src/mesh_exporter.cpp`, `src/ssh_exporter.cpp`.
