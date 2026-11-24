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

## SSH host keys — pin them with `host_key_fingerprint`

Authenticating *you* to the server does not authenticate the *server*. Without a
host-key check, anyone on the network path can pose as the bastion: collect a
password, or read and alter the forwarded traffic. Pin the server's key on the
secret:

```sql
CREATE SECRET bastion (TYPE ssh_tunnel, host 'bastion.example.com', user 'jump',
    private_key_path '/home/me/.ssh/id_ed25519',
    host_key_fingerprint 'SHA256:57vu13dj2yi7gd0ERv87El7PwaTsEfigPRyHu4v1mNo');
```

Get the fingerprint out-of-band, from a machine and network you trust (or ask
the server's administrator for `ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub`):

```sh
ssh-keyscan -p 22 bastion.example.com 2>/dev/null | ssh-keygen -lf -
```

The format is exactly what `ssh-keygen -lf` prints: `SHA256:` plus 43 base64
characters (the bare base64 part is accepted too). Anything else — an MD5
fingerprint, a truncated copy — is rejected at `CREATE SECRET`.

| `host_key_fingerprint` | Result |
|---|---|
| set, matches | connects |
| set, does not match | refused right after the handshake, **before authentication** — no password is sent, no key is offered. The error names the expected and the presented fingerprint. |
| not set | connects as before, with a `WARNING: the SSH host key of … was NOT verified …` in the pragma's `message` column and in DuckDB's log (level WARNING; the DuckDB 2.0 CLI shows it) — once per tunnel, naming the presented fingerprint and how to pin it |

The check covers both directions (`tunnel_import` and `tunnel_export`). There is
no trust-on-first-use and nothing is read from or written to `known_hosts`. If the
server has several host keys, `ssh-keyscan` lists them all; pin the one the
warning (or the mismatch error) names, since that is the one negotiated. When a
server's key legitimately changes, confirm the new fingerprint with its
administrator before updating the secret. Prefer key auth over passwords either
way.
