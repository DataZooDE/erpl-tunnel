[![License: BSL 1.1](https://img.shields.io/badge/License-BSL%201.1-blue.svg)](LICENSE)
[![DuckDB](https://img.shields.io/badge/DuckDB-1.4%2B-green.svg)](https://duckdb.org)
[![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)]()

# ERPL Tunnel — reach any TCP service from DuckDB, through SSH or a mesh VPN

**`erpl_tunnel` opens a tunnel from your DuckDB session to a service that lives
behind a firewall or NAT — an SSH bastion, a private HTTP/CSV endpoint, a SAP
gateway, or another DuckDB — and gives you back a plain `localhost:PORT` to point
any client at.** Paste one credential, run one `PRAGMA`, and query as if the
service were local. No daemon, no root, no VPN client to install — everything is
inside the extension.

Three transports, one small SQL surface:

- **SSH** — classic local port-forwarding over a bastion (libssh2). Everywhere.
- **Tailscale** — an in-process `tsnet` node; reach any peer on your tailnet.
- **NetBird** — an in-process `client/embed` node; reach any peer on your network.

```sql
LOAD erpl_tunnel;
CREATE SECRET bastion (TYPE ssh_tunnel, host 'bastion.example.com',
                       user 'jump', password '…');
PRAGMA tunnel_create(secret = 'bastion',
       remote_host = 'sap.internal', remote_port = 3300, local_port = 9001);
-- the private SAP gateway is now at localhost:9001
```

## ✨ Highlights

- **One `localhost:PORT` for anything** — the tunnel forwards raw TCP, so it works
  for SAP RFC, HTTP/CSV over `httpfs`, the DuckDB remote protocol, anything.
- **Paste-one-key setup** — an SSH password/key, a Tailscale auth key, or a NetBird
  setup key. The happy path is three SQL statements.
- **Nothing to install** — libssh2/OpenSSL and the mesh nodes are inside the single
  extension file. No `tailscaled`, no `netbird` daemon, no kernel TUN, no root.
- **Safe by default** — the local listener binds `127.0.0.1`; every secret field is
  redacted; nothing sensitive is logged.
- **Peer discovery** — on a mesh, `tunnel_peers()` lists reachable nodes with no
  control-plane API token.
- **Actionable errors** — every failure tells you the likely cause and the fix.

## 🚀 Install

```sql
INSTALL erpl_tunnel FROM community;
LOAD erpl_tunnel;
```

> **Publication status.** The community-extensions submission is in progress. Until
> it lands, [build from source](#-build-from-source) and `LOAD` the local
> `erpl_tunnel.duckdb_extension` (start DuckDB with `-unsigned`).

Platforms: **Linux** and **macOS** get all three backends (SSH + Tailscale +
NetBird). **Windows** gets SSH tunneling (the mesh backends are Unix-only). One
extension, the right backends for your platform.

## ⚡ Quick start

### 1 · SSH — reach a service behind a bastion

```sql
CREATE SECRET bastion (TYPE ssh_tunnel,
    host 'bastion.example.com', user 'jump', password '…');   -- or private_key_path '…'

PRAGMA tunnel_create(secret = 'bastion',
    remote_host = 'internal-http', remote_port = 8000, local_port = 9000);

-- Now read a private CSV as if it were local:
INSTALL httpfs; LOAD httpfs;
SELECT * FROM read_csv_auto('http://localhost:9000/data.csv');

SELECT * FROM tunnels();        -- what's open
PRAGMA tunnel_close_all;        -- tear everything down
```

### 2 · Tailscale — reach a peer on your tailnet

```sql
-- Generate a reusable, tagged auth key at https://login.tailscale.com/admin/settings/keys
CREATE SECRET ts (TYPE tunnel, backend 'tailscale',
    auth_key 'tskey-auth-…', hostname 'duckdb-eu-1', ephemeral true);

SELECT host_name, mesh_ip, online FROM tunnel_peers(secret = 'ts');  -- who's out there

PRAGMA tunnel_create(secret = 'ts',
    remote_host = 'duckdb-eu-shard3', remote_port = 4213, local_port = 9000);
```

### 3 · NetBird — reach a peer on your NetBird network

```sql
CREATE SECRET nb (TYPE tunnel, backend 'netbird',
    setup_key '…', hostname 'duckdb-eu-1');

PRAGMA tunnel_create(secret = 'nb',
    remote_host = '100.x.y.z', remote_port = 8000, local_port = 9000);
```

More detail per backend, plus how to get the keys and run your own control server:
**[docs/guides/](docs/guides/)** — [getting started](docs/guides/getting-started.md) ·
[Tailscale](docs/guides/tailscale.md) · [NetBird](docs/guides/netbird.md) ·
[discovery](docs/guides/discovery.md) · [security](docs/guides/security.md) ·
[troubleshooting](docs/guides/troubleshooting.md).

## 📖 Reference

### SQL surface

| Function | Kind | What it does |
|---|---|---|
| `tunnel_create(secret, remote_host, remote_port, local_port [, timeout, bind_all])` | pragma | Open a tunnel; returns `(tunnel_id, message)`. |
| `tunnels()` | table | Active local tunnels: `tunnel_id, backend, remote_host, remote_port, local_port, bind_addr, status` (plus `ssh_host/ssh_port/ssh_user` for SSH). |
| `tunnel_close(id)` / `tunnel_close_all` | pragma | Close one / all tunnels (idempotent). |
| `tunnel_peers(secret)` † | table | Mesh peers (peer-local, no API token): `backend, host_name, dns_name, mesh_ip, tags, online`. Tailscale today; NetBird returns an empty set (the embed API exposes no peer list — use `tunnel_self` + the dashboard). |
| `tunnel_self(secret)` † | table | This node's own mesh identity. |
| `tunnel_mesh_activate('tailscale'\|'netbird')` † | pragma | *Advanced* — force-load a mesh backend now. Normally automatic on first `tunnel_create`/`tunnel_peers`; use only to surface auth errors early or pin the one mesh for this process. |

† Present only in **mesh-enabled builds** (the published Linux/macOS artifact); not
on Windows/musl SSH-only builds. Check what your build has with
`SELECT function_name FROM duckdb_functions() WHERE function_name LIKE 'tunnel_%';`.

`tunnel_create` binds `127.0.0.1` by default; pass `bind_all = true` for all
interfaces. `timeout` (seconds) bounds how long it waits for the listener.

### Secret fields

```sql
-- SSH (TYPE ssh_tunnel or TYPE tunnel, backend 'ssh')
CREATE SECRET s (TYPE ssh_tunnel, host '…', port 22, user '…',
    password '…' /* or */ private_key_path '…', passphrase '…');

-- Tailscale (TYPE tunnel, backend 'tailscale')
CREATE SECRET s (TYPE tunnel, backend 'tailscale', auth_key '…',
    hostname '…', tags 'tag:duckdb', control_url '' /* empty = Tailscale cloud */,
    ephemeral true, state_dir '…');

-- NetBird (TYPE tunnel, backend 'netbird')
CREATE SECRET s (TYPE tunnel, backend 'netbird', setup_key '…',
    hostname '…', groups 'duckdb', management_url '' /* empty = NetBird cloud */,
    ephemeral true, state_dir '…');
```

Sensitive fields (`password`, `passphrase`, `private_key_path`, `auth_key`,
`setup_key`) are always redacted in `duckdb_secrets()`.

### One mesh per process

A DuckDB process can run **one** mesh (Tailscale *or* NetBird) at a time — a
Go-runtime constraint. SSH has no such limit and needs no Go. Start a new session
to switch meshes. See [the guide](docs/guides/one-mesh-per-process.md).

## 🔧 Telemetry

Anonymous, opt-out usage telemetry (which backends/functions are used) — no hosts,
keys, ports, or SQL ever leave the machine. Turn it off with
`SET erpl_telemetry_enabled = false;`. Full details: [TELEMETRY.md](TELEMETRY.md).

## 🛠️ Build from source

```sh
git clone --recurse-submodules https://github.com/DataZooDE/erpl-tunnel
cd erpl-tunnel
make release                    # 'both' (SSH+Tailscale+NetBird); bootstraps Go as needed
MESH_BACKEND=ssh make release   # SSH-only, no Go — fastest
```

See [docs/guides/building.md](docs/guides/building.md).

## 📐 Design & development

Part of the **erpl** SAP family (`erpl_rfc`, `erpl_odp`, `erpl_bics`, `erpl_idoc`).
Architecture and the (real-service, no-mock) test story:
[BRD](docs/design/BRD.md) · [HLD](docs/design/HLD.md) ·
[implementation plan](docs/design/IMPLEMENTATION_PLAN.md) ·
[publication plan](docs/design/PUBLICATION_READINESS_PLAN.md).

## License

BSL 1.1 (→ MPL-2.0 after the change date) — see [LICENSE](LICENSE).
