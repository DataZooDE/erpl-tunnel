# Tailscale backend

Reach any peer on your **tailnet** from DuckDB. The extension runs an in-process
`tsnet` node — no `tailscaled`, no root, no kernel TUN.

## 1. Get an auth key

In the Tailscale admin console → **Settings → Keys**
(<https://login.tailscale.com/admin/settings/keys>), generate an **auth key**.
Recommended: **reusable**, **ephemeral**, and **tagged** (e.g. `tag:duckdb`) so
the node auto-cleans and is easy to find. You'll paste it into the secret.

> **First time? Start untagged.** A tagged auth key only works if that tag has a
> `tagOwners` entry in your tailnet ACL — otherwise enrollment is rejected. For
> your very first run, generate an **untagged** key and omit the `tags` field
> below; add the tag once you've set up `tagOwners` in the ACL editor.

## 2. Create the secret

```sql
CREATE SECRET ts (TYPE tunnel, backend 'tailscale',
    auth_key   'tskey-auth-…',
    hostname   'duckdb-eu-1',      -- how this node appears on the tailnet
    tags       'tag:duckdb',       -- optional; must be permitted by your ACL
    ephemeral  true,               -- auto-removed when offline
    control_url '',                -- empty = Tailscale cloud; a URL = your Headscale
    state_dir  '');                -- empty = a throwaway dir (nothing persists)
```

## 3. Discover and connect

```sql
-- Who is reachable? (peer-local; no API token needed)
SELECT host_name, dns_name, mesh_ip, online FROM tunnel_peers(secret = 'ts');

-- This node's own identity (the name/IP to hand to peers):
SELECT * FROM tunnel_self(secret = 'ts');

-- Tunnel to a peer by its MagicDNS name or 100.x IP:
PRAGMA tunnel_create(secret = 'ts',
    remote_host = 'duckdb-eu-shard3', remote_port = 4213, local_port = 9000);
```

The first mesh call brings the node up (enrolls + connects); it can take a few
seconds. Subsequent calls reuse the same node.

## Self-hosted control (Headscale)

Point `control_url` at your [Headscale](https://headscale.net) server instead of
the Tailscale cloud:

```sql
CREATE SECRET ts (TYPE tunnel, backend 'tailscale',
    auth_key '…', control_url 'https://headscale.example.com', hostname 'duckdb-1');
```

For real cross-NAT connectivity, Headscale needs a reachable **DERP** relay
(front it with TLS); two nodes on the same subnet connect directly without it.

## Notes

- **One mesh per process.** A DuckDB process runs Tailscale *or* NetBird, not both
  — see [one mesh per process](one-mesh-per-process.md).
- **Platform.** Tailscale is available on Linux and macOS builds (not Windows).
- Trouble connecting? See [troubleshooting](troubleshooting.md).
