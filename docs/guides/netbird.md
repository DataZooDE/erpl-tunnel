# NetBird backend

Reach any peer on your **NetBird** network from DuckDB. The extension runs an
in-process `client/embed` node — userspace WireGuard, no daemon, no root.

## 1. Get a setup key

In the NetBird dashboard (<https://app.netbird.io> for the cloud) → **Setup Keys**,
create one (reusable + a group like `duckdb` is handy). Self-hosted NetBird works
too — see below.

> **Traffic needs a policy.** NetBird is default-deny between groups. For DuckDB to
> actually reach a peer, an **Access Control policy** must allow this node's group →
> the target peer's group (the default `All → All` policy is enough to start). A
> peer can be *Connected* yet unreachable if no policy permits the flow — see the
> [firewall gotcha](#the-one-gotcha-youre-likely-to-hit) below.

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
