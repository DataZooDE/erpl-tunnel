# Peer discovery (mesh backends)

On a mesh, you often don't know a peer's IP up front. `tunnel_peers()` lists what's
reachable — read **peer-locally** from the node's own view, with **no control-plane
API token**.

```sql
SELECT backend, host_name, dns_name, mesh_ip, tags, online
FROM   tunnel_peers(secret = 'ts');
```

| Column | Meaning |
|---|---|
| `backend` | `tailscale` or `netbird` |
| `host_name` | the peer's short name |
| `dns_name` | MagicDNS / DNS name (Tailscale) |
| `mesh_ip` | the `100.x` overlay IP |
| `tags` | the peer's tags (Tailscale) |
| `online` | whether it's currently reachable |

Filter like any table:

```sql
-- all online 'duckdb'-tagged Tailscale peers
SELECT host_name, mesh_ip FROM tunnel_peers(secret = 'ts')
WHERE online AND list_contains(tags, 'tag:duckdb');
```

Then dial one by `dns_name` or `mesh_ip`:

```sql
PRAGMA tunnel_create(secret = 'ts',
    remote_host = 'duckdb-eu-shard3', remote_port = 4213, local_port = 9000);
```

## Announcing this node

A node advertises itself simply by **enrolling with a hostname and tag/group**
(set in the secret). There's no separate "announce" step and no metadata service —
encode role/shard/region in the hostname convention (e.g. `duckdb-eu-shard3`) and
peers find it by name. See this node's own identity with `tunnel_self(secret)`.

> NetBird: `tunnel_peers()` currently returns an empty set (the embed API has no
> stable peer accessor); use the NetBird dashboard. `tunnel_self()` works.
