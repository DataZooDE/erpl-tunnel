# Security notes

What `erpl_tunnel` does to keep you safe, and what to be aware of.

## Local listener binds loopback

`tunnel_create` binds `127.0.0.1` by default, so a tunnel is reachable only from
the local machine. Opt into all interfaces explicitly:

```sql
PRAGMA tunnel_create(secret = 's', remote_host = '…', remote_port = 8000,
    local_port = 9000, bind_all = true);   -- 0.0.0.0 — only if you mean it
```

Check what a tunnel is bound to:

```sql
SELECT local_port, bind_addr, status FROM tunnels();
```

## Secrets are redacted; nothing sensitive is logged

`password`, `passphrase`, `private_key_path`, `auth_key`, and `setup_key` are
redacted in `duckdb_secrets()`. No credential, host, or SQL text is ever written to
a log or emitted in telemetry ([TELEMETRY.md](../../TELEMETRY.md)).

## Mesh state files

A mesh node persists WireGuard keys / node identity in `state_dir`. Leave it empty
to use a throwaway directory (ephemeral — nothing persists). If you set one, it's
created with restrictive permissions; keep it private.

## Self-hosted control keeps metadata yours

Pointing `control_url` (Headscale) or `management_url` (self-hosted NetBird) at
infrastructure you run keeps all enrollment and coordination metadata on your side.

## SSH host keys

The SSH backend authenticates *you* to the server; it does not currently pin the
server's host key. Use it to reach hosts you trust (a bastion you operate), and
prefer key auth over passwords.
