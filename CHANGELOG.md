# Changelog

All notable changes to `erpl_tunnel` are documented here. Versions follow
`vYYYY.MM.DD` (the date the binary set was cut). Same-day re-cuts append `.N`.

Binaries are self-distributed via [get.erpl.io](http://get.erpl.io) for
`{linux_amd64, linux_arm64, osx_amd64, osx_arm64, windows_amd64} ×
{DuckDB v1.4.5, v1.5.5}`. Install with:

```sql
INSTALL erpl_tunnel FROM 'http://get.erpl.io';
LOAD erpl_tunnel;
```

DuckDB must be started with `-unsigned` (or `allow_unsigned_extensions`) — the
self-distributed binaries are not signed with DuckDB's key. Every published
platform carries all three backends. musl and wasm are **not published** — build
from source if you need them (musl is SSH-only).

## v2026.07.26 — first published release: import **and** export

The first binary set published to get.erpl.io.

- **[export]** New `tunnel_export` publishes a local port onto the network so
  peers can reach *in* — the direction the extension never had. Works on
  Tailscale, NetBird and SSH. The headline use is letting a peer run SQL inside
  this DuckDB over the quack protocol, which is covered by an acceptance test on
  both mesh backends (a real peer `ATTACH`es and queries across WireGuard).
  - Mesh export proxies entirely inside Go, so no file descriptor crosses the C
    ABI — `SCM_RIGHTS` has no Windows equivalent (ADR-014).
  - SSH export is a real remote-forward (`ssh -R`) via
    `libssh2_channel_forward_listen_ex`, built unconditionally so it exists in
    musl/wasm SSH-only builds too.
  - Multiple ports may be exported at once; on a mesh they share one identity.
- **[naming]** `tunnel_create` is now `tunnel_import`, with `tunnel_create` kept
  as a deprecated alias. `tunnels()` gains `direction` and `local_host`.
- **[fix]** A tunnel pragma used the secret **named** in `secret=`. The lookup
  matched by scope path, not name, and tunnel secrets carry no scope — so with
  two secrets defined, `secret='a'` could silently use secret `b`, a different
  host with different credentials and possibly a different backend.
- **[fix]** One mesh node per secret. Every `tunnel_self()`/`tunnel_peers()`/
  `tunnel_import`/`tunnel_export` used to build a **new** node and tear it down,
  so `tunnel_self()` reported an address that was not the one an export was
  published on, and each call registered another peer with the control plane.
- **[fix]** Tailscale node tags are actually applied. The `tags` secret field was
  parsed and then never read, so nodes always registered untagged. Nodes now also
  advertise `tag:erpl-tunnel`. Note that tags are *granted* by the control plane
  from the auth key: an untagged key still yields an untagged node.
- **[fix]** NetBird `tunnel_self()` reports the node's overlay address; the shim
  previously hardcoded it empty.
- **[fix]** `TunnelConnection::Close()` freed the libssh2 session before joining
  the worker threads that dereference it. Threads are now joined first.
- **[fix]** libssh2/Winsock init moved to `std::call_once` — destroying one tunnel
  used to tear the library down underneath every other live tunnel.
- **[robustness]** Bounded, non-blocking TCP connect (a blocking `connect()`
  ignored `timeout` entirely), no busy-spin on `EAGAIN`, atomic tunnel ids,
  handles closed outside the manager lock, and port-range validation on export.
- **[docs]** Guides for the SAP gateway pattern (RFC and BW/BICS) and for sharing
  a DuckLake catalog, both over quack.
