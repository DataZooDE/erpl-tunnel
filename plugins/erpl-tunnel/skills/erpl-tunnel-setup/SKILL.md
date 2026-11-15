---
name: erpl-tunnel-setup
description: >
  Installing erpl_tunnel and creating its secrets: INSTALL erpl_tunnel FROM https://get.erpl.io (unsigned
  extensions), CREATE SECRET of TYPE tunnel / ssh_tunnel with backend 'ssh' (ssh_host, ssh_port, ssh_user,
  password, private_key_path, passphrase, auth_method), 'tailscale' (auth_key, hostname, tags, control_url for
  Headscale, ephemeral, state_dir) or 'netbird' (setup_key, hostname, groups, management_url), getting auth and
  setup keys, ACL tags (tag:erpl-tunnel), the one-mesh-per-process rule and tunnel_mesh_activate, and which
  functions a build has. Use when someone sets up a tunnel for DuckDB, needs a jump host, Tailscale or NetBird
  to reach SAP or another service, or gets an authentication or "secret not found" error from erpl_tunnel.
user-invocable: false
---

# erpl_tunnel — Setup & Secrets Cheat Sheet

`erpl_tunnel` forwards TCP from DuckDB — SAP RFC, the DuckDB quack protocol, HTTP, Postgres — over SSH or an
in-process Tailscale or NetBird node. No daemon, no root, nothing else to install.

## Critical gotchas

1. **Unsigned extension.** Start DuckDB with `-unsigned` (or `allow_unsigned_extensions` in the client config).
2. **Named parameters use `=`** in the pragmas: `PRAGMA tunnel_import(secret = 's', …)`. `:=` is rejected.
3. **One mesh per process.** Tailscale and NetBird each embed a Go runtime; the first mesh used pins the process.
   Using the other one afterwards is an error — start another process.
4. **Every node advertises `tag:erpl-tunnel`** (plus the secret's `tags`), but tags are *granted* from the
   auth key: to get them, own the tags in the tailnet's `tagOwners` and select them on the key. If enrolment is
   refused or `tunnel_self(...).tags` comes back without them, that is the place to look. NetBird's `groups`
   field cannot change anything — set groups on the setup key.
5. **SSH host keys are not verified.** erpl_tunnel does not check the server's host key, so key authentication
   protects your credential but does not authenticate the bastion. Use SSH tunnels only across networks you
   trust; prefer a mesh backend otherwise.
6. **SSH agent authentication is not supported**: use `auth_method 'key'` with `private_key_path`, or
   `'password'`.
7. **Mesh functions exist only in mesh-enabled builds** (published Linux, macOS, Windows); musl builds are
   SSH-only. Check: `SELECT function_name FROM duckdb_functions() WHERE function_name LIKE 'tunnel_%';`.

## Install

```sql
INSTALL erpl_tunnel FROM 'https://get.erpl.io';
LOAD erpl_tunnel;
```

## Secrets

`TYPE tunnel` and `TYPE ssh_tunnel` are the same secret type; `backend` picks the transport (default `ssh`).

```sql
-- SSH with a key
CREATE SECRET bastion (TYPE tunnel, backend 'ssh',
    ssh_host 'bastion.example.com', ssh_port 22, ssh_user 'jump',
    auth_method 'key', private_key_path '/home/me/.ssh/id_ed25519');

-- SSH with a password (host/user/port are accepted as short aliases)
CREATE SECRET bastion_pw (TYPE ssh_tunnel, host 'bastion.example.com', user 'jump', password 'change-me');

-- Tailscale (or Headscale via control_url)
CREATE SECRET ts (TYPE tunnel, backend 'tailscale',
    auth_key 'tskey-auth-…', hostname 'duckdb-eu-1', ephemeral true);

-- NetBird
CREATE SECRET nb (TYPE tunnel, backend 'netbird',
    setup_key 'A2C8E62B-…', hostname 'duckdb-eu-1');
```

| Backend | Fields |
|---|---|
| `ssh` | `ssh_host`/`host`, `ssh_port`/`port` (22), `ssh_user`/`user`, `password`, `private_key_path`, `passphrase`, `auth_method` (`password`, `key`) |
| `tailscale` | `auth_key`, `hostname`, `tags` (`tag:` prefix added), `control_url` (Headscale), `ephemeral`, `state_dir` (empty = throwaway) |
| `netbird` | `setup_key`, `hostname`, `management_url` (self-hosted), `groups` (no effect — set on the key), `ephemeral`, `state_dir` |

`password`, `passphrase`, `private_key_path`, `auth_key` and `setup_key` are redacted in `duckdb_secrets()`.

### Getting keys

- **Tailscale**: first own `tag:erpl-tunnel` (and any role tag) in the ACL's `tagOwners`; then admin console →
  Settings → Keys → *Generate auth key* — *Ephemeral*, a short expiry, and the tags selected. Tagged nodes do not
  expire on their own, so prefer ephemeral nodes and short-lived keys.
- **NetBird**: dashboard → Setup Keys → *Create* — *Reusable*, optionally an auto-assigned group; then make sure
  an **Access Control policy** lets that group reach the target (NetBird is default-deny).

## Verify

```sql
-- check:skip (needs a mesh account)
SELECT * FROM tunnel_self(secret = 'ts');          -- enrols the node: mesh_ip and the tags actually granted
```

`tunnel_self` is the enrolment check — it uses the credentials. `PRAGMA tunnel_mesh_activate('tailscale')` only
loads the backend and pins the process to it (surfacing load errors early); it does not check credentials.
A non-empty `state_dir` keeps the node identity on disk: mode 0700, not on shared volumes.

See also: `erpl-tunnel-import` (reach a service), `erpl-tunnel-export` (publish a service),
`erpl-tunnel-operate` (status, troubleshooting).
