[![License: BSL 1.1](https://img.shields.io/badge/License-BSL%201.1-blue.svg)](LICENSE)
[![DuckDB](https://img.shields.io/badge/DuckDB-1.5.4+-green.svg)](https://duckdb.org)
[![Status](https://img.shields.io/badge/status-planning-orange.svg)]()

# erpl-tunnel

**A zero-dependency DuckDB extension that tunnels raw TCP — the DuckDB *quack*
remote protocol, SAP RFC, HTTP — over SSH, Tailscale, or NetBird**, with
dead-simple auth (paste one key) and peer discovery, and nothing external to
install.

It extracts erpl's self-contained SSH-tunnel module (`erpl/tunnel/`,
`erpl_tunnel`) into a standalone extension and extends it with two **in-process
mesh-VPN backends** (Tailscale via vendored `tsnet`, NetBird via
`client/embed`) so a DuckDB instance can reach a service behind NAT/firewalls
with a single pasted token and no daemon, no root, no kernel TUN.

```sql
LOAD erpl_tunnel;
CREATE SECRET ts (TYPE tunnel, backend 'tailscale',
                  auth_key 'tskey-auth-…', tags 'tag:duckdb', ephemeral true);
PRAGMA tunnel_create(secret := 'ts',
       remote_host := 'duckdb-eu-shard3', remote_port := 4213, local_port := 9000);
-- now query the peer at localhost:9000
```

## Status

Planning. This repo currently holds the design and the implementation plan; code
lands per the phased plan below.

## Documents

- **[docs/BRD.md](docs/BRD.md)** — Business Requirements (goals, use cases,
  functional/non-functional requirements, locked decisions, risks).
- **[docs/HLD.md](docs/HLD.md)** — High-Level Design (arc42: architecture,
  the mesh-shim C ABI, lazy-`dlopen` + single-mesh latch, ADRs).
- **[docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md)** — phased plan:
  extract & parity → Tailscale + the lazy-load spike → NetBird → polish & release.

## Design in one breath

- **One loadable file, nothing external** — static libssh2/OpenSSL plus the
  selected mesh Go `c-shared` shim(s) embedded and lazily `dlopen`'d only on
  first mesh use. SSH-only sessions load no Go at all.
- **One transport seam** — a C++ `TransportBackend` with a single primitive
  `Dial(host,port)→Stream`; SSH, Tailscale, and NetBird all implement it and
  feed one raw-TCP port-forward engine.
- **One mesh per process** — Tailscale and NetBird are never needed at once, so
  each is its own Go shim behind a single-mesh latch (the two-Go-runtimes clash
  is impossible by construction).
- **Discovery = tags/naming**, peer-local, no control-plane token.

Part of the **erpl** SAP family (`erpl_rfc`, `erpl_odp`, `erpl_bics`,
`erpl_idoc`).

## License

BSL 1.1 (→ MPL 2.0 after the change date) — see [LICENSE](LICENSE). The final
distribution license is an open decision (BRD R2) and gates public release.
