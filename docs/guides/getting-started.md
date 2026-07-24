# Getting started

Reach a service that isn't directly reachable from your DuckDB session — in three
statements.

## Install

```sql
INSTALL erpl_tunnel FROM community;
LOAD erpl_tunnel;
```

## The idea

`erpl_tunnel` opens a **local listener** on `127.0.0.1:<local_port>` and forwards
everything that connects to it to a **remote `host:port`**, over one of three
transports (SSH, Tailscale, NetBird). You then point any client — `httpfs`,
`erpl_rfc`, another DuckDB — at `localhost:<local_port>` and it just works.

Three steps, always the same shape:

1. **`CREATE SECRET`** — where to connect and how to authenticate.
2. **`PRAGMA tunnel_create(...)`** — open the tunnel.
3. **Use `localhost:<local_port>`** — query as if the service were local.

## Your first tunnel (SSH)

Say there's an HTTP service you can only reach *through* an SSH bastion:

```sql
-- 1. the credential
CREATE SECRET bastion (TYPE ssh_tunnel,
    host 'bastion.example.com', user 'jump', password 'secret');
    -- or: private_key_path '/home/me/.ssh/id_ed25519', passphrase '…'

-- 2. open the tunnel: localhost:9000  ->  bastion  ->  internal-http:8000
PRAGMA tunnel_create(secret = 'bastion',
    remote_host = 'internal-http', remote_port = 8000, local_port = 9000);

-- 3. use it
SELECT * FROM read_csv_auto('http://localhost:9000/data.csv');
```

Manage tunnels:

```sql
SELECT * FROM tunnels();     -- tunnel_id, backend, remote_host, local_port, bind_addr, status
PRAGMA tunnel_close(1);      -- close one
PRAGMA tunnel_close_all;     -- close all (idempotent — safe if none are open)
```

## Going through a mesh instead of a bastion

If the service is on a **Tailscale** or **NetBird** network, swap the secret — the
`tunnel_create` call is identical. See [Tailscale](tailscale.md) and
[NetBird](netbird.md). Before dialing, you can discover what's reachable:

```sql
SELECT host_name, mesh_ip, online FROM tunnel_peers(secret = 'ts');
```

## Good defaults, and how to change them

- The listener binds **loopback** (`127.0.0.1`). Pass `bind_all = true` to
  `tunnel_create` to listen on all interfaces.
- `tunnel_create` waits up to `timeout` seconds (default 60) for the listener to
  be ready, then returns an actionable error.
- Secrets **redact** every sensitive field in `duckdb_secrets()`.

## When something goes wrong

Errors are written to be actionable — they name the likely cause and the fix. If
you're stuck, the [troubleshooting guide](troubleshooting.md) lists the real ones
(and the mesh-specific gotchas).

Next: [Tailscale](tailscale.md) · [NetBird](netbird.md) · [discovery](discovery.md)
· [security](security.md).
