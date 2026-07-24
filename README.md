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

Working across all three backends. SSH is full parity with erpl plus the defect
fixes; Tailscale enrolls against a real control server and forwards traffic;
NetBird shares the identical shim ABI behind the single-mesh latch. Test matrix
(all real services, no mocks):

| Proof | How | State |
|-------|-----|-------|
| SSH tunnel forwards real HTTP | docker sshd + private HTTP service | ✅ |
| erpl defects fixed (loopback bind, thread join, agent-auth, missing-secret, DNS) | sqllogictest + `ss` | ✅ |
| Go-in-`dlopen` works | `shim/spike_dlopen.c` | ✅ |
| SSH-only load maps **no Go**; mesh shim `dlopen`'d lazily on first use | `/proc/pid/maps` assertion | ✅ |
| Real Tailscale enrollment + `tunnel_create` over the tailnet | hermetic **Headscale** | ✅ |
| NetBird shim, **identical C ABI** | `nm` + spike | ✅ |
| **Single-mesh latch** (one mesh per process, symmetric) | `both` build, real dlopen | ✅ |
| NetBird AGPL clearance (R4) | `go list -deps` audit | ✅ |
| Real Tailscale **data plane** to an official kernel-TUN peer | Headscale + `tailscale` daemon, direct WireGuard | ✅ |
| Real NetBird **data plane** to an official kernel-TUN peer | no-IdP self-hosted mgmt/signal/relay (seeded store), direct WireGuard | ✅ |

See [`docs/IMPLEMENTATION_PLAN.md`](docs/IMPLEMENTATION_PLAN.md) for the phased plan
and [`docs/NETBIRD_AGPL_AUDIT.md`](docs/NETBIRD_AGPL_AUDIT.md) for R4.

## Documents

- **[docs/BRD.md](docs/BRD.md)** — Business Requirements (goals, use cases,
  functional/non-functional requirements, locked decisions, risks).
- **[docs/HLD.md](docs/HLD.md)** — High-Level Design (arc42: architecture,
  the mesh-shim C ABI, lazy-`dlopen` + single-mesh latch, ADRs).
- **[docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md)** — phased plan:
  extract & parity → Tailscale + the lazy-load spike → NetBird → polish & release.

## Usage

```sql
LOAD erpl_tunnel;

-- SSH (parity with erpl, plus loopback-default bind and clean teardown)
CREATE SECRET s (TYPE ssh_tunnel, ssh_host 'bastion', ssh_user 'jump', password '…');
PRAGMA tunnel_create(secret='s', remote_host='sap.internal', remote_port='3300',
                     local_port='9001');            -- → localhost:9001

-- Tailscale (in-process tsnet; control_url empty = Tailscale cloud, or a Headscale URL)
CREATE SECRET ts (TYPE tunnel, backend 'tailscale', auth_key 'tskey-auth-…',
                  hostname 'duckdb-eu-1', ephemeral true);
SELECT * FROM tunnel_self(secret='ts');             -- this node's name / mesh IP
SELECT * FROM tunnel_peers(secret='ts');            -- peer-local discovery, no API token
PRAGMA tunnel_create(secret='ts', remote_host='duckdb-eu-shard3', remote_port='4213',
                     local_port='9000');

-- NetBird (in-process client/embed; management_url empty = NetBird cloud)
CREATE SECRET nb (TYPE tunnel, backend 'netbird', setup_key '…', hostname 'duckdb-eu-1');

SELECT * FROM tunnels();                             -- active local tunnels
PRAGMA tunnel_close(1);
PRAGMA tunnel_close_all;
```

Local listeners bind `127.0.0.1` by default; pass `bind_all:=true` to opt into all
interfaces. Build variants: `MESH_BACKEND=ssh|tailscale|netbird|both` — `ssh`
carries **no Go** at all (smallest); each bundled mesh shim adds ~25–45 MB and is
`dlopen`'d only when that backend is first used. One mesh is live per process.

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
