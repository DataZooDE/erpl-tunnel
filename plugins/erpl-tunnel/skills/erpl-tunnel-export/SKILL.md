---
name: erpl-tunnel-export
description: >
  Publishing a local service from DuckDB with erpl_tunnel: PRAGMA tunnel_export(secret, local_port [, local_host,
  remote_port, remote_host, timeout]) as a mesh export on the node's own Tailscale/NetBird address or an SSH reverse
  tunnel (ssh -R, GatewayPorts, remote_port = 0), the SAP-gateway pattern (one DuckDB next to SAP with erpl,
  quack_serve with a generated token, a role tag and an ACL, exported onto the tailnet), and the security model an
  export needs (quack has no per-view boundary; the gateway's SAP user is the real limit). Use when other people or
  machines should query a DuckDB, an SAP extract or a local service through a tunnel, or when designing a shared
  SAP data gateway.
user-invocable: false
---

# erpl_tunnel — Publishing a Service Cheat Sheet

`tunnel_export` makes a local port reachable by others — the direction that **grants access**. On a mesh it is
published on this node's mesh IP to peers the control plane admits; over SSH it is a remote forward on the
server.

## Critical gotchas

1. **quack has no view boundary.** A peer holding the token can run *any* SQL in the serving process — read every
   table, call `sap_read_table('<any table>', secret => 'sap')` with the gateway's secret, read local files.
   Views in `main` are the interface you intend, not a security control. The real limits are: a dedicated SAP user
   whose authorizations cover only what peers may see, and a serving process that holds no other secrets and has
   no access to files it should not expose.
2. **Every erpl_tunnel node carries `tag:erpl-tunnel`** — consumers included. Never grant access on that tag.
   Give the gateway its own role tag (`tags 'duckdb-sap-gw'`) and write the ACL for `tag:duckdb-sap-gw:9494` with an
   explicit source group. Check what was granted (`tunnel_self(...).tags`) before exporting.
3. **Start the authenticated service first.** SSH exports probe the local port and fail if nothing listens; a
   **mesh export publishes the port even if nothing listens yet** — whatever binds it later is exposed.
4. **Authenticate the service.** The tunnel proves reachability, not identity. Generate a strong quack token,
   pass it explicitly and hand it out out of band.
5. **`local_host` defaults to 127.0.0.1.** Any other value publishes *that* host (e.g. `local_host 'sap.internal'`
   with port 3300 publishes raw RFC to every peer allowed to reach the export).
6. **On a mesh `remote_host` is rejected**; peers dial `tunnel_self(...).mesh_ip`. SSH exports bind the server's
   loopback unless sshd has `GatewayPorts yes`; `AllowTcpForwarding` must allow remote forwards; `remote_port = 0`
   lets the server choose.

## The SAP gateway pattern

```sql
-- check:skip (needs SAP, a mesh account and the quack extension)
INSTALL erpl FROM 'https://get.erpl.io'; LOAD erpl;
-- A dedicated SAP user that may read only what peers may see (gotcha 1)
CREATE SECRET sap (TYPE sap_rfc, ASHOST 'sap.internal', SYSNR '00', CLIENT '100',
    USER 'ERPL_GATEWAY', PASSWD '…', LANG 'EN');
CREATE VIEW flights AS SELECT * FROM sap_read_table('SFLIGHT', secret => 'sap');   -- the intended interface

INSTALL quack; LOAD quack;
SET VARIABLE quack_token = gen_random_uuid()::VARCHAR || gen_random_uuid()::VARCHAR;
SELECT getvariable('quack_token');                 -- hand this to peers out of band
CALL quack_serve('quack:127.0.0.1:9494', token => getvariable('quack_token'), allow_other_hostname => true);

LOAD erpl_tunnel;
CREATE SECRET ts (TYPE tunnel, backend 'tailscale', auth_key 'tskey-auth-…',
    hostname 'duckdb-sap-gw', tags 'duckdb-sap-gw', ephemeral true);
SELECT tags FROM tunnel_self(secret = 'ts');       -- expect tag:duckdb-sap-gw (and tag:erpl-tunnel)
PRAGMA tunnel_export(secret = 'ts', local_port = 9494);
SELECT mesh_ip FROM tunnel_self(secret = 'ts');    -- the address to share
```

Tailscale ACL for the role tag (adapt group names):

```text
"tagOwners": { "tag:erpl-tunnel": ["group:data-eng"], "tag:duckdb-sap-gw": ["group:data-eng"] },
"acls": [ { "action": "accept", "src": ["group:sap-analysts"], "dst": ["tag:duckdb-sap-gw:9494"] } ]
```

Peers attach as shown in `erpl-tunnel-import` ("A DuckDB published on a tailnet"). They need no SAP account, SDK
or route to SAP — every query runs as `ERPL_GATEWAY`.

## SSH reverse tunnel

```sql
-- check:skip (needs an SSH server)
PRAGMA tunnel_export(secret = 'bastion', local_port = 9494, remote_port = 0);   -- server picks the port
SELECT tunnel_id, direction, local_port, remote_port, status FROM tunnels();
```

## Before exporting — checklist

- What answers behind the port (`local_host`:`local_port`), and is it authenticated with a generated token?
- Which SAP user and authorizations does a peer's query run with? Which other secrets and files can that process
  reach? (gotcha 1)
- Who may reach it: an ACL on the gateway's role tag with an explicit source (mesh); `GatewayPorts` and firewall
  (SSH). Never `tag:erpl-tunnel`.
- Is the SSH server the one you mean? Pin its host key with `host_key_fingerprint` on the secret (see
  `erpl-tunnel-setup`); without it `tunnel_export` warns that the key was not verified.
- How is it stopped: `PRAGMA tunnel_close(<id>);` stops accepting immediately.

See also: `erpl-tunnel-setup`, `erpl-tunnel-import`, `erpl-tunnel-operate`. Serving data as REST or MCP instead of
the DuckDB protocol, with per-endpoint auth: the flapi plugin (`flapi-rest`, `flapi-mcp`, `flapi-auth`).
