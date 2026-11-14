---
name: erpl-tunnel-operate
description: >
  Operating and troubleshooting erpl_tunnel: listing tunnels with tunnels() (direction, backend, bind_addr,
  status), closing them with tunnel_close / tunnel_close_all, and fixing common failures — secret not found, SSH
  agent auth, port in use, Tailscale control-plane timeouts and untagged nodes, Headscale DERP over plain HTTP,
  NetBird default-drop firewall (NB_FORCE_USERSPACE_FIREWALL), export with nothing listening, GatewayPorts, quack
  token and DISABLE_SSL errors, one mesh per process — plus telemetry opt-out. Use when a tunnel does not connect,
  traffic does not flow, an export is unreachable, or tunnels need cleaning up.
user-invocable: false
---

# erpl_tunnel — Operations & Troubleshooting Cheat Sheet

## Status and cleanup

```sql
-- check:offline
LOAD erpl_tunnel;
SELECT tunnel_id, backend, direction, remote_host, remote_port, local_port, bind_addr, status FROM tunnels();
PRAGMA tunnel_close_all;
```

`PRAGMA tunnel_close(<id>)` closes one tunnel; both are idempotent. Tunnels live as long as the DuckDB process.

## Symptoms

| Symptom | Cause | Fix |
|---|---|---|
| `Tunnel: secret 'X' not found` | Secret not created (or other name) | `CREATE SECRET X (TYPE tunnel, …)` first |
| `SSH agent authentication is not supported` | Agent auth | `auth_method 'key'` + `private_key_path`, or `'password'` |
| `local port N is already in use` | Port taken | Another `local_port` |
| `tsnet.Up: context deadline exceeded` | Control server unreachable | Check `control_url`; from a container `127.0.0.1` is the container |
| Tailscale node has no tags | Untagged auth key | Own tags in the ACL, regenerate the key with tags; check `tunnel_self(...).tags` |
| Enrolled but no traffic (Headscale) | Embedded DERP is plain HTTP | Same-subnet direct connections work; front Headscale with TLS |
| NetBird `Connected` but nothing flows | Default-drop iptables firewall without `ip_set_hash_net` | `NB_FORCE_USERSPACE_FIREWALL=true` in the process environment |
| NetBird `groups` has no effect | Groups come from the setup key | Set groups on the key |
| `nothing is listening on 127.0.0.1:N` (export) | Local service not started | Start it, then export |
| `remote_host applies only to the ssh backend` | Mesh export | Drop `remote_host`; peers use `tunnel_self` |
| SSH server refused the remote port | sshd forwarding policy, taken port, port < 1024 | `AllowTcpForwarding yes`, free port > 1024, or `remote_port = 0` |
| Export reachable only on the SSH server | `GatewayPorts no` | `GatewayPorts yes` |
| Peer: TLS/handshake error on quack `ATTACH` | quack defaults to HTTPS remotely | `DISABLE_SSL true` |
| Peer: `schema "lake" does not exist` | quack serves only the default catalog | Views in `main` |
| Error mentioning the other mesh | One mesh per process | Separate processes for Tailscale and NetBird |

## Telemetry

Anonymous usage telemetry (no hosts, credentials or SQL) is on by default; turn it off with
`SET erpl_telemetry_enabled = false;` or `DATAZOO_DISABLE_TELEMETRY=1`.

More: `docs/guides/troubleshooting.md` in the erpl-tunnel repository.

See also: `erpl-tunnel-setup`, `erpl-tunnel-import`, `erpl-tunnel-export`.
