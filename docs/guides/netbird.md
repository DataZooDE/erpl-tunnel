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
PRAGMA tunnel_create(secret = 'nb',
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

## Notes

- **One mesh per process** (see [one mesh per process](one-mesh-per-process.md)).
- **Platform.** NetBird is available on Linux and macOS builds (not Windows).
- **License.** The shim links only NetBird's BSD-3 client code, never the AGPL
  server (see [the audit](../design/NETBIRD_AGPL_AUDIT.md)).
