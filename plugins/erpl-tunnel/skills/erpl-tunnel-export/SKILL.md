---
name: erpl-tunnel-export
description: >
  Publishing a local service from DuckDB with erpl_tunnel: PRAGMA tunnel_export(secret, local_port [, local_host,
  remote_port, remote_host, timeout]) as a mesh export on the node's own Tailscale/NetBird address or an SSH reverse
  tunnel (ssh -R, GatewayPorts, remote_port = 0), the SAP-gateway pattern (one DuckDB next to SAP with erpl, views
  in main, quack_serve with a token, exported onto the tailnet), sharing a DuckLake through views, and the security
  review an export needs. Use when other people or machines should query a DuckDB, an SAP extract or a local
  service through a tunnel, or when designing a shared SAP data gateway.
user-invocable: false
---

# erpl_tunnel — Publishing a Service Cheat Sheet

`tunnel_export` makes a local port reachable by others — the direction that **grants access**. On a mesh it is
published on this node's mesh IP to peers the control plane admits; over SSH it is a remote forward on the
server.

## Critical gotchas

1. **The local service must already listen.** Export probes it first and fails if nothing is behind the port.
2. **Authenticate the service itself.** The tunnel proves reachability, not identity: give `quack_serve` an
   explicit `token => '…'` (at least 4 characters) and pass it out of band.
3. **Share views, not raw access.** quack serves only the default catalog — expose exactly what peers may read
   as views in `main` (also how to share an attached DuckLake).
4. **On a mesh `remote_host` is rejected**; peers dial `tunnel_self(...).mesh_ip`.
5. **SSH exports bind the server's loopback** unless sshd has `GatewayPorts yes`; `AllowTcpForwarding` must allow
   remote forwarding; ports below 1024 need root there. `remote_port = 0` lets the server choose.

## The SAP gateway pattern

```sql
-- check:skip-live (needs SAP, a mesh account and the quack extension)
INSTALL erpl FROM 'https://get.erpl.io'; LOAD erpl;
CREATE SECRET sap (TYPE sap_rfc, ASHOST 'sap.internal', SYSNR '00', CLIENT '100',
    USER 'ERPL_USER', PASSWD 'change-me', LANG 'EN');
CREATE VIEW flights AS SELECT * FROM sap_read_table('SFLIGHT', secret => 'sap');   -- the contract peers see

INSTALL quack; LOAD quack;
CALL quack_serve('quack:127.0.0.1:9494', token => 'a-long-shared-token', allow_other_hostname => true);

LOAD erpl_tunnel;
CREATE SECRET ts (TYPE tunnel, backend 'tailscale', auth_key 'tskey-auth-…',
    hostname 'duckdb-sap-gw', ephemeral true);
PRAGMA tunnel_export(secret = 'ts', local_port = 9494);
SELECT mesh_ip FROM tunnel_self(secret = 'ts');   -- the address to share
```

Peers need no SAP account, SDK or route to SAP — they attach with `erpl-tunnel-import`'s quack recipe. Every
peer query runs as the gateway's SAP user: scope that user's authorizations accordingly.

## SSH reverse tunnel

```sql
-- check:skip-live (needs an SSH server)
PRAGMA tunnel_export(secret = 'bastion', local_port = 9494, remote_port = 0);   -- server picks the port
SELECT tunnel_id, direction, local_port, remote_port, status FROM tunnels();
```

## Before exporting — checklist

- What exactly is reachable behind the port, and is it authenticated?
- Who may reach it: Tailscale ACLs / NetBird policies (mesh), `GatewayPorts` and firewall (SSH).
- Which SAP user and authorizations does a peer's query run with?
- How is it stopped: `PRAGMA tunnel_close(<id>);` stops accepting immediately.

See also: `erpl-tunnel-setup`, `erpl-tunnel-import`, `erpl-tunnel-operate`. Serving data as REST or MCP instead
of the DuckDB protocol: the flapi plugin (`flapi-rest`, `flapi-mcp`).
