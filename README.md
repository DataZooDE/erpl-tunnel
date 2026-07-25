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

Platforms: **Linux**, **macOS** and **Windows** all get the three backends
(SSH + Tailscale + NetBird). musl and wasm builds are SSH-only. One extension,
the right backends for your platform.

## ⚡ Quick start

### 1 · SSH — reach a service behind a bastion

```sql
CREATE SECRET bastion (TYPE ssh_tunnel,
    host 'bastion.example.com', user 'jump', password '…');   -- or private_key_path '…'

PRAGMA tunnel_import(secret = 'bastion',
    remote_host = 'internal-http', remote_port = 8000, local_port = 9000);

-- Now read a private CSV as if it were local:
INSTALL httpfs; LOAD httpfs;
SELECT * FROM read_csv_auto('http://localhost:9000/data.csv');

SELECT * FROM tunnels();        -- what's open
PRAGMA tunnel_close_all;        -- tear everything down
```

### 2 · Tailscale — reach a peer on your tailnet

Get an auth key: <https://login.tailscale.com/admin/settings/keys> → **Generate
auth key…** → turn on **Reusable** and **Ephemeral**, leave **Tags** empty for a
first run (a tagged key is rejected without a matching `tagOwners` ACL entry).
Copy it — it is shown once. Full walkthrough: [Tailscale guide](docs/guides/tailscale.md).

```sql
CREATE SECRET ts (TYPE tunnel, backend 'tailscale',
    auth_key 'tskey-auth-…', hostname 'duckdb-eu-1', ephemeral true);

SELECT host_name, mesh_ip, online FROM tunnel_peers(secret = 'ts');  -- who's out there

PRAGMA tunnel_import(secret = 'ts',
    remote_host = 'duckdb-eu-shard3', remote_port = 4213, local_port = 9000);
```

### 3 · NetBird — reach a peer on your NetBird network

Get a setup key: <https://app.netbird.io> → **Setup Keys** → **Create Setup Key**
→ make it **Reusable**, optionally auto-assign a group like `duckdb`. Copy it — it
is shown once. Then check **Access Control → Policies** actually allows that group
to reach the peer: NetBird is default-deny, so a peer can read *Connected* and
still be unreachable. Full walkthrough: [NetBird guide](docs/guides/netbird.md).

```sql
CREATE SECRET nb (TYPE tunnel, backend 'netbird',
    setup_key 'A2C8E62B-38F5-…', hostname 'duckdb-eu-1');

PRAGMA tunnel_import(secret = 'nb',
    remote_host = '100.x.y.z', remote_port = 8000, local_port = 9000);
```

### 4 · Export — turn this DuckDB into a SAP gateway others can query

The three examples above *consume* a service. The reverse is often more useful:
put one DuckDB next to SAP — the box that has the NetWeaver RFC SDK, the SAP
credentials and the network path — and let colleagues query it from anywhere on
the mesh. They need no SAP account, no SDK and no route to the SAP network.

**On the SAP-adjacent node**, expose exactly what you want to share as views, then
publish the DuckDB protocol port onto the tailnet:

```sql
INSTALL 'erpl' FROM 'http://get.erpl.io'; LOAD 'erpl';
CREATE SECRET sap (TYPE sap_rfc, ASHOST 'sap.internal', SYSNR '00',
    CLIENT '100', USER 'DEVELOPER', PASSWD '…', LANG 'EN');

-- Share views, not raw access: this is the contract your peers see.
CREATE VIEW flights AS SELECT * FROM sap_read_table('SFLIGHT', SECRET='sap');

INSTALL quack; LOAD quack;
CALL quack_serve('quack:127.0.0.1:9494', token => 'a-long-shared-token',
    allow_other_hostname => true);

CREATE SECRET ts (TYPE tunnel, backend 'tailscale', auth_key 'tskey-auth-…',
    hostname 'duckdb-sap-gw', ephemeral true);
PRAGMA tunnel_export(secret = 'ts', local_port = 9494);

SELECT mesh_ip FROM tunnel_self(secret = 'ts');   -- the address to share
```

**On any peer**, attach and query. The SQL runs on the gateway, so the SAP round
trip happens there and only results cross the network:

```sql
INSTALL quack; LOAD quack;
ATTACH 'quack:100.x.y.z:9494' AS sapgw (TYPE quack,
    TOKEN 'a-long-shared-token', DISABLE_SSL true);

SELECT CARRID, count(*) FROM sapgw.main.flights GROUP BY 1 ORDER BY 2 DESC;
```


#### Sharing a DuckLake lakehouse the same way

The same shape publishes a [DuckLake](https://ducklake.select) catalog. This is
useful when the lakehouse sits on storage only one machine can reach — an
on-prem object store, a mounted volume, or a private S3 endpoint — and you want
peers to query it without handing out storage credentials.

```sql
INSTALL ducklake; LOAD ducklake;
INSTALL quack;    LOAD quack;

ATTACH 'ducklake:/srv/lake/catalog.ducklake' AS lake (DATA_PATH '/srv/lake/data/');

-- quack serves the DEFAULT catalog, so surface the lake through views in main.
-- A peer cannot reach `lake.*` directly across the connection.
CREATE VIEW measurements AS SELECT * FROM lake.measurements;

CALL quack_serve('quack:127.0.0.1:9494', token => 'a-long-shared-token',
    allow_other_hostname => true);
PRAGMA tunnel_export(secret = 'ts', local_port = 9494);
```

Peers then query the lakehouse as an ordinary table, with filters and aggregates
evaluated on the gateway so only results cross the network:

```sql
ATTACH 'quack:100.x.y.z:9494' AS lakegw (TYPE quack,
    TOKEN 'a-long-shared-token', DISABLE_SSL true);

SELECT id, value FROM lakegw.main.measurements WHERE value > 4 ORDER BY id;
```

`DISABLE_SSL true` is required — the quack client defaults to HTTPS for a
non-local address, and the mesh already encrypts the hop. Works the same on
NetBird; see the [Tailscale](docs/guides/tailscale.md) and
[NetBird](docs/guides/netbird.md) guides for the full walkthrough, including SAP BW.

More detail per backend, plus how to get the keys and run your own control server:
**[docs/guides/](docs/guides/)** — [getting started](docs/guides/getting-started.md) ·
[Tailscale](docs/guides/tailscale.md) · [NetBird](docs/guides/netbird.md) ·
[discovery](docs/guides/discovery.md) · [security](docs/guides/security.md) ·
[troubleshooting](docs/guides/troubleshooting.md).

## 📖 Reference

### SQL surface

| Function | Kind | What it does |
|---|---|---|
| `tunnel_import(secret, remote_host, remote_port, local_port [, timeout, bind_all])` | pragma | Bring a remote service to a local port; returns `(tunnel_id, message)`. Alias: `tunnel_create`. |
| `tunnel_export(secret, local_port [, local_host, remote_port, remote_host, timeout])` | pragma | Publish a local port onto the network; returns `(tunnel_id, remote_port, message)`. |
| `tunnels()` | table | Active tunnels both ways: `tunnel_id, backend, direction, remote_host, remote_port, local_host, local_port, bind_addr, status` (plus `ssh_host/ssh_port/ssh_user` for SSH). |
| `tunnel_close(id)` / `tunnel_close_all` | pragma | Close one / all tunnels (idempotent). |
| `tunnel_peers(secret)` † | table | Mesh peers (peer-local, no API token): `backend, host_name, dns_name, mesh_ip, tags, online`. Tailscale today; NetBird returns an empty set (the embed API exposes no peer list — use `tunnel_self` for your own address, and the dashboard for others). |
| `tunnel_self(secret)` † | table | This node's own mesh identity — including the `mesh_ip` peers should dial. |
| `tunnel_mesh_activate('tailscale'\|'netbird')` † | pragma | *Advanced* — force-load a mesh backend now. Normally automatic on first `tunnel_import`/`tunnel_peers`; use only to surface auth errors early or pin the one mesh for this process. |

† Present only in **mesh-enabled builds** — the published Linux, macOS and Windows
artifacts; not in musl/wasm SSH-only builds. Check what your build has with
`SELECT function_name FROM duckdb_functions() WHERE function_name LIKE 'tunnel_%';`.

`tunnel_import` binds `127.0.0.1` by default; pass `bind_all = true` for all
interfaces. `timeout` (seconds) bounds how long it waits for the listener.

### Direction: import vs export

A tunnel goes one way, and which way is the first thing to decide:

```
tunnel_import(remote_host, remote_port, local_port)     -- consume someone else's service
    binds 127.0.0.1:local_port   →   dials remote_host:remote_port over the backend

tunnel_export(local_port [, remote_port])               -- offer your own service
    accepts on the network   →   dials 127.0.0.1:local_port here
```

**Import** when the data lives elsewhere and you want to query it: `remote_host`/
`remote_port` are the service you want, `local_port` is the local address you get
back for it. This is `ssh -L`, applied uniformly to the mesh backends too.

**Export** when *this* DuckDB is the thing worth reaching — the shape that lets
peers run queries against you:

```sql
INSTALL quack; LOAD quack;
CALL quack_serve('quack:127.0.0.1:9494', allow_other_hostname => true);
PRAGMA tunnel_export(secret = 'ts', local_port = 9494);
```

Any peer on the network can then attach to it:

```sql
ATTACH 'quack:100.x.y.z:9494' AS remote (TYPE quack, TOKEN '…', DISABLE_SSL true);
SELECT * FROM remote.main.my_table;
```

`DISABLE_SSL true` is required: the quack client defaults to HTTPS for a non-local
address, and the mesh already encrypts the link. Find your own address with
`SELECT * FROM tunnel_self(secret = 'ts');`.

On a mesh the service is published on **your node's own mesh address** — there is
no host to name, so `remote_host` is rejected. Over SSH, `tunnel_export` is a real
remote-forward (`ssh -R`): the server binds `remote_port` and `remote_host` is the
bind address there. `remote_port` defaults to `local_port`; pass `remote_port = 0`
to let an SSH server choose one and read it back from the returned row.

`tunnel_create` is a deprecated alias of `tunnel_import`, kept so existing scripts
keep working.

### Secret fields

```sql
-- SSH (TYPE ssh_tunnel or TYPE tunnel, backend 'ssh')
CREATE SECRET s (TYPE ssh_tunnel, host '…', port 22, user '…',
    password '…' /* or */ private_key_path '…', passphrase '…');

-- Tailscale (TYPE tunnel, backend 'tailscale')
-- Nodes always advertise tag:erpl-tunnel; `tags` adds more. Tags are GRANTED from
-- the auth key, so an untagged key yields an untagged node (see the guide).
CREATE SECRET s (TYPE tunnel, backend 'tailscale', auth_key '…',
    hostname '…', tags 'duckdb-export', control_url '' /* empty = Tailscale cloud */,
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
[architecture decisions](docs/design/ADR.md) ·
[mesh on Windows](docs/design/ADR-013-windows-mesh.md) ·
[what is verified where](docs/design/VERIFICATION.md) ·
[footprint](docs/design/FOOTPRINT.md).

## License

BSL 1.1 (→ MPL-2.0 after the change date) — see [LICENSE](LICENSE).
