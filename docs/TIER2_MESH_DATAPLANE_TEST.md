# Tier 2 mesh data-plane test — concrete plan

> Prove **real traffic flows over the mesh WireGuard data plane** by reaching a
> service on a peer that runs the **official daemon** (kernel TUN, real interface),
> for both Tailscale (Headscale) and NetBird (self-hosted). This closes the gap the
> current tests leave: enrollment/discovery are live, but no bytes have crossed the
> mesh to a real peer.

## What it proves (exit criteria)

For each backend, one green assertion:

- **A→B over the mesh.** Node A = our extension's in-process userspace node. Node B =
  the official daemon in a container with a real mesh interface, co-located with an
  HTTP service. `tunnel_create` from A to `B_mesh_ip:8000`, then a byte-exact payload
  comes back through `localhost:<local_port>` — exactly like the SSH e2e, but the
  transport is WireGuard/DERP instead of `direct-tcpip`.
- **Flagship (stretch): DuckDB data over the mesh.** B serves a real `.parquet`; A
  reads it *through the tunnel* (`read_parquet('http://localhost:<port>/data.parquet')`
  via httpfs) — BRD UC3. Kept optional so httpfs availability never blocks the core
  assertion (curl-through-tunnel is the reliable signal, as in the SSH e2e).

## Key technical decisions (the non-obvious mechanics)

These are the things that make Tier 2 actually work; get them wrong and it hangs.

1. **Keep node A on the host; make the relay reachable from BOTH sides.**
   The control server advertises a DERP/relay URL derived from its `server_url`. That
   URL must resolve for node A (host) *and* node B (container). Using `127.0.0.1:18080`
   breaks B (that's B's own loopback). **Fix:** set `server_url` to the **docker bridge
   gateway IP** (e.g. `172.17.0.1:18080`, computed at runtime). The host reaches the
   mapped port there; containers reach the same gateway IP. So node A stays a plain
   host DuckDB process and the assertion is a host-side `curl` through the tunnel — no
   need to containerize node A.

2. **Peer B needs a real interface.** Run the official daemon container with
   `--cap-add=NET_ADMIN --device=/dev/net/tun`. Kernel TUN gives B a real
   `tailscale0`/`wt0`, so an HTTP server bound to `0.0.0.0:8000` is reachable at B's
   mesh IP. (GitHub Actions ubuntu runners allow `/dev/net/tun` + NET_ADMIN.)

3. **Service co-located via a shared netns sidecar.** The official daemon images are
   minimal. Run the HTTP service as a sidecar with `network_mode: "service:<peer>"`
   so it shares B's network namespace and its bind is reachable at B's mesh IP. Keeps
   the daemon container pure.

4. **Discover B's mesh IP at runtime**, don't hardcode:
   `docker exec ts-peer tailscale ip -4` / `docker exec nb-peer netbird status`
   (or read it from `headscale nodes list` / management). Feed it into `tunnel_create`.

5. **Connectivity is relay-backed.** Two nodes across the docker bridge rarely form a
   *direct* path; the embedded DERP (Tailscale) / the **relay** (NetBird) bridges them.
   The NetBird stack therefore **must include the relay**, not just management+signal.

6. **Bounded-retry assertion.** WireGuard/DERP handshake takes a few seconds after
   enrollment; poll `curl`-through-the-tunnel until it succeeds or a deadline, so the
   test isn't flaky.

## Tailscale Tier 2

Builds on the working `test/integration/headscale/` stack.

**Files**
- `test/integration/headscale/config.yaml` — templatize `server_url` to
  `http://${HS_GATEWAY_IP}:18080` (rendered by the script; embedded DERP already on).
- `test/integration/headscale/docker-compose.yml` — add:
  - `ts-peer`: `tailscale/tailscale:<pinned>`, `cap_add: [NET_ADMIN]`,
    `devices: [/dev/net/tun]`, env `TS_AUTHKEY`, `TS_USERSPACE=false`,
    `TS_HOSTNAME=ts-peer`, `TS_EXTRA_ARGS=--login-server=http://${HS_GATEWAY_IP}:18080`.
  - `ts-peer-svc`: `network_mode: "service:ts-peer"`, serves `python -m http.server 8000`
    over a mounted `./assets/` (a known text file + a small `data.parquet`).
- `test/integration/mesh_dataplane_tailscale.sh`.

**Steps (script)**
1. Compute `HS_GATEWAY_IP` (docker network gateway); render config; `compose up --wait`.
2. `headscale users create`; mint one **reusable, tagged** preauth key.
3. Start `ts-peer` with that key (compose env); wait until
   `docker exec ts-peer tailscale ip -4` returns a `100.64.x` → `B_IP`.
4. Host node A: `CREATE SECRET` (backend tailscale, same key, `control_url` gateway),
   `PRAGMA tunnel_create(secret, remote_host=B_IP, remote_port=8000, local_port=9500)`.
5. Poll `curl -s http://127.0.0.1:9500/hello` until it equals the known payload
   (deadline ~30s). **Core assertion.**
6. Stretch: `SELECT count(*) FROM read_parquet('http://127.0.0.1:9500/data.parquet')`
   equals the known row count (if httpfs is loadable).
7. `compose down -v`.

**Exit:** real HTTP (and optionally parquet) payload retrieved through the Tailscale
tunnel between a userspace node (A) and a kernel-TUN node (B).

## NetBird Tier 2

Same shape; the cost is the control plane.

### Spike outcome (2026-07-23, netbird v0.74.7) — the setup-key problem

Peer enrollment via a setup key is a **gRPC path that does NOT need an IdP** — the
IdP (Zitadel) only gates *user*/dashboard auth. So management + signal + relay can
run without Zitadel *for enrollment*. The blocker is **creating** the setup key,
which normally goes through the IdP-authenticated management API. Findings:

- `management --help` exposes `--single-account-mode-domain` (default on) but no flag
  to mint a setup key headlessly.
- The store (`management/server/store`, FileStore/SQLite) has `SaveAccount` +
  `SaveSetupKey`. So the no-IdP path is: **pre-seed the store** with an account +
  a known setup key, then run management against it and enroll the peer.
- Cost of seeding: a valid `Account` needs a Network (CIDR for peer-IP allocation),
  a default group, settings and a domain; and `SetupKey` stores a **hash** (`Key` /
  `KeySecret`) so the seeder must replicate NetBird's `hashSetupKey`. That's ~100+
  lines of Go against **internal** packages — brittle across version bumps (R3).

**Two viable implementations (pick when doing step 4):**
- **(A) Store-seeder helper** — a small Go program (build-tagged, in `test/`) that
  imports `management/server/store` + `types`, creates an account with a network and
  a fixed setup key, and writes the store; management mounts it. No Zitadel, ~3
  containers, CI-friendly — but couples to NetBird internals (pin + re-verify on bump).
- **(B) Official quickstart** — `getnetbird.sh`-generated compose (management + signal
  + relay + dashboard + **Zitadel**), pinned. Real and sanctioned, but ~6 containers
  and a slow Zitadel init; gate as `workflow_dispatch`/nightly.

**Recommendation:** (A) for CI viability; keep (B) documented as the fallback.

### Status: PASSING (path A). Root cause found & fixed.

`test/integration/mesh_dataplane_netbird.sh` (`make nb_dataplane`) is green: our
extension's `client/embed` node enrolls against the no-IdP self-hosted control
plane, `tunnel_create`s to the official kernel-TUN `netbird` peer, and fetches the
payload **through the tunnel** over direct WireGuard.

**The real root cause** (an earlier note wrongly guessed STUN/relay — see below):
NetBird's client picked the **iptables + ipset** firewall backend, which is
**default-drop**, and the ACL *allow* rule failed to apply because the host kernel
(Arch LTS 6.18) lacks the `ip_set_hash_net` type module — so `hash:net` ipset
creation errored (`create ipset nb0000001: invalid type`), **0 rules applied**, and
every overlay packet was dropped *even though WireGuard connected* (the handshake is
below the firewall). Confirmed via `netbird status -d` (P2P, host/host, handshake
recent, but curl+ping time out) and the client debug log (`ACL rules processed …
total rules count: 0`).

Two earlier red herrings that cost time, now documented so nobody repeats them:
1. The `netbirdio/netbird` image has **no `curl`** — my first "official-to-official
   fails" probe was a broken test (curl-not-found), not a data-path failure. Use a
   throwaway `curlimages/curl` container joined with `--network container:<peer>`.
2. It looked like "relay/STUN infra"; it was the **firewall**. The STUN entries do
   show "Checking…" (placeholder STUN), but host candidates form a **direct** P2P
   path regardless — STUN was never the blocker.

**The fix:** set `NB_FORCE_USERSPACE_FIREWALL=true` on every NetBird node (the
official peers in the compose, and our extension's node A `docker run`). NetBird then
uses its Go **userspace packet filter** (uspfilter) instead of iptables+ipset —
kernel-module-independent, no `sudo`/`modprobe`, CI-friendly. With it, `ACL rules
processed … total rules count: 2` and data flows. Our `client/embed` node honours the
same env because it runs in userspace-bind mode
(`client/firewall/create_linux.go: IsUserspaceBind() && forceUserspaceFirewall()`).
(An alternative for a host/CI that *has* the module: `sudo modprobe ip_set_hash_net
ip_set_hash_ip` — but the userspace firewall is preferred everywhere since it needs
no host privileges. This RCA was cross-checked with OpenAI Codex against the v0.74.7
source.)

### What was built (path A)

Path (A) is implemented and passing:

- `test/integration/netbird/seeder/` — a Go program that runs NetBird's own
  `store.NewTestStoreFromSQL(store.sql)` to produce a management-ready `store.db`
  (account + reusable setup key `A2C8E62B-38F5-4553-B31E-DD66C696CEBB`, a network,
  and a permissive default "All"↔"All" allow-all policy).
- `test/integration/netbird/docker-compose.yml` — management (`IdpManagerConfig:
  none`) against the seeded store, + signal + relay + an official `netbird` daemon
  peer (kernel TUN, real `wt0`) + a shared-netns HTTP service.
- `management.json`, `nodea_entrypoint.sh`, `mesh_dataplane_netbird.sh` mirroring the
  Tailscale harness.

**Proven (real, no mocks):**
- **No-IdP enrollment works.** The official `netbird` daemon enrolls against the
  seeded-store, Zitadel-less management and gets a real overlay IP
  (e.g. `100.64.146.73`), Management/Signal Connected. Node B has a real `wt0`; a
  self-curl to its own overlay IP:8000 returns the payload.
- **Our extension's `client/embed` node enrolls and forms a WireGuard handshake**
  with the official peer — exactly like an official peer does (`configure WireGuard
  endpoint …`, `first wg handshake detected`).

- **Full data-plane pass.** With the userspace-firewall fix (above), node A tunnels
  to node B over direct WireGuard and fetches the payload through the tunnel
  (`NETBIRD-DATAPLANE OK`).

> An earlier version of this section blamed STUN/relay and called the data plane a
> "remaining gap." That was wrong — see **"Root cause found & fixed"** above. The
> real cause was the iptables/ipset firewall default-dropping data; the fix is
> `NB_FORCE_USERSPACE_FIREWALL=true`.

**Files**
- `test/integration/netbird/docker-compose.yml` — management + signal + relay (+ IdP
  per the spike), plus:
  - `nb-peer`: `netbirdio/netbird:<pinned>`, `cap_add:[NET_ADMIN]`,
    `devices:[/dev/net/tun]`, env `NB_SETUP_KEY`, `NB_MANAGEMENT_URL=http://${GW}:33073`.
  - `nb-peer-svc`: `network_mode: "service:nb-peer"`, HTTP server on 8000.
- `test/integration/netbird/management.json` — static config; **relay URL set to the
  gateway IP** so it's reachable from host node A and container B.
- `test/integration/mesh_dataplane_netbird.sh`.

**Steps** mirror Tailscale: bring up control plane → create a setup key
(`netbird`/management API or CLI) → start `nb-peer`, wait for
`docker exec nb-peer netbird status` to show a NetBird IP → host node A
`CREATE SECRET` (backend netbird, setup_key, management_url gateway) →
`tunnel_create` to `B_IP:8000` → poll curl-through-tunnel → assert.

**R4 note:** this pulls the AGPL management/signal/relay **as separate containers**
(their own processes) — that does NOT link them into our extension, so it doesn't
change the audit (`docs/NETBIRD_AGPL_AUDIT.md`); our shim still links only client code.

## Shared harness

The body is backend-agnostic: *enroll B (daemon+service) → enroll A (extension) →
A tunnels to B:8000 → poll curl → assert*. Factor into
`test/integration/lib/mesh_dataplane.sh` with a per-backend adapter supplying:
`bring_up`, `mint_key`, `start_peer`, `peer_ip`, `secret_sql`, `tear_down`. Two thin
wrappers call it. The assertion reuses the SSH e2e's FIFO-keepalive + `curl` pattern.

## CI

- New jobs in `.github/workflows/mesh-tests.yml` (or a separate `mesh-dataplane.yml`):
  `tailscale-dataplane` (every push) and `netbird-dataplane`
  (`workflow_dispatch` + nightly if the stack is heavy).
- Requirements: `setup-go`, docker with `/dev/net/tun` + `--privileged`/NET_ADMIN,
  a duckdb CLI, and the `MESH_BACKEND=both` (or per-mesh) build.
- Pin every image (`tailscale`, `headscale`, `netbird`, management/signal/relay).

## Risks & mitigations

- **DERP/relay reachability (the #1 gotcha).** Mitigated by the gateway-IP `server_url`
  decision above; validate with a first "does A see B online / can A dial B at all"
  smoke before asserting payloads.
- **NetBird control-plane weight/flakiness.** Mitigated by the IdP spike (shrink to
  management+signal+relay if possible) and by gating the job.
- **Kernel TUN in CI.** Low risk on GitHub-hosted ubuntu; documented as a requirement.
- **Handshake timing.** Bounded-retry polling, generous deadlines.
- **quack protocol server.** Not a standardized DuckDB server in 1.5.4 → use
  httpfs+parquet as the "DuckDB data over the mesh" flagship instead; treat true quack
  as a later item behind whatever server (`httpserver` ext / ducklake) we standardize on.

## Sequencing

1. **Tailscale gateway-IP smoke** — reconfigure Headscale `server_url` to the gateway
   IP, bring up `ts-peer` (kernel TUN), and confirm A can *dial* B at all
   (`mesh_dial` succeeds). This validates the single riskiest assumption (relay
   reachability across host↔container) before building assertions.
2. **Tailscale Tier 2 full** — service sidecar + curl-through-tunnel assertion + CI job.
3. **NetBird control-plane spike** — setup-key enrollment without full IdP?
4. **NetBird Tier 2 full** — mirror Tailscale with the chosen control plane + CI job.
5. **Flagship** — swap/extend the assertion to DuckDB-data-over-mesh (parquet via
   httpfs), and revisit true quack once a DuckDB network server is chosen.
