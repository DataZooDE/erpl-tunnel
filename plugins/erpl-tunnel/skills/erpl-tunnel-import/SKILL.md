---
name: erpl-tunnel-import
description: >
  Reaching a remote TCP service from DuckDB through erpl_tunnel: PRAGMA tunnel_import(secret, remote_host,
  remote_port, local_port [, timeout, bind_all]) as ssh -L for SSH, Tailscale and NetBird, combining it with an
  erpl sap_rfc secret to reach an SAP system behind a bastion or on a mesh (gateway port 33<sysnr>, ASHOST
  127.0.0.1, message server 36<sysnr>, or GWSERV), reaching Postgres, HTTP or a remote DuckDB via quack ATTACH
  (DISABLE_SSL true), and finding peers with tunnel_peers and tunnel_self. Use when SAP or another service is
  not directly reachable from where DuckDB runs, or when connecting to a DuckDB published on a tailnet.
user-invocable: false
---

# erpl_tunnel — Reaching a Remote Service Cheat Sheet

`tunnel_import` binds a local port (127.0.0.1 by default) and forwards it to `remote_host:remote_port` over the
secret's transport. Anything that can talk to `localhost:<local_port>` then reaches the remote service.

## Critical gotchas

1. **SAP RFC derives the port from the system number.** `SYSNR '00'` means gateway port `3300`; the local end of
   the tunnel must be that port, or set `GWSERV` in the `sap_rfc` secret to the local port.
2. **Local means 127.0.0.1.** Point clients at `127.0.0.1`/`localhost`; `bind_all = true` opens the port on all
   interfaces — only when other machines must use it.
3. **A port in use fails** (`local port N is already in use`): pick another `local_port`.
4. **On a mesh, `remote_host` is a peer name or mesh IP**; over SSH it is resolved *on the bastion*, so internal
   names work there.

## `PRAGMA tunnel_import(secret = …, remote_host = …, remote_port = …, local_port = … [, timeout, bind_all])`

Returns `(tunnel_id, message)`. `timeout` (seconds) bounds the wait for the listener. `tunnel_create` is a
deprecated alias.

## SAP through a bastion

```sql
-- check:skip-live (needs a bastion and an SAP system)
LOAD erpl_tunnel;
LOAD erpl;
CREATE SECRET bastion (TYPE tunnel, ssh_host 'bastion.example.com', ssh_user 'jump',
    auth_method 'key', private_key_path '/home/me/.ssh/id_ed25519');
PRAGMA tunnel_import(secret = 'bastion', remote_host = 'sap.internal', remote_port = 3300, local_port = 3300);
CREATE SECRET my_sap (TYPE sap_rfc, ASHOST '127.0.0.1', SYSNR '00', CLIENT '100',
    USER 'ERPL_USER', PASSWD 'change-me');
PRAGMA sap_rfc_ping(secret = 'my_sap');
```

| SAP endpoint | Remote port | Secret |
|---|---|---|
| Application server gateway | `33<sysnr>` | `ASHOST '127.0.0.1'`, `SYSNR '<nn>'` (local port = 33nn) or `GWSERV '<local port>'` |
| Message server (load balancing) | `36<sysnr>` | Load balancing hands out application-server hosts the tunnel does not cover — prefer a direct `ASHOST` through the tunnel |
| WebSocket RFC (HTTPS) | ICM HTTPS port, e.g. `443` / `44300` | `WSHOST '127.0.0.1'`, `WSPORT '<local port>'` — the server certificate is issued for the real host name |

The erpl skills (`erpl-setup` in the erpl plugin) cover the SAP side.

## A DuckDB published on a tailnet (quack)

```sql
-- check:skip-live (needs a mesh peer)
INSTALL quack; LOAD quack;
ATTACH 'quack:100.x.y.z:9494' AS remote (TYPE quack, TOKEN '…', DISABLE_SSL true);
SELECT * FROM remote.main.flights;
```

`DISABLE_SSL true` is required for non-local addresses (the mesh encrypts the hop). Only the publisher's default
catalog is served — it shares views in `main`.

## Finding peers

```sql
-- check:skip-live (needs a mesh account)
SELECT host_name, dns_name, mesh_ip, tags, online FROM tunnel_peers(secret = 'ts');   -- Tailscale; NetBird returns none
SELECT * FROM tunnel_self(secret = 'ts');                                              -- your own address
```

Filter peers by tag or a naming convention (`hostname` per role) and import from the chosen `mesh_ip`.

See also: `erpl-tunnel-setup`, `erpl-tunnel-export`, `erpl-tunnel-operate`.
