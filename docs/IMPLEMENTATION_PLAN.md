# Implementation Plan — `erpl-tunnel`

> How to build the extension specified in [`BRD.md`](BRD.md) and [`HLD.md`](HLD.md),
> in one coherent pass, using the existing erpl DuckDB extensions as the blueprint.

| | |
|---|---|
| **Status** | Ready to execute |
| **Date** | 2026-07-23 |
| **Base** | `DataZooDE/erpl` → `tunnel/` module (~2000 LOC C++, SSH-only) |
| **Blueprint** | `DataZooDE/erpl-idoc` (standalone repo layout, CMake/Makefile/CI/vcpkg/telemetry) |
| **Repo** | `DataZooDE/erpl-tunnel` (private until license R2 resolved) |

---

## Delivery status (2026-07-23)

Implemented and verified against **real services, no mocks**:

- **Phase 0/0b** ✅ — standalone extension builds; SSH parity green (docker sshd +
  private HTTP service); real HTTP payload forwarded through the tunnel.
- **Phase 1 (M1)** ✅ — erpl defects fixed red/green: loopback-default bind (proven
  via `ss`), tracked-and-joined per-connection workers (deterministic teardown),
  agent-auth actionable failure, missing-secret hard error, `getaddrinfo` resolution.
- **Phase 2 (M2)** ✅ — `ts_shim` (tsnet) c-shared; the Go-in-`dlopen` spike; lazy
  activation proven zero-Go-at-load (`/proc/pid/maps`); real Tailscale enrollment +
  `tunnel_create` over the tailnet against hermetic **Headscale**; unified secret;
  `tunnel_peers`/`tunnel_self`.
- **Phase 3 (M3)** ✅ (core) — `nb_shim` (client/embed) with the **identical C ABI**;
  **single-mesh latch proven symmetrically** with both real shims embedded; R4 AGPL
  audit cleared (`docs/NETBIRD_AGPL_AUDIT.md`).
- **Phase 4 (M4)** ✅ (core) — **zero-dependency verified** (glibc-only `ldd` + bare
  `--network none` container load); footprint docs; mesh CI workflow; README.

Remaining / not yet done in this pass:

- **NetBird live enrollment E2E** — the self-hosted management/signal/relay + IdP
  stack is heavy and flaky to stand up; ABI + latch + AGPL are proven, live
  enrollment is scaffolded (`shim/nb`) but not run against a real NetBird control
  plane. (M3 exit's NetBird-enroll leg.)
- **Two-node quack data-plane over the tailnet** — `tunnel_create` over mesh is
  proven to bind/list; a full A-serves-quack / B-queries-through-tunnel run is not
  yet automated.
- **macOS build + `.dylib` signing (R7)** and **Windows (NG5/R6)** — Linux only here.
- **R2 (license/distribution)** — still the owner decision that gates public release.

---

## 0. Orientation — what we are reusing vs. building

**Extract & keep (from `erpl/tunnel/src/`):** `tunnel_extension.cpp`,
`tunnel_connection.{cpp,hpp}` (624 LOC — the port-forward engine + libssh2),
`ssh_tunnel_client.cpp`, `tunnel_secret.{cpp,hpp}`, `tunnel_manager.{cpp,hpp}`,
`pragma_tunnel_{create,close,close_all}.cpp`, `scanner_tunnels.cpp`, and the
`test/integration/` docker-compose sshd harness + `test/sql/*.test`.

**Blueprint to copy (from `erpl-idoc/`):** top-level `CMakeLists.txt`,
`extension_config.cmake`, `Makefile`, `.gitmodules` (duckdb + extension-ci-tools +
posthog-telemetry submodules, pinned to `v1.5.4`/`v1.4-andium`), `.github/workflows/`
(`MainDistributionPipeline.yml` + `_extension_deploy.yml`), `.gitignore`, `LICENSE`,
`packaging/description.yml`, and the `-fno-gnu-unique` GCC workaround.

**Build new:** the `TransportBackend` abstraction, the two Go `c-shared` mesh shims
(`ts_shim`, `nb_shim`), the lazy-`dlopen` loader + single-mesh latch, the unified
secret model, `tunnel_peers()`/`tunnel_self()`, and the mesh CI harnesses
(Headscale, self-hosted NetBird).

**Couplings to sever (BRD §2.1):** the current `tunnel/CMakeLists.txt` links
`erpl_rfc_extension` and includes `../rfc/src/include` *only* for PostHog telemetry.
Replace with the vendored `posthog-telemetry` submodule (as erpl-idoc does) and this
module's own `vcpkg.json`.

---

## Phase 0 — Repo scaffolding (bootstrap the standalone build)

Goal: `make debug` builds an SSH-parity extension in a fresh clone. No behaviour
change yet — pure extraction into the erpl-idoc layout.

1. **Submodules & pins** — add `duckdb` (`v1.5.4`), `extension-ci-tools`
   (`v1.5.4`), `posthog-telemetry` (`DataZooDE/posthog-telemetry`), matching
   erpl-idoc `.gitmodules`.
2. **Top-level CMake** — port `erpl-idoc/CMakeLists.txt`: set
   `TARGET_NAME=erpl_tunnel`; `find_package(OpenSSL)` + `libssh2 CONFIG`; keep the
   `-fno-gnu-unique` GCC guard; drop tinyxml2, add libssh2.
3. **`extension_config.cmake` + `Makefile` + `.github/workflows/`** — copy from
   erpl-idoc, rename `erpl_idoc`→`erpl_tunnel`. Keep the dual DuckDB matrix
   (v1.5.4 + v1.4.5) and the `exclude_archs` (Windows/wasm off — BRD NG5).
4. **`vcpkg.json`** — `{"dependencies": ["libssh2", "openssl"]}` (static triplet).
5. **`LICENSE`** — carry BSL 1.1 for now; the license text is a packaging decision
   (R2), tracked separately, no code impact.
6. **Move sources** — copy `erpl/tunnel/src/**` into `src/`; rename
   `erpl_tunnel_extension.hpp` include paths; remove `../rfc/src/include` and
   `erpl_rfc_extension` link; swap telemetry include to the submodule.
7. **Smoke test** — `make debug && make sql_tests`: the extracted SSH-only
   extension loads and the ported `test/sql/*.test` pass (these become the M3
   parity baseline).

**Exit:** clean-clone `make debug` green; SSH `tunnels()`/`tunnel_create` work.

---

## Phase 1 — M1: SSH parity + erpl-defect fixes

Refactor the extracted code onto the target architecture and fix the inherited
defects, all behind the existing SSH tests plus new red-green tests per fix.

1. **Introduce `TransportBackend`** (HLD §5, §8.3) — interface `EnsureUp()`,
   `Dial(host,port)→Stream`, `Peers()`, `Self()`, `Close()`. Refactor the existing
   libssh2 code in `tunnel_connection.cpp` into `SshBackend` implementing it;
   `Dial` opens a `direct-tcpip` channel; `Peers()` returns unsupported/empty.
   `Stream` is an fd-or-channel handle the port-forward engine pumps uniformly.
2. **Port-forward engine** — extract the accept-loop + 16 KB bidi pump into a
   backend-agnostic engine consuming `backend.Dial(...)`.
3. **Fix FR-2 / ADR-006** — bind `127.0.0.1` by default (was `INADDR_ANY`); add an
   explicit opt-in flag for all-interfaces. *Red test: assert no bind on `0.0.0.0`.*
4. **Fix FR-5 / §8.3** — per-connection lifecycle tracking; `tunnel_close` signals
   and **joins** workers (kills erpl's detached-thread leak). *Red test: open N
   conns, close tunnel, assert 0 residual threads/fds.*
5. **Fix FR-8** — SSH-agent auth: implement, or fail with the actionable
   "not supported — use key or password" message (no silent `false`). *Red test.*
6. **Fix FR-9** — hostname resolution (not IPv4-only); IPv6 where feasible.
7. **Fix §8.2** — a missing secret is a hard, actionable error; never the silent
   localhost/agent default. *Red test.*
8. **FR-6** — `timeout` param gates a connectability probe of the local listener
   with an actionable error.
9. **Error taxonomy skeleton (Q4/§8.6)** — introduce the typed categories
   (`AuthRejected`, `PortInUse`, `Timeout`, `ConfigMissing`, `Unsupported`, …) and
   route SSH/libssh2 codes through it now, so mesh backends plug in later.

**Exit (BRD M3):** every ported erpl `tunnel/` sqllogictest passes; each fixed
defect has a test at the layer it manifests, wired into the green gate.

---

## Phase 2 — M2: Tailscale + the lazy-`dlopen` spike (**highest risk, do first**)

Per BRD R1/R7 and HLD §12, prove the embed→extract→`dlopen`→activate path *before*
building on it.

1. **`ts_shim` (Go `c-shared`)** — vendor `tsnet` (pinned, ADR-007), export the
   exact C ABI in HLD §5.2 (`mesh_kind/new/set_str/set_bool/up/dial/peers_json/
   self_json/errmsg/close`). Streams handed back as **OS fds** (the libtailscale fd
   trick) so C++ never touches Go memory. Build via `go build -buildmode=c-shared`
   (Go ≥ 1.25) as a CMake custom target; code-sign on macOS (R7); embed the
   resulting `.so`/`.dylib` into the extension as a byte blob.
2. **The spike (do first)** — a minimal test that: (a) `LOAD`s the extension and
   asserts **no Go mapped** into the process; (b) triggers activation so the blob is
   extracted to a `0700` temp file, `dlopen`'d, symbols resolved — exactly the
   runtime path — on **linux/amd64, linux/arm64, osx/amd64, osx/arm64** (incl. macOS
   signing/Gatekeeper); (c) asserts the single-mesh latch refuses a second mesh.
   *This is the gating de-risk — if it fails, revisit ADR-001/011 before more work.*
3. **Mesh loader + single-mesh latch (§6.5, ADR-011)** — C++ loader: extract blob →
   `dlopen` → resolve `mesh_*`. Process-global latch records active `mesh_kind`;
   second-different-mesh and not-in-this-build both fail with the actionable message
   (FR-26/27), not a `dlsym` error. Lazy: SSH never reaches the loader (FR-24).
4. **`MeshBackend` (C++)** — implements `TransportBackend` over the C ABI; `Dial`→
   `mesh_dial` fd; `Peers/Self`→`mesh_*_json`. `EnsureUp` idempotent, node-per-
   identity, ref-counted (ADR-008).
5. **Build-time bundle flag (ADR-012)** — `MESH_BACKEND=ssh|tailscale|netbird|both`
   selects which shim blobs embed. `ssh` carries no Go.
6. **Unified secret model (ADR-005)** — one `TYPE tunnel` parser with a `backend`
   switch; per-backend required/optional fields; redact
   `{password,passphrase,private_key_path,auth_key,setup_key}`; keep `ssh_tunnel`
   as a backward-compatible alias.
7. **Discovery** — `tunnel_peers()` + `tunnel_self()` table functions reading the
   shim's peer-local status JSON; tags/`ephemeral`/`control_url` (Headscale) support
   (FR-11/12/17/20).
8. **Hermetic CI** — Headscale control server via docker-compose; end-to-end
   **quack** between two real DuckDB nodes over the tailnet; HTTP via httpfs; RFC
   where A4H is reachable. No mocks (ADR-009).

**Exit (BRD M2/M5):** quack/HTTP/RFC over Tailscale green in CI; SSH-only load
carries no Go (verified on a bare-glibc container); latch test green.

---

## Phase 3 — M3: NetBird (second shim behind the same seam)

1. **`nb_shim` (Go `c-shared`)** — over `netbird/client/embed`, exporting the
   **identical** C ABI as `ts_shim` (so the C++ `MeshBackend` binds one symbol set).
   Setup-key auth, groups, `management_url`, DNS-label discovery (FR-14/15/16).
2. **Clear R4 (AGPL)** — `go list -deps ./client/embed | grep -E
   'management|signal|relay'` + license-header audit; confirm only permissive
   generated stubs link, not AGPL server code. **Gate before shipping.**
3. **Wire behind the existing loader/latch** — no new C++ seam; the mutual-exclusion
   test (activate TS then attempt NB in one process → refused, process healthy)
   proves ADR-011.
4. **Hermetic CI** — self-hosted NetBird management/signal/relay stack via
   docker-compose; real `client/embed` enrolls via setup key; quack/HTTP/RFC.

**Exit:** all-backend matrix green incl. the mutual-exclusion test.

---

## Phase 4 — M4: Polish & release

1. **Error-message pass (Q4/BRD §9)** — every failure mode renders the worked
   actionable message; no secret in any message or log; audit the taxonomy.
2. **Docs** — README (erpl-idoc style: highlights, 3-statement happy path, worked
   examples), the one-mesh-per-process rule, footprint budget (NFR-8, ~25–45 MB per
   bundled mesh), glibc-only note (NFR-7), self-hosted-control security note (§8.7).
3. **Resolve R2 (license/distribution)** — owner decision: BSL 1.1 + self-hosted
   signed repo, **or** MPL-2.0 for the community registry, **or** split. Then wire
   `packaging/description.yml` + signing/registry accordingly. **Blocks public
   release / repo-visibility flip.**
4. **Footprint & signing** — macOS notarization of the embedded shim blobs;
   document the `both`-build size.

**Exit:** M1–M5 acceptance metrics green; release artifacts per (OS, arch, DuckDB
version, bundle flag).

---

## Cross-cutting: testing philosophy (BRD §13 / HLD §12)

Red-green TDD against **real services, no mocks** throughout — not just at the end:
- **SSH:** ported erpl docker `sshd` + private HTTP service.
- **Tailscale:** Headscale in docker (hermetic); optional real-tailnet smoke via
  ephemeral `TS_AUTHKEY`.
- **NetBird:** self-hosted stack in docker.
- **Real payloads:** a real DuckDB serving **quack** on one node, tunneled and
  queried from another; SAP RFC against A4H where available; HTTP via httpfs.
- Every fixed erpl defect gets a test at its layer, wired into the green gate.

## Sequencing rationale & risk order

1. **Phase 0/1 first** — cheap, high-certainty, delivers immediate value (a working
   standalone SSH extension = M3 parity) and establishes the `TransportBackend` seam
   the mesh backends slot into.
2. **Phase 2's lazy-`dlopen` spike is the single highest-risk item** (Go-in-`dlopen`,
   macOS signing) — front-loaded so a failure reshapes the architecture *before*
   NetBird is built on top.
3. **Phase 3 reuses Phase 2's seam** — NetBird is "second shim, same ABI, same
   latch," so its risk is mostly the AGPL clearance (R4), not new architecture.
4. **Phase 4 gates public release on R2** (license) — the only reason the repo is
   private today.

## Open items to resolve during execution

- **R2 (license/distribution)** — owner decision before public release / going
  community-registry vs. self-hosted signed repo.
- **R4 (NetBird AGPL)** — dependency audit must pass before shipping NetBird.
- **NG5/R6 (Windows)** — deferred; confirm still acceptable at M4.
- **Go toolchain in CI** — the erpl-idoc CI matrix has no Go step; the mesh jobs add
  `go build -buildmode=c-shared` per target arch (cgo needs the target C toolchain).
