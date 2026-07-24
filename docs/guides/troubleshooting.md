# Troubleshooting `erpl_tunnel`

Real problems you may hit, with the fix. Grouped by symptom.

## SSH

**`Tunnel: secret 'X' not found`** — you passed `secret := 'X'` but never created it,
or created it under a different name. Create it first:
`CREATE SECRET X (TYPE ssh_tunnel, ssh_host '…', ssh_user '…', password '…')`. The
extension never silently falls back to an anonymous localhost node.

**`SSH agent authentication is not supported`** — set `auth_method 'key'` (with
`private_key_path`) or `'password'` on the secret. SSH-agent auth is not implemented.

**`local port N is already in use`** — pick a free `local_port`. The listener binds
`127.0.0.1` by default; pass `bind_all := true` to `tunnel_create` only if you really
need all interfaces.

## Tailscale

**Node never comes up / `tsnet.Up: context deadline exceeded`** — the node can't reach
the control server. Check `control_url` (empty = Tailscale cloud; a URL = your
Headscale). From a container, `127.0.0.1` is the container's own loopback — use a
control URL reachable from where the DuckDB process runs.

**Enrolls but no traffic flows to a peer** — the peer must actually be reachable on the
tailnet. With **Headscale**, its **embedded DERP is plain HTTP**, which the *official*
`tailscale` daemon refuses for relay (`tls: first record does not look like a TLS
handshake`). Two enrolled nodes on the **same subnet** connect **directly** (host ICE
candidates) and don't need DERP — that's how the data-plane test works. For real
cross-NAT use, front Headscale with TLS.

## NetBird

**Peer shows `Connected` / WireGuard handshake succeeds, but no TCP/ICMP flows** — this
is the big one. NetBird's client defaults to the **iptables + ipset** firewall, which
is **default-drop**. If the host kernel lacks the `ip_set_hash_net` module, the ACL
*allow* rule can't be created (`create ipset: invalid type`), **0 rules apply**, and
all overlay data is dropped even though WireGuard connects. **Fix:** set
`NB_FORCE_USERSPACE_FIREWALL=true` in the environment of every NetBird node — it uses
NetBird's Go userspace packet filter instead, applies the ACL, and needs no kernel
module or root. (Alternative, if you control the host: `sudo modprobe ip_set_hash_net
ip_set_hash_ip`.)

**Testing NetBird locally without a cloud account** — you don't need one. Run
management with `IdpManagerConfig: none` against a store seeded from NetBird's own
`store.sql` fixture (see `test/integration/netbird/`) — enrollment via setup key is a
gRPC path that needs no IdP.

**The `netbirdio/netbird` image has no `curl`** — to probe overlay reachability, run a
throwaway client sharing the peer's netns: `docker run --rm --network container:<peer>
curlimages/curl -s http://<overlay-ip>:8000/…`.

## Loading / building

**`did not contain the expected entrypoint function`** — DuckDB derives the entrypoint
from the extension **filename**. The file must be named `erpl_tunnel.duckdb_extension`
(not, say, `ext.duckdb_extension`).

**`Tunnel: … is active in this DuckDB process; … cannot be loaded`** — you tried to use
a second mesh (e.g. NetBird after Tailscale) in the same process. Only **one mesh per
process** (a Go-runtime constraint). Start a new DuckDB session for the other mesh.

**`this build does not include the … backend`** — you're on a build compiled with a
narrower `MESH_BACKEND`. The published artifact is `both`; a local `MESH_BACKEND=ssh`
build has no mesh.

**Extension loads but won't run on an old distro (`GLIBC_2.xx not found`)** — mesh
builds are **glibc-only** and carry the glibc floor of the machine they were built on.
The published artifact is built on the manylinux CI image for a low, portable floor;
a local build on a rolling distro inherits that distro's newer floor. musl is
unsupported.

**Building the mesh shims: `module requires go >= 1.26.4`** — the tailscale shim needs
a new Go. The build **bootstraps** a pinned Go automatically (system Go if new enough,
else a checksum-verified download) — you don't need Go preinstalled unless you want a
mesh build offline.
