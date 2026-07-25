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
PRAGMA tunnel_import(secret = 'ts',
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
PRAGMA tunnel_import(secret = 'ts',
    remote_host = '<the peer>', remote_port = <its port>,   -- what you want to reach
    local_port  = <a free local port>);                     -- where it shows up for you
```

Read it as: *"tailnet peer `<the peer>` has something on `<its port>`; give me a
`localhost:<local_port>` that points at it."* The mesh carries the bytes, so
`<the peer>` is a MagicDNS name or `100.x` tailnet IP, not a routable public address.

### The other direction — publishing a port onto the tailnet

Your node is a first-class peer with its own `100.x` address, so it can also
*offer* a service. That is `tunnel_export`, and it needs no `remote_host` — the
service is published on your own tailnet address:

```sql
PRAGMA tunnel_export(secret = 'ts', local_port = 9494);
SELECT * FROM tunnel_self(secret = 'ts');   -- the 100.x address peers should use
```

The headline use is letting peers query this DuckDB directly, over quack:

```sql
INSTALL quack; LOAD quack;
CALL quack_serve('quack:127.0.0.1:9494', allow_other_hostname => true);
PRAGMA tunnel_export(secret = 'ts', local_port = 9494);
```

and from any peer on the tailnet:

```sql
ATTACH 'quack:100.x.y.z:9494' AS remote (TYPE quack, TOKEN '<from quack_serve>', DISABLE_SSL true);
SELECT count(*) FROM remote.main.my_table;
```

`DISABLE_SSL true` is not optional — the quack client defaults to HTTPS for any
non-local address, and WireGuard already encrypts the hop. Note also that
`quack_serve` mints a random token unless you pass `token => '…'`; either way the
peer needs it, and it must be at least 4 characters.

Tailnet ACLs still apply: peers that cannot reach your node cannot reach the
exported port either.

### Walkthrough: a SAP gateway on the tailnet

This is the pattern the export direction exists for. SAP access is usually pinned
to one machine — the one with the NetWeaver RFC SDK installed, the service account,
and a network path to the SAP system. Everyone else ends up asking that machine's
owner for extracts.

Instead, run DuckDB there once and put it on the tailnet.

**Node A — the SAP-adjacent gateway.** It needs `erpl` for SAP, `quack` to speak
the DuckDB protocol, and `erpl_tunnel` to get onto the tailnet:

```sql
INSTALL 'erpl' FROM 'http://get.erpl.io'; LOAD 'erpl';
INSTALL quack; LOAD quack;
LOAD erpl_tunnel;

CREATE SECRET sap (TYPE sap_rfc,
    ASHOST 'sap.internal', SYSNR '00', CLIENT '100',
    USER 'DEVELOPER', PASSWD '…', LANG 'EN');
```

Publish **views**, not raw SAP access. A view is the contract your peers see: it
fixes the table, the columns and the filter, and the SQL still executes here, so
`sap_read_table`'s pushdown keeps working:

```sql
CREATE VIEW flights AS
    SELECT CARRID, CONNID, FLDATE, PRICE, SEATSOCC
    FROM sap_read_table('SFLIGHT', SECRET='sap');

-- Push the expensive predicate into SAP rather than filtering after the fact:
CREATE VIEW lh_flights AS
    SELECT * FROM sap_read_table('SFLIGHT',
        COLUMNS=['CARRID','CONNID','FLDATE','PRICE'],
        FILTER='CARRID = ''LH''',
        THREADS=4, SECRET='sap');
```

Serve and export. `local_port` is the port quack listens on; nothing else changes:

```sql
CALL quack_serve('quack:127.0.0.1:9494', token => 'a-long-shared-token',
    allow_other_hostname => true);

CREATE SECRET ts (TYPE tunnel, backend 'tailscale',
    auth_key 'tskey-auth-…', hostname 'duckdb-sap-gw', ephemeral true);
PRAGMA tunnel_export(secret = 'ts', local_port = 9494);

SELECT mesh_ip FROM tunnel_self(secret = 'ts');   -- hand this to your peers
SELECT direction, remote_host, remote_port, status FROM tunnels();
```

**Node B — any analyst's laptop on the tailnet.** No SAP SDK, no SAP account, no
route to the SAP network:

```sql
INSTALL quack; LOAD quack;
ATTACH 'quack:100.x.y.z:9494' AS sapgw (TYPE quack,
    TOKEN 'a-long-shared-token', DISABLE_SSL true);

SELECT CARRID, count(*) AS legs, avg(PRICE) AS avg_price
FROM sapgw.main.flights
GROUP BY 1 ORDER BY legs DESC;
```

The aggregate runs **on node A**, so the SAP round trip happens next to SAP and
only the grouped result crosses WireGuard. You can also join SAP data against
local data — DuckDB pulls across the link only what it needs:

```sql
SELECT f.CARRID, f.FLDATE, w.temperature
FROM sapgw.main.flights f
JOIN read_csv_auto('~/weather.csv') w USING (FLDATE);
```

What this buys you: SAP credentials never leave node A, the SDK is installed once,
and access is governed by two independent things — the tailnet ACL that decides who
can reach node A, and the views that decide what they can see.

## Notes

- **One mesh per process.** A DuckDB process runs Tailscale *or* NetBird, not both
  — see [one mesh per process](one-mesh-per-process.md).
- **Platform.** Tailscale ships on Linux, macOS and Windows. musl and wasm builds
  are SSH-only.
- Trouble connecting? See [troubleshooting](troubleshooting.md).
