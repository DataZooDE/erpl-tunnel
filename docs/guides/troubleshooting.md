# Troubleshooting `erpl_tunnel`

Real problems you may hit, with the fix. Grouped by symptom.

## SSH

**`Tunnel: secret 'X' not found`** — you passed `secret = 'X'` but never created it,
or created it under a different name. Create it first:
`CREATE SECRET X (TYPE ssh_tunnel, ssh_host '…', ssh_user '…', password '…')`. The
extension never silently falls back to an anonymous localhost node.

**`SSH agent authentication is not supported`** — set `auth_method 'key'` (with
`private_key_path`) or `'password'` on the secret. SSH-agent auth is not implemented.

**`local port N is already in use`** — pick a free `local_port`. The listener binds
`127.0.0.1` by default; pass `bind_all = true` to `tunnel_import` only if you really
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

**Node shows no tags in the admin console** — tags are granted by the control
plane from the **auth key**, not by the client. `erpl_tunnel` advertises
`tag:erpl-tunnel` plus the secret's `tags`, but an untagged key produces an
untagged node and no error. Add `tagOwners` entries for the tags, regenerate the
auth key **with those tags selected**, and re-enrol. Check what was actually
granted with `SELECT tags FROM tunnel_self(secret = 'ts');` — that is the control
plane's answer, not your request.

**A tag cannot be added per exported port** — tags are fixed when the node
registers and `tunnel_export` happens later, so nothing can add one at export
time. Name the node (`hostname`) for its role and read ports from `tunnels()`; if
you need per-port tags, list them in the secret before the node starts.

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

**`groups` on a NetBird secret has no effect** — NetBird assigns groups from the
**setup key** (configured where you create the key), and its embed API exposes no
client-side group option, so the field is accepted for symmetry with Tailscale's
`tags` but cannot change anything. Set the groups on the setup key instead.

**Testing NetBird locally without a cloud account** — you don't need one. Run
management with `IdpManagerConfig: none` against a store seeded from NetBird's own
`store.sql` fixture (see `test/integration/netbird/`) — enrollment via setup key is a
gRPC path that needs no IdP.

**The `netbirdio/netbird` image has no `curl`** — to probe overlay reachability, run a
throwaway client sharing the peer's netns: `docker run --rm --network container:<peer>
curlimages/curl -s http://<overlay-ip>:8000/…`.

## Exporting a port (`tunnel_export`)

**`nothing is listening on 127.0.0.1:N, so there is nothing to export`** — the local
service must be running *before* you export it. `tunnel_export` probes it first on
purpose: a listener published onto the network with nothing behind it fails later,
from a peer, with a much worse error. Start the service, or fix `local_host`/
`local_port`.

**`remote_host applies only to the ssh backend`** — on a mesh there is no host to
name; the service is published on your own node's address. Drop `remote_host` and
get the address peers should use from
`SELECT * FROM tunnel_self(secret = '…');`.

**`the SSH server refused to bind remote port N`** — usually one of three things:
sshd has `AllowTcpForwarding no` (or `local`) and needs `yes`/`remote`; the port is
already taken on the server; or it is below 1024, which needs root there. Pick a
free port above 1024, or pass `remote_port = 0` and read the port the server picked
out of the returned row.

**The export works but only from the SSH server itself** — that is `GatewayPorts no`,
sshd's default, which binds the forward to the server's loopback. Set
`GatewayPorts yes` (or `clientspecified` plus `remote_host = '0.0.0.0'`) to let other
machines reach it. This applies to SSH only; a mesh export is reachable from every
peer that policy allows.

**`an inbound connection arrived but 127.0.0.1:N refused it`** — the export is fine
and still listening; the *local* service went away. Restart it; connections will be
served again without re-running `tunnel_export`.

**`Quack server token must be at least 4 characters long`** — `quack_serve` rejects
a short `token =>`. It fails the `CALL` but leaves the rest of your script running,
so the first visible symptom is usually the peer's "Could not connect to server":
nothing ever started listening. Check the server's own output before blaming the
tunnel.

**Peer says `schema "lake" does not exist` for an ATTACHed catalog** — quack serves
the **default** catalog only. A database you `ATTACH`ed on the gateway (a DuckLake,
a second .db, a Postgres) is a separate catalog and is not reachable as
`remote.<that_catalog>.*`. Surface what you want to share as views in `main`:
`CREATE VIEW measurements AS SELECT * FROM lake.measurements;` — peers then read
`remote.main.measurements`. This is also the right seam for deciding what to expose.

**Peer gets a TLS or handshake error attaching over quack** — pass
`DISABLE_SSL true` in the `ATTACH`. The quack client defaults to HTTPS for any
non-local address, and the mesh already encrypts the hop. Remember also that
`quack_serve` mints a random token unless you pass `token => '…'` — the peer needs
whichever one is in force.

## Windows

**`failed to load the Tailscale shim … The specified module could not be found`**
— `ERROR_MOD_NOT_FOUND`. The shim is extracted on its own into a cache directory,
so it must not depend on anything but system DLLs. A shim built without
`-static-libgcc` pulls in `libgcc_s_seh-1.dll` / `libwinpthread-1.dll`, which are
not there. Rebuild with the bundled CMake logic (or check with
`dumpbin /dependents` — you should see only `KERNEL32` and `api-ms-win-crt-*`).

**Antivirus quarantines the shim, or first activation is slow** — the mesh shim is
written to
`%LOCALAPPDATA%\DataZoo\erpl-tunnel\shims\<hash>\{ts,nb}_shim.dll` and loaded
from there. It is a large unsigned DLL, so a first-load scan can take a few
seconds, and aggressive enterprise policies may quarantine it. Allowlist that
path. Deleting the directory is safe when DuckDB is not running — it is recreated
on the next activation.

**`tunnel_peers` / `tunnel_self` / `tunnel_mesh_activate` do not exist** — you are
on an SSH-only build. On Windows the mesh backends need a mingw-w64 gcc for cgo at
*build* time; without one the build degrades to SSH-only with a warning. Check
with `SELECT function_name FROM duckdb_functions() WHERE function_name LIKE
'tunnel_%';` and see [building](building.md).

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
