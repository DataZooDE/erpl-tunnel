# NetBird backend

Reach any peer on your **NetBird** network from DuckDB. The extension runs an
in-process `client/embed` node — userspace WireGuard, no daemon, no root.

## 1. Get a setup key

A **setup key** enrolls this DuckDB node into your NetBird network without an
interactive login. You need a NetBird account; the free plan is enough.

1. Sign in at <https://app.netbird.io> (or your own dashboard if self-hosted).
2. Open **Setup Keys** in the left sidebar, then **Create Setup Key**.
3. Set the options:

   | Option | Set it to | Why |
   |---|---|---|
   | **Name** | e.g. `duckdb erpl_tunnel` | so you can find and revoke it later |
   | **Type / Reusable** | **Reusable** | a one-off key is consumed by the first node, so every later session fails to enroll |
   | **Expires in** | your call | the key stops working after this; the secret then needs a new one |
   | **Usage limit** | `0` (unlimited) or a sensible cap | how many peers may enroll with this key |
   | **Auto-assign groups** | e.g. `duckdb` | this is what your access policy will reference — see step 4 |
   | **Ephemeral peers** | **on** if available | the node is removed after it goes offline instead of accumulating |

4. Click **Create Setup Key** and copy it — it is shown **once**. It is a UUID,
   e.g. `A2C8E62B-38F5-4553-B31E-DD66C696CEBB`.

> **Then allow the traffic — this is the step people miss.** NetBird is
> default-deny between groups: a peer can show **Connected** and still be
> unreachable. Go to **Access Control → Policies** and make sure a policy permits
> the DuckDB node's group to reach the target peer's group. A fresh account has a
> `Default` `All → All` policy which is enough to start; if you assigned a `duckdb`
> group above and locked things down, add a policy for
> `duckdb → <target group>` on the ports you need.

If a peer is *Connected* but nothing flows, it is almost always either this policy
or the kernel-module gotcha in
[The one gotcha you're likely to hit](#the-one-gotcha-youre-likely-to-hit).

## 2. Create the secret

```sql
CREATE SECRET nb (TYPE tunnel, backend 'netbird',
    setup_key      '…',
    hostname       'duckdb-eu-1',   -- this node's device name
    groups         'duckdb',        -- optional
    management_url '',              -- empty = NetBird cloud; a URL = self-hosted
    ephemeral      true,
    state_dir      '');
```

## 3. Discover and connect

```sql
SELECT * FROM tunnel_self(secret = 'nb');   -- this node's identity
PRAGMA tunnel_import(secret = 'nb',
    remote_host = '100.x.y.z', remote_port = 8000, local_port = 9000);
```

(`tunnel_peers()` returns an empty set for NetBird today — the embed API doesn't
expose a stable peer list; use the NetBird dashboard to see peers.)

## Self-hosted NetBird

Point `management_url` at your management server. Setup-key enrollment is a gRPC
path that does **not** need an interactive IdP login, so a self-hosted
management + signal + relay stack (no Zitadel) is enough — see
`test/integration/netbird/` for a working, seeded, no-IdP setup used by the tests.

## The one gotcha you're likely to hit

If a peer shows **Connected** but no traffic flows, it's almost always NetBird's
in-client firewall failing to apply its ACL because the host kernel lacks the
`ip_set_hash_net` module. **Fix:** set `NB_FORCE_USERSPACE_FIREWALL=true` in the
environment before starting DuckDB — NetBird then applies the ACL in userspace, no
kernel module needed. Full detail in [troubleshooting](troubleshooting.md).

## Which way does a tunnel go?

Outbound only: this DuckDB process reaches **out** to a service on a peer.

```
PRAGMA tunnel_import(secret = 'nb',
    remote_host = '<the peer>', remote_port = <its port>,   -- what you want to reach
    local_port  = <a free local port>);                     -- where it shows up for you
```

Read it as: *"NetBird peer `<the peer>` has something on `<its port>`; give me a
`localhost:<local_port>` that points at it."* The mesh carries the bytes, so
`<the peer>` is a peer name or `100.x` NetBird IP, not a routable public address.

### The other direction — publishing a port onto the NetBird network

Your node is a first-class peer with its own `100.x` address, so it can also
*offer* a service. That is `tunnel_export`, and it needs no `remote_host` — the
service is published on your own NetBird address:

```sql
PRAGMA tunnel_export(secret = 'nb', local_port = 9494);
SELECT * FROM tunnel_self(secret = 'nb');   -- the 100.x address peers should use
```

The headline use is letting peers query this DuckDB directly, over quack:

```sql
INSTALL quack; LOAD quack;
CALL quack_serve('quack:127.0.0.1:9494', allow_other_hostname => true);
PRAGMA tunnel_export(secret = 'nb', local_port = 9494);
```

and from any peer on the network:

```sql
ATTACH 'quack:100.x.y.z:9494' AS remote (TYPE quack, TOKEN '<from quack_serve>', DISABLE_SSL true);
SELECT count(*) FROM remote.main.my_table;
```

`DISABLE_SSL true` is not optional — the quack client defaults to HTTPS for any
non-local address, and WireGuard already encrypts the hop. Note also that
`quack_serve` mints a random token unless you pass `token => '…'`; either way the
peer needs it, and it must be at least 4 characters.

Two NetBird specifics: inbound access policies apply to the exported port like any
other service, so the calling peer's group needs to be allowed to reach yours; and
this is the *mesh-local* primitive (`ListenTCP`), not NetBird's `Expose`, which is
a public-internet reverse proxy via `*.netbird.services` — the analogue of
Tailscale Funnel. `tunnel_export` never publishes to the public internet.

### Walkthrough: a SAP BW gateway on your NetBird network

BW is a good fit for this shape. A BICS query is stateful — you begin a session,
configure axes, filter, then fetch — and it is chatty, so you want that
conversation happening next to the BW system, not across a WAN. Export the result
instead.

**Node A — next to SAP BW.** It needs `erpl` (BICS lives in the same suite),
`quack`, and `erpl_tunnel`:

```sql
INSTALL 'erpl' FROM 'http://get.erpl.io'; LOAD 'erpl';
INSTALL quack; LOAD quack;
LOAD erpl_tunnel;

CREATE SECRET sap (TYPE sap_rfc,
    ASHOST 'bw.internal', SYSNR '00', CLIENT '100',
    USER 'BWUSER', PASSWD '…', LANG 'EN');

-- What is even available?
SELECT * FROM sap_bics_show_cubes();
```

Run the stateful BICS workflow here and materialise the answer, so peers get a
plain relation instead of a session they would have to drive remotely:

```sql
SELECT * FROM sap_bics_begin('0D_NW_C01', id => 'sales');
SELECT * FROM sap_bics_rows('sales', '0CALMONTH', '0MATERIAL');
SELECT * FROM sap_bics_columns('sales', '0AMOUNT');
SELECT * FROM sap_bics_filter('sales', '0CALMONTH', '202601', '202602', op => 'SET');

CREATE TABLE sales_by_material AS SELECT * FROM sap_bics_result('sales');
```

For an ERP table alongside it, a view is enough — it stays live, and the read runs
on this node:

```sql
CREATE VIEW materials AS
    SELECT * FROM sap_read_table('MARA',
        COLUMNS=['MATNR','MTART','MATKL'], SECRET='sap');
```

Serve and publish onto the NetBird network:

```sql
CALL quack_serve('quack:127.0.0.1:9494', token => 'a-long-shared-token',
    allow_other_hostname => true);

CREATE SECRET nb (TYPE tunnel, backend 'netbird',
    setup_key 'A2C8E62B-38F5-…', hostname 'duckdb-bw-gw', groups 'duckdb');
PRAGMA tunnel_export(secret = 'nb', local_port = 9494);

SELECT mesh_ip FROM tunnel_self(secret = 'nb');   -- hand this to your peers
```

**Node B — any peer allowed by policy:**

```sql
INSTALL quack; LOAD quack;
ATTACH 'quack:100.x.y.z:9494' AS bw (TYPE quack,
    TOKEN 'a-long-shared-token', DISABLE_SSL true);

SELECT m.MTART, sum(s."0AMOUNT") AS amount
FROM bw.main.sales_by_material s
JOIN bw.main.materials m USING (MATNR)
GROUP BY 1 ORDER BY amount DESC;
```

Both relations live on node A, so the join and the aggregation happen there and
only the summary crosses WireGuard.

**Mind the policy.** NetBird is default-deny in both directions. Exporting a port
does not grant anyone access to it — the calling peer's group still needs a policy
allowing it to reach node A's group (`duckdb` above). A peer that reads
*Connected* and still times out on the `ATTACH` is almost always a policy gap, not
a tunnel fault; see [troubleshooting](troubleshooting.md).

## Notes

- **One mesh per process** (see [one mesh per process](one-mesh-per-process.md)).
- **Platform.** NetBird ships on Linux, macOS and Windows. musl and wasm builds
  are SSH-only.
- **License.** The shim links only NetBird's BSD-3 client code, never the AGPL
  server (see [the audit](../design/NETBIRD_AGPL_AUDIT.md)).
