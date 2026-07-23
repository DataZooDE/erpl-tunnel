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

**Spike first (do before the compose): can NetBird enroll with a setup key WITHOUT a
full IdP?** Setup keys are non-interactive, so management + signal + relay *may* run
with a static config and no Zitadel. Verify by standing up management/signal/relay
from a pinned config and enrolling one `netbird` container via `NB_SETUP_KEY`.
- If **yes** → a 3-container control plane (management+signal+relay), CI-viable.
- If **no** → use NetBird's official quickstart compose (adds dashboard + Zitadel),
  pinned to a version; heavier and slower but real. Gate this job so it can be
  `workflow_dispatch`-only if it's too heavy for every push.

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
