# Tailscale backend

Reach any peer on your **tailnet** from DuckDB. The extension runs an in-process
`tsnet` node — no `tailscaled`, no root, no kernel TUN.

## 1. Get an auth key

An **auth key** lets this DuckDB node join your tailnet without an interactive
login. You need a Tailscale account and a tailnet; the free plan is enough.

1. Sign in at <https://login.tailscale.com>.
2. Go to **Settings → Keys**
   (<https://login.tailscale.com/admin/settings/keys>) — *Settings*, not the
   *Machines* tab.
3. Click **Generate auth key…**.
4. Set the options:

   | Option | Set it to | Why |
   |---|---|---|
   | **Description** | e.g. `duckdb erpl_tunnel` | so you can find and revoke it later |
   | **Reusable** | **on** | otherwise the key is consumed by the first node and every later session fails to enroll |
   | **Ephemeral** | **on** | the node disappears from your machine list shortly after DuckDB exits, instead of piling up dead entries |
   | **Expiration** | your call (max 90 days) | the key stops working after this; the secret then needs a new one |
   | **Tags** | **leave empty for your first run** | see the warning below |

5. Click **Generate key** and copy it — it is shown **once**. It looks like
   `tskey-auth-kXXXXXXCNTRL-XXXXXXXXXXXXXXXXXXXXXX`.

> **Leave tags off until the ACL is ready.** A tagged auth key is rejected unless
> that tag has a `tagOwners` entry in your tailnet ACL, and the failure surfaces
> as a generic enrollment error. Get an untagged key working first, then — if you
> want tags — add to **Access controls**:
>
> ```json
> "tagOwners": { "tag:duckdb": ["autogroup:admin"] }
> ```
>
> and regenerate the key with `tag:duckdb` selected. Tagged nodes also do not
> expire, which is usually what you want for a long-lived service.

**Self-hosting with Headscale?** Generate the key with
`headscale preauthkeys create --user <user> --reusable --expiration 24h` and set
`control_url` on the secret to your Headscale URL — see
[Self-hosted control (Headscale)](#self-hosted-control-headscale) below.

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

## Which way does a tunnel go?

Outbound only: this DuckDB process reaches **out** to a service on a peer.

```
PRAGMA tunnel_create(secret = 'ts',
    remote_host = '<the peer>', remote_port = <its port>,   -- what you want to reach
    local_port  = <a free local port>);                     -- where it shows up for you
```

Read it as: *"tailnet peer `<the peer>` has something on `<its port>`; give me a
`localhost:<local_port>` that points at it."* The mesh carries the bytes, so
`<the peer>` is a MagicDNS name or `100.x` tailnet IP, not a routable public address.

**The other direction is not supported.** Publishing a port from this machine onto
the tailnet network — so peers you do not know upfront can connect *to* DuckDB — would
need a listen primitive that the mesh shim does not currently expose (it has
`dial` only). Your node genuinely is a first-class peer with its own `100.x`
address, so this is a fair thing to expect; it is a gap, not a design stance.
`tsnet.Server.Listen` supports it upstream, so it is about surfacing it rather than feasibility.

## Notes

- **One mesh per process.** A DuckDB process runs Tailscale *or* NetBird, not both
  — see [one mesh per process](one-mesh-per-process.md).
- **Platform.** Tailscale ships on Linux, macOS and Windows. musl and wasm builds
  are SSH-only.
- Trouble connecting? See [troubleshooting](troubleshooting.md).
