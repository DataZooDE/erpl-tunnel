# Security notes

What `erpl_tunnel` does to keep you safe, and what to be aware of.

## Local listener binds loopback

`tunnel_import` binds `127.0.0.1` by default, so a tunnel is reachable only from
the local machine. Opt into all interfaces explicitly:

```sql
PRAGMA tunnel_import(secret = 's', remote_host = '…', remote_port = 8000,
    local_port = 9000, bind_all = true);   -- 0.0.0.0 — only if you mean it
```

Check what a tunnel is bound to:

```sql
SELECT local_port, bind_addr, status FROM tunnels();
```

## Exporting is the direction that grants access — treat it as such

`tunnel_import` only lets *you* reach out. `tunnel_export` does the opposite: it
makes a local port reachable by others, so the blast radius is whatever that
service allows.

- **Export the narrowest thing that works.** `tunnel_export(local_port = 9494)`
  publishes exactly that port, not the machine. It is not a VPN route into your
  host, and it is never exposed to the public internet — on a mesh it is reachable
  only by peers your control plane admits, and over SSH only per the server's
  `GatewayPorts` policy.
- **Authenticate the service itself.** The tunnel proves *network reachability*,
  not identity. When exporting quack, `quack_serve` mints a token — treat it as a
  credential: pass it out of band, and pass `token => '…'` explicitly rather than
  scraping it if you need a known value.
- **NetBird access policies and Tailscale ACLs still apply** to an exported port.
  They are the right place to say who may reach it.
- **Check what you are offering:**

```sql
SELECT tunnel_id, direction, local_port, remote_port, status FROM tunnels();
PRAGMA tunnel_close(1);   -- stops accepting immediately
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
