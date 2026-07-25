# Business Requirements Document — `erpl-tunnel`

> A zero-dependency DuckDB extension that tunnels arbitrary TCP traffic
> (DuckDB *quack* remote protocol, SAP RFC, HTTP, …) over **SSH**,
> **Tailscale**, or **NetBird**, with dead-simple auth and peer discovery.

| | |
|---|---|
| **Document** | Business Requirements Document (BRD) |
| **Status** | Draft for review |
| **Date** | 2026-07-23 |
| **Author** | Research (DataZoo) |
| **Companion** | [`HLD.md`](HLD.md) — High-Level Design |
| **Source material** | `github.com/DataZooDE/erpl` `tunnel/` module (SSH tunnel to be extracted) |

---

## 1. Executive summary

`erpl` already contains a self-contained SSH-tunnel sub-extension
(`tunnel/`, DuckDB extension name `erpl_tunnel`). This project **extracts
that module into a standalone extension** and **extends it with two mesh-VPN
backends — Tailscale and NetBird** — so that a DuckDB instance can reach a
remote service (or another DuckDB instance) across NAT/firewalls with a
single pasted token and no external daemon to install.

The north-star qualities are, in priority order:

1. **Zero external dependency** — everything the extension needs is
   statically linked or packaged *inside* the loadable extension. No
   `tailscaled`, no `netbird` daemon, no system SSH client, no kernel TUN,
   no root.
2. **Trivially easy setup** — "paste one auth key, get a tunnel." Great
   error messages. Robust, idempotent lifecycle.
3. **Protocol-agnostic transport** — the same `tunnel_create` works for the
   quack remote protocol, SAP RFC, and generic HTTP because it forwards raw
   TCP.
4. **Discovery** — DuckDB instances announce themselves on the mesh and can
   be enumerated by peers.

## 2. Background & context

### 2.1 What exists today (erpl `tunnel/`)

- **Transport:** classic SSH **local port forwarding** via **libssh2**
  (vcpkg, statically linkable). Opens a local listening socket and forwards
  each accepted connection through an SSH `direct-tcpip` channel to
  `remote_host:remote_port`. Explicit — the user points the downstream
  client at `localhost:<local_port>`. No SOCKS, no interception.
- **Surface:** a DuckDB `SECRET` of `TYPE ssh_tunnel` (host/user/password or
  key), plus `PRAGMA tunnel_create(...)`, `PRAGMA tunnel_close(id)`,
  `PRAGMA tunnel_close_all`, and a `tunnels()` table function.
- **Auth:** password, private-key-from-memory; SSH-agent is stubbed
  (declared, not implemented).
- **Coupling to remove:** links `erpl_rfc_extension` and includes
  `../rfc/src/include` solely for PostHog telemetry. Extraction = sever
  those three couplings and add the module's own `vcpkg.json`.
- **License:** BSL 1.1 (→ MPL 2.0 after change date).

### 2.2 Why extend it

Users need to reach data endpoints that sit behind NAT or a corporate
firewall — a colleague's laptop DuckDB, an on-prem SAP gateway, a private
HTTP data service — without standing up bastions or opening ports. Mesh VPNs
(Tailscale, NetBird) solve NAT traversal with WireGuard and make every node
individually addressable. Embedding them **in-process** keeps the promise of
"install a DuckDB extension, nothing else."

### 2.3 Feasibility findings that shape scope (from research)

- **Tailscale** ships `tsnet` (in-process userspace node, no daemon/TUN/root)
  and official C bindings `libtailscale` (BSD-3). Auth = one auth key.
  Discovery = ACL **tags** + peer-local `Status()`. Raw TCP `Listen`/`Dial`
  works peer-to-peer.
- **NetBird** ships `client/embed` (in-process, userspace WireGuard +
  netstack, no daemon/root; BSD-3 client). Auth = **setup key**. **No
  official C library** — a cgo shim over `client/embed` is required
  (community prior art exists). Discovery = DNS labels + groups +
  peer-local sync/status. A management-server AGPL license point must be
  cleared before shipping (see §11).
- **Hard constraint & its resolution:** both meshes are Go, and **two
  independent Go runtimes cannot coexist in one process**. Since a DuckDB
  process **never needs Tailscale and NetBird active at the same time**, each
  mesh is built as its **own Go `c-shared` shim** (vendored `tsnet`;
  `netbird/client/embed`), embedded in the extension and **lazily `dlopen`'d
  only when that backend is first activated**, guarded by a **single-mesh
  latch** (first backend activated wins; the other then errors). Only one Go
  runtime is ever loaded → the clash is impossible by construction. SSH stays
  in C++ (libssh2) and needs no Go at all. *(Decisions D1/D1a, §10.)*

## 3. Goals & non-goals

### 3.1 Goals

- **G1** Extract `erpl_tunnel` into a standalone, independently buildable and
  releasable DuckDB extension with SSH feature parity.
- **G2** Add a **Tailscale** transport backend (in-process).
- **G3** Add a **NetBird** transport backend (in-process).
- **G4** One coherent, minimal SQL surface across all three backends.
- **G5** Peer **discovery** for mesh backends (tags/naming based).
- **G6** Zero external runtime dependency; single loadable artifact.
- **G7** Best-in-class **ease of use** and **error messages**; robust,
  idempotent setup.

### 3.2 Non-goals (explicitly out of scope for v1)

- **NG1** No transparent/SOCKS interception. Only **explicit local
  port-forward**. *(Decision D2.)*
- **NG2** No app-level "announce service" carrying rich JSON metadata; no
  per-node capability/schema/load advertisement. Discovery metadata is
  limited to what tags/hostname/DNS-label encode. *(Decision D3.)*
- **NG3** No management-plane administration (creating ACLs, minting keys,
  managing users) from inside the extension. The extension is a *node*, not
  a control panel.
- **NG4** Not a general VPN for the host OS. The mesh node is userspace and
  in-process; host-level tools do not see the tunnel.
- **NG5** No Windows first-class support in v1 if it materially delays
  delivery (Go `c-shared` on Windows is the weakest target); Linux + macOS
  are the committed platforms. *(Confirm in planning.)*
  > **Outcome (2026-07-25): superseded — Windows ships the mesh backends.** It did
  > not materially delay delivery: SSH on Windows shipped first, and the mesh port
  > followed once the feasibility spikes passed. See
  > [ADR-013](ADR-013-windows-mesh.md).
- **NG6** No self-service relicensing of erpl SSH code beyond the chosen
  license (§11).

## 4. Stakeholders & personas

| Persona | Need |
|---|---|
| **Data engineer (Ada)** | Query a private/remote DuckDB or CSV-over-HTTP without VPN setup; wants `LOAD`, paste a key, `tunnel_create`, run SQL. |
| **SAP integrator (Sven)** | Reach an on-prem SAP gateway (RFC 33xx) from a cloud DuckDB running `erpl_rfc`; needs a stable `localhost:port`. |
| **Platform operator (Priya)** | Runs a fleet of ephemeral DuckDB query nodes; wants them to auto-enroll, be discoverable, and self-clean when they die. |
| **DataZoo maintainer** | Wants one extension, small surface, hermetic CI against real services, and a clear license/distribution story. |

## 5. Key use cases

- **UC1 — Quack over the mesh.** Ada enrolls two DuckDB nodes on a tailnet,
  lists peers, and tunnels the quack remote protocol from her laptop to a
  peer node, then runs a distributed query.
- **UC2 — SAP RFC over SSH.** Sven creates an `ssh` tunnel to a jump host and
  points `erpl_rfc` at `localhost:9001` → SAP gateway `3300`.
- **UC3 — HTTP data file over NetBird.** Ada tunnels to a private HTTP
  service and reads `http://localhost:9000/data.csv` via httpfs.
- **UC4 — Fleet discovery.** Priya's autoscaler brings up ephemeral DuckDB
  nodes tagged `duckdb`; any node runs `SELECT * FROM tunnel_peers()` to find
  live siblings by tag/name; dead nodes disappear automatically.

## 6. Functional requirements

### 6.1 Transport & lifecycle (all backends)

- **FR-1** Create a tunnel that binds a **local TCP listener** and forwards
  each connection to `remote_host:remote_port` over the selected backend.
- **FR-2** Bind the local listener to **`127.0.0.1` by default** (security
  fix vs erpl's `INADDR_ANY`); allow an explicit opt-in to bind all
  interfaces.
- **FR-3** List active tunnels (`tunnels()`) with backend, endpoints, local
  port, and status.
- **FR-4** Close one tunnel by id and close all tunnels; both idempotent and
  safe to call when none exist.
- **FR-5** Per-connection lifecycle is tracked (no fire-and-forget detached
  threads that outlive close); closing a tunnel tears down its in-flight
  connections deterministically.
- **FR-6** A `timeout` controls how long tunnel creation waits for the local
  listener to become connectable before returning an actionable error.

### 6.2 SSH backend (parity + fixes)

- **FR-7** Support password and private-key (from file/memory, with
  passphrase) auth, matching erpl.
- **FR-8** Either implement SSH-agent auth **or** fail with a clear
  "not supported / use key or password" message (no silent false).
- **FR-9** Resolve hostnames (not IPv4-only); support IPv6 endpoints where
  feasible.

### 6.3 Tailscale backend

- **FR-10** Bring up an in-process `tsnet` node from a single **auth key**;
  no daemon, no TUN, no root.
- **FR-11** Support **ephemeral** nodes (auto-removed when offline) and
  **tag** advertisement (e.g. `tag:duckdb`).
- **FR-12** Support a custom **control URL** (self-hosted Headscale) for
  hermetic/self-hosted operation.
- **FR-13** `Dial` arbitrary `remote_host:remote_port` on the tailnet
  (MagicDNS name or `100.x` IP) to feed the port-forward engine.

### 6.4 NetBird backend

- **FR-14** Bring up an in-process `client/embed` node from a single **setup
  key**; userspace WireGuard, no daemon/root.
- **FR-15** Support **ephemeral** peers, **group** assignment, and a
  configurable **management URL** (NetBird Cloud default or self-hosted).
- **FR-16** `Dial` arbitrary `remote_host:remote_port` on the NetBird
  network (DNS label or mesh IP).

### 6.5 Discovery (mesh backends)

- **FR-17** Enumerate mesh peers **peer-locally** (no control-plane API
  token) via `tunnel_peers()`, returning at least: backend, host name, DNS
  name/FQDN, mesh IP, tags/groups, online status.
- **FR-18** Allow filtering by tag/group/name so a caller can select "all
  `duckdb` nodes."
- **FR-19** This node **announces itself** implicitly by enrolling with a
  chosen **hostname** and **tag/group** (naming convention is the metadata
  channel). No separate announce listener is run. *(Decision D3.)*
- **FR-20** Optionally expose this node's own identity (`tunnel_self()` or a
  `self` flag) so a user can see the name/IP to hand to peers.

### 6.6 Configuration & secrets

- **FR-21** A single DuckDB `SECRET` type carries per-backend credentials and
  options (unified `TYPE tunnel` with a `backend` discriminator;
  `TYPE ssh_tunnel` retained as a backward-compatible alias).
- **FR-22** Secrets redact sensitive fields (`password`, `passphrase`,
  `auth_key`, `setup_key`, `private_key_path`).
- **FR-23** State that a mesh node must persist (WireGuard keys, node
  identity) is stored in a caller-configurable directory; ephemeral mode can
  use a throwaway dir so nothing persists.

### 6.7 Backend activation & mutual exclusion

- **FR-24** SSH works with **no Go runtime loaded**; the extension loads
  instantly and SSH-only sessions never pull in a mesh library.
- **FR-25** A mesh backend's Go library is **lazily loaded on first
  activation** (first `tunnel_up`/`tunnel_create`/`tunnel_peers` for that
  backend), not at extension load. *(Decision D1a.)*
- **FR-26** **Single mesh per process:** once one mesh backend is activated,
  activating the *other* mesh in the same process fails with a clear,
  actionable message (e.g. "Tailscale is active in this DuckDB process;
  NetBird cannot be loaded — start a new session"). *(Decision D1a.)*
- **FR-27** If a backend was not compiled into the current build (per the
  build-time bundle flag), activating it fails with a message naming which
  build to install, not an obscure symbol error.

## 7. Non-functional requirements

- **NFR-1 Zero dependency.** The shipped artifact is a single loadable
  extension; libssh2, OpenSSL, and the selected mesh Go shim(s) are
  linked/embedded in it. No external binary or service is required at
  runtime. (SSH-only builds carry no Go at all.)
- **NFR-2 Ease of use.** The happy path is ≤ 3 SQL statements
  (`CREATE SECRET`, `LOAD`, `tunnel_create`). Sensible defaults for ports,
  timeouts, control/management URLs, and state dirs.
- **NFR-3 Error quality.** Every failure returns a specific, actionable
  message with the probable cause and the fix — never a bare errno or a
  silent default. (Examples in §9.)
- **NFR-4 Robustness / idempotency.** Enrolling an already-enrolled node,
  creating a duplicate tunnel, or closing a non-existent tunnel are safe and
  clearly reported. Backend bring-up retries transient failures with bounded
  backoff.
- **NFR-5 Security.** Local listeners default to loopback; secrets are
  redacted; no credentials are logged; mesh state files are created with
  restrictive permissions.
- **NFR-6 Performance.** Forwarding overhead is dominated by the transport;
  the engine must stream (not buffer whole payloads) and sustain the quack
  protocol and bulk CSV/RFC transfers. Userspace-WireGuard/netstack overhead
  is accepted and documented.
- **NFR-7 Portability.** Linux (glibc) and macOS are first-class. The build
  is a per-platform CI matrix (cgo needs a target C toolchain). glibc is
  required (musl static is explicitly unsupported — see risk R1).
- **NFR-8 Footprint.** Each bundled mesh shim adds an estimated ~25–45 MB to
  the artifact (a `both` build carries two); this is accepted and must be
  documented. Because the mesh library is only `dlopen`'d on activation,
  SSH-only usage incurs no Go runtime memory cost.
- **NFR-9 Observability.** A verbosity/log setting surfaces backend bring-up
  progress (including the interactive login URL for first-run dev) without
  leaking secrets.

## 8. Success metrics / acceptance

- **M1** From a clean machine: `LOAD erpl_tunnel`, paste one key, and reach a
  remote endpoint in **≤ 3 statements and ≤ 60 s** (mesh cold-enroll
  included), on Linux and macOS.
- **M2** All three backends carry **quack**, **SAP RFC**, and **HTTP**
  traffic end-to-end in CI against **real** services (no mocks).
- **M3** SSH parity: every existing erpl `tunnel/` sqllogictest passes
  against the extracted extension.
- **M4** Discovery: a node can enumerate a tagged peer set peer-locally with
  no API token.
- **M5** Single-artifact, zero-external-dependency load verified on a
  container with nothing but glibc.

## 9. Ease-of-use bar — worked examples

Happy path (Tailscale):

```sql
LOAD erpl_tunnel;
CREATE SECRET ts (TYPE tunnel, backend 'tailscale',
                  auth_key 'tskey-auth-…', tags 'tag:duckdb', ephemeral true);
PRAGMA tunnel_create(secret = 'ts',
       remote_host = 'duckdb-eu-shard3', remote_port = 4213, local_port = 9000);
-- now: ATTACH 'ducklake://…@localhost:9000' or query quack at localhost:9000
```

Discovery then connect:

```sql
SELECT host_name, dns_name, mesh_ip, online
FROM   tunnel_peers(secret = 'ts') WHERE 'tag:duckdb' = ANY(tags);
```

Error-message bar (illustrative — the *quality* is the requirement):

- Missing secret → `Tunnel: secret 'ts' not found. Create it with CREATE
  SECRET ts (TYPE tunnel, backend 'tailscale', auth_key '…'). Not defaulting
  to an anonymous node.` *(erpl today silently defaults — must not.)*
- Bad/expired auth key → `Tailscale: auth key rejected (expired or wrong
  tailnet). Generate a reusable, tagged key at https://login.tailscale.com/…
  and set it in the 'ts' secret.`
- Peer unreachable → `Tunnel: remote 'duckdb-eu-shard3:4213' is enrolled but
  offline (last seen 4m ago). Check the peer, or pick another from
  tunnel_peers().`
- Local port busy → `Tunnel: local port 9000 is already in use. Choose a free
  local_port.`

## 10. Locked decisions (from stakeholder review, 2026-07-23)

| ID | Decision | Rationale |
|----|----------|-----------|
| **D1** | **Per-backend Go `c-shared` shims, one mesh live per process.** Each mesh (vendored `tsnet`; `netbird/embed`) is its own shim with an identical C ABI; SSH is C++/libssh2 with no Go. A **build-time flag** (`MESH_BACKEND=ssh\|tailscale\|netbird\|both`) selects which shims are bundled. | Tailscale and NetBird are never needed simultaneously, so we avoid the two-Go-runtimes clash by construction instead of forcing both into one archive. |
| **D1a** | **Lazy `dlopen` + single-mesh latch.** The mesh shim is embedded in the extension and loaded only on first activation; the first mesh activated wins, the other is refused in that process. | Zero Go cost for SSH-only use; only one Go runtime ever in-process; also defers Go init out of the extension-load path (mitigates R1). |
| **D4-ts** | **Tailscale = vendored `tsnet` + our own shim** (not `libtailscale`). | `libtailscale` has no releases and doesn't export peer `Status()` (needed for discovery); our shim gives full control and a C ABI symmetric with the NetBird shim. |
| **D2** | **Explicit local port-forward only.** No SOCKS/transparent interception. | Protocol-agnostic (quack/RFC/HTTP), matches erpl, simplest mental model; SAP RFC has no SOCKS support anyway. |
| **D3** | **Discovery via tags/naming only**, peer-local status; no app-level announce service or metadata bag. | Minimal surface; metadata encoded in hostname/tag/DNS-label. |
| **D4** | **DuckDB Community-Extension install UX, license BSL 1.1.** | Easiest install + consistency with erpl brand. **⚠ Conflict — see R2.** |

## 11. Risks & open decisions

- **R1 — Go runtime in a `dlopen`'d `.so`.** Real hazards: musl/Alpine
  segfaults, `dlopen`+cgo panics with some toolchains, signal-handler
  conflicts, ~25–45 MB per mesh, glibc-only. **Largely mitigated by D1a:** the
  mesh shim is `dlopen`'d **on demand** (not at extension load, and only if a
  mesh is used), after DuckDB's handlers are installed, and only one mesh runs
  per process. *Residual mitigation:* build/test on glibc; validate the
  extract-and-`dlopen` path per OS **early** (spike in M2); document the
  one-mesh-per-process rule. *(macOS: the extracted `.dylib` must satisfy code
  signing/notarization — see R7.)*
- **R2 — License/distribution conflict (D4).** The DuckDB **Community
  Extensions** registry requires an **OSI-approved** license; **BSL 1.1 is
  not OSI-approved**, so "community listing + BSL 1.1" cannot both hold as
  stated. **Options to resolve:** (a) keep BSL 1.1 and self-host a signed
  extension repo (erpl's current model — one extra `SET
  custom_extension_repository` step, *not* a community listing); (b) license
  the standalone extension under **MPL-2.0** (BSL's own change-license) to
  qualify for the community registry; (c) split — OSI core in community,
  commercial add-ons self-hosted. **Owner decision required before release.**
- **R3 — Mesh shim ownership.** No official C library for NetBird, and we
  vendor `tsnet` for Tailscale, so DataZoo maintains **both** cgo `c-shared`
  shims (identical C ABI) over `client/embed` and `tsnet`. Ongoing maintenance
  surface; pin versions. Prior art (Rust `netbird-embed`, `libtailscale`)
  de-risks the pattern but not the upkeep.
- **R4 — NetBird management-proto AGPL.** `client/embed` imports management
  protobuf types. Verify (`go list -deps ./client/embed | grep -E
  'management|signal|relay'` + license headers) that only permissive
  generated stubs link in, not AGPL server code. **Clear before shipping.**
- **R5 — Vendored `tsnet` upkeep.** We depend on `tailscale.com` (pinned)
  directly rather than `libtailscale` (which is unmaintained and, decisively,
  lacks a peer-`Status()` export for discovery). Track `tsnet` API drift on
  upgrade.
- **R6 — Windows.** Go `c-shared` on Windows is the weakest target; may slip
  to a later milestone (NG5).
  > **Retired (2026-07-25).** The risk was real but bounded, and every part of it
  > was measured rather than assumed: both shims build for `GOOS=windows`
  > (NetBird included, the biggest unknown), the mingw-built DLL depends only on
  > `KERNEL32` + UCRT forwarders, and the Go runtime provably initialises under an
  > MSVC host. Two assumptions did fail and were caught by spikes: an RCDATA
  > resource does not survive static linking, and Go 1.25.x silently skips
  > `init()` in a c-shared DLL. See [ADR-013](ADR-013-windows-mesh.md).
- **R7 — macOS embedded-`.dylib` signing.** The lazily-extracted mesh
  `.dylib` must be code-signed/notarized to `dlopen` cleanly under Gatekeeper;
  and writing an executable library to a temp dir must respect hardened
  runtime. *Mitigation:* sign the embedded blob at build; validate the
  extract-and-load path on macOS in the M2 spike (part of R1's spike).

## 12. Delivery phasing

- **M1 — Extract & parity.** Standalone `erpl-tunnel` with SSH only; sever
  erpl_rfc couplings; own `vcpkg.json`; port erpl's docker-compose sshd
  tests; fix loopback bind + agent-auth message + per-connection lifecycle.
  *Exit:* M3 metric (SSH parity) green.
- **M2 — Tailscale + the lazy-`dlopen` spike.** Build the `ts_shim`
  (`tsnet`, `c-shared`) with our C ABI; **prove the embed → extract →
  `dlopen` → activate path on Linux+macOS** (**highest risk, do first**,
  incl. macOS signing R7); implement the single-mesh latch and lazy loader in
  C++; tags + `tunnel_peers()`; hermetic CI via **Headscale**. *Exit:*
  quack/HTTP/RFC over Tailscale in CI, SSH-only load carries no Go.
- **M3 — NetBird.** Build the `nb_shim` (`client/embed`) exporting the **same
  C ABI** as `ts_shim`; wire it behind the same lazy loader/latch; setup-key
  auth, groups, DNS-label discovery; clear R4; hermetic CI via self-hosted
  NetBird stack. *Exit:* all-backend matrix green (incl. mutual-exclusion
  test).
- **M4 — Polish & release.** Error-message pass, docs, footprint docs,
  resolve R2 (license/distribution), signing/registry, footprint budget.

## 13. Testing philosophy (drives acceptance)

Red-green TDD against **real services and real protocols — no mocks**:

- **SSH:** dockerised `sshd` + a private HTTP service (reuse erpl's compose).
- **Tailscale:** **Headscale** (open-source control server) in CI for
  hermetic, cloud-free runs; optional real-tailnet smoke via an ephemeral
  `TS_AUTHKEY`.
- **NetBird:** **self-hosted NetBird** management/signal/relay stack in CI;
  real `client/embed` enrolls via a setup key.
- **Real payloads:** an actual DuckDB serving the **quack** remote protocol
  on one node, tunneled and queried from another; **SAP RFC** against the
  existing A4H docker where available; **HTTP** via httpfs.

Details and the harness design are in [`HLD.md`](HLD.md) §12.
