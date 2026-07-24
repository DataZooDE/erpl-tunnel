# Publication-readiness plan — closing the gaps to ship `erpl_tunnel`

> Goal: make `erpl_tunnel` **publishable on the DuckDB community-extensions
> registry** (SSH-only core) and **self-hosted** (mesh variants), close every gap
> from the original BRD/HLD plan, harden tests, and rewrite the docs to guide a new
> user — with Codex code/doc review rounds baked in.

| | |
|---|---|
| **Status** | Plan for review |
| **Date** | 2026-07-24 |
| **Companions** | [`BRD.md`](BRD.md), [`HLD.md`](HLD.md), [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md), [`TIER2_MESH_DATAPLANE_TEST.md`](TIER2_MESH_DATAPLANE_TEST.md) |

## The publication strategy (decided)

**License: BSL 1.1** (decided — matches the whole erpl family; → MPL-2.0 after the
change date). **Versioning: CalVer**, same as erpl / erpl-web — git tags
`vYYYY.MM.DD` (optional `.N` for same-day re-releases) and the identical `YYYY.MM.DD`
string stamped on telemetry (`SetProduct` + `CaptureExtensionLoad`). *(Applied
2026-07-24: single `ERPL_TUNNEL_VERSION` constant + `description.yml version`.)*

**Everything ships to the DuckDB community-extensions registry** — one `erpl_tunnel`
extension built `MESH_BACKEND=both` (all backends, runtime-selected via the latch).
BSL is fine (the registry already hosts BSL extensions; `erpl_idoc`'s BSL PR is
mergeable-pending-maintainer). Self-hosting is an optional fallback, not the plan.

The registry builds from source on **extension-ci-tools** (`v1.5-variegata` for
DuckDB v1.5.4). Two verified facts drive the build work — the mesh shims are Go, and:

1. **The CI's Go is too old for our shims.** The linux **manylinux** container pins
   **Go 1.20.5**; the macOS/Windows runners get **1.23** (`actions/setup-go@v4`). Our
   shims need **tsnet ≥ 1.24** and **netbird go.mod `go 1.25.5`**. Worse, Go 1.20
   predates `GOTOOLCHAIN` auto-download, so it can't self-upgrade. → **We must bootstrap
   our own pinned Go** in the CMake shim build (download `go1.25.x` for the target
   arch, use it for `-buildmode=c-shared`), making the build independent of the CI's
   Go pin. Committed in `CMakeLists.txt`; needs network at build time (available).
2. **Toolchain + platform declaration.** `description.yml` must declare the Go
   toolchain (`requires_toolchains`/`enable_go`) *and* exclude every platform the mesh
   (Go cgo `c-shared`) can't target: **wasm, windows, and musl** (the musl containers
   install no Go, and cgo can't target musl — BRD C3). Only glibc linux amd64/arm64 +
   osx amd64/arm64 remain.

No `extra_extension_config` passthrough on the registry → everything (the
`-fno-gnu-unique` guard, `MESH_BACKEND=both`, the Go bootstrap) lives in committed
`extension_config.cmake` / `CMakeLists.txt`. This is the `erpl_idoc` lesson.

Deliverable: **one BSL, CalVer `erpl_tunnel` (`both`) on duckdb/community-extensions**,
glibc linux + macOS. Self-hosted mirror optional.

---

## Phase A — Publishability blockers (gate to the community registry)

- **A1 · License = BSL 1.1 (DECIDED).** Keep `LICENSE` (BSL 1.1, → MPL-2.0 after the
  change date), matching the erpl family; `description.yml` `license: BSL-1.1`; BSL
  headers on new source files. No relicense. The only open bit is *whether the
  community registry accepts BSL* — a best-effort attempt (A4), not a blocker, since
  self-hosted is the primary channel. **Versioning = CalVer (DONE):** single
  `ERPL_TUNNEL_VERSION` constant used by both telemetry calls; `description.yml
  version: 2026.07.24`; releases tagged `vYYYY.MM.DD`.
- **A2 · Bootstrap a pinned Go toolchain in the CMake shim build.** The CI's Go
  (1.20.5 linux / 1.23 runners) is too old and can't auto-upgrade. Add a CMake step
  that downloads a pinned `go1.25.x` for the build arch (linux amd64/arm64, osx
  amd64/arm64), verifies its checksum, and uses it for `go build -buildmode=c-shared`
  (drop `GOTOOLCHAIN=local` → point `GOROOT`/`PATH` at the fetched toolchain). Cache
  it. Also make `find_program(go)` only `REQUIRED` when a mesh backend is bundled, so a
  bare `MESH_BACKEND=ssh` build still needs no Go at all.
- **A3 · Make the community build produce `both`, config in committed files.** In
  `extension_config.cmake` (no `extra_extension_config` passthrough on the registry):
  `set(MESH_BACKEND "both")`, keep the `-fno-gnu-unique` GCC guard, and any Go-bootstrap
  glue. Build on the exact community toolchain (v1.5.4 / `v1.5-variegata` / vcpkg
  `84bab45…`) and confirm green incl. the Go shims; verify the loadable entrypoint name
  and all sqllogictests pass.
- **A4 · Finalize `packaging/description.yml`.** `license: BSL-1.1`, `version`
  CalVer, real `docs.hello_world`/`extended_description`; declare the **Go toolchain**
  (`requires_toolchains: go` / `enable_go`); `excluded_platforms` = **wasm + all
  windows + all musl** (glibc-only Go cgo). Pin `repo.ref` to the release commit.
- **A5 · Optional self-hosted mirror.** The erpl `get.erpl.io` flow
  (`MainDistributionPipeline.yml` + `_extension_deploy.yml`) can also publish the
  variants for `SET custom_extension_repository` users — nice-to-have, not required.
- **A6 · Codex review round #1 (build/packaging):** Codex reviews the Go-bootstrap
  CMake, `extension_config.cmake`, and `description.yml` against the community-extensions
  `build.yml` contract, the ci-tools Go/toolchain handling, and the `erpl_idoc`
  precedent — especially the manylinux Go-download path and the platform exclusions.

**Exit:** the `both` extension builds green on the community toolchain (Go shims
included via the bootstrapped toolchain) across glibc linux + macOS; `description.yml`
final; license/version consistent.

## Phase B — Feature/quality gaps from the original plan

- **B1 · Telemetry parity (you flagged this).** Instrument the **new mesh functions**
  (`tunnel_peers`, `tunnel_self`, `tunnel_mesh_activate`) with `RecordFunctionCall`
  (erpl-idoc instruments every function). Record the **backend** (`ssh`/`tailscale`/
  `netbird`) as a safe enum property on `tunnel_create`. Keep the privacy contract
  (enums/numbers only — never hosts/keys/SQL). Ship **`TELEMETRY.md`** (every erpl
  extension has one).
- **B2 · `tunnels()` `backend` column (HLD §8.1).** Add a `backend` discriminator so
  ssh/tailscale/netbird tunnels are distinguishable (today mesh rows have empty
  `ssh_*`). Consider `dns_name`/`mesh_ip` for mesh rows too.
- **B3 · Error taxonomy (§8.6).** Introduce the typed categories
  (`AuthRejected`, `ControlUnreachable`, `PeerOffline`, `PortInUse`, `Timeout`,
  `ConfigMissing`, `Unsupported`) mapping libssh2/mesh-C-ABI codes → one actionable
  message renderer, replacing today's ad-hoc inline strings (keeps NFR-3 quality but
  gives the structure the HLD specified).
- **B4 · Observability / log verbosity (NFR-9).** A `SET erpl_tunnel_log_level` (or
  a secret/pragma option) that surfaces backend bring-up progress — including the
  interactive login URL on first-run dev — without leaking secrets. Route the Go shim
  stderr through it.
- **B5 · Robustness polish (NFR-4).** Bounded-backoff bring-up retries in the mesh
  state machine (`Degraded ⇄ Up`); confirm duplicate-tunnel / already-enrolled /
  close-nonexistent are all idempotent with clear messages (mostly done — add the
  missing tests in Phase C).
- **B6 · Codex review round #2 (feature code):** Codex reviews the new C++
  (taxonomy, telemetry, backend column, log setting) for correctness, leaks, and
  privacy-contract adherence.

## Phase C — Test hardening ("very good tests")

Today: strong **integration** (9 real-service scripts, no mocks) but **zero unit
tests**. Close that.

- **C1 · C++ unit tests (Catch2, like `erpl_idoc` `core_tests`).** Standalone,
  DuckDB-free where possible: secret parse + **redaction**, the `tunnel_peers` JSON
  parser (`mesh_backend.cpp`), the error-taxonomy mapping (B3), and the port-forward
  pump framing. Add a `core_tests` Make target + a fast CI job.
- **C2 · Secret-redaction sqllogictest (FR-22).** Assert `auth_key`/`setup_key`/
  `password`/`passphrase`/`private_key_path` are redacted in the secret display —
  security-relevant and currently unverified.
- **C3 · Mesh output-schema tests.** `tunnel_peers`/`tunnel_self` column names/types
  and filtering by tag/group (FR-18).
- **C4 · The M2 flagship payloads (quack + SAP RFC).** HTTP over all three backends
  is done; add: **quack** — a real DuckDB served over the tunnel (research the
  `httpserver` community extension or `ATTACH`-over-tunnel as the quack transport),
  queried through the tunnel end-to-end; **SAP RFC** — `erpl_rfc` pointed at
  `localhost:port` → A4H gateway where available (self-skips otherwise).
- **C5 · One green gate + CI.** A `make test` that runs sqllogictests + core_tests +
  (docker-gated) integration; ensure the mesh-tests and dataplane jobs are all wired.
- **C6 · Explicit unsupported-platform coverage.** A test/doc asserting the
  actionable error on musl/no-Go-mesh and the glibc-floor behaviour (NFR-7).
- **C7 · Codex review round #3 (tests):** Codex audits coverage for gaps and
  flaky-timing risks in the mesh/dataplane scripts.

## Phase D — Documentation overhaul (rewrite to guide the user)

Today's docs are **design docs** (BRD/HLD/plan/tier2/agpl) — excellent for us, wrong
shape for a **user**. Restructure into user-facing guides + a design archive.

- **D1 · Restructure.** Move design docs to `docs/design/` (BRD, HLD,
  IMPLEMENTATION_PLAN, TIER2, NETBIRD_AGPL_AUDIT, this plan). Create `docs/guides/`
  for user-facing content.
- **D2 · Rewrite `README.md` as the front door.** What/why in three sentences;
  **install** (community SSH-only vs self-hosted mesh); the ≤3-statement quickstart;
  the status matrix; links into the guides. No design-doc dumping.
- **D3 · New user guides** (`docs/guides/`):
  - `getting-started.md` — install + first SSH tunnel.
  - `tailscale.md` — get an auth key / point at Headscale; tags; ephemeral; discovery.
  - `netbird.md` — setup key / self-hosted management; groups; **the userspace-firewall
    note**; discovery.
  - `discovery.md` — `tunnel_peers`/`tunnel_self`, filtering, the naming convention.
  - `security.md` — loopback bind, redaction, state-dir perms, self-hosted-control.
  - `one-mesh-per-process.md` — the latch rule, footprint, `MESH_BACKEND` variants.
  - `building.md` — from source, `MESH_BACKEND`, Go/glibc requirements, the seeder.
  - `troubleshooting.md` — the real gotchas we hit: NetBird ipset →
    `NB_FORCE_USERSPACE_FIREWALL`, Headscale plain-HTTP DERP → same-subnet direct,
    glibc floor, filename-derived entrypoint, "one mesh per process".
- **D4 · `TELEMETRY.md`** (privacy contract, opt-out) + `CONTRIBUTING.md` (build/test
  loop, PR conventions).
- **D5 · Codex documentation-review round #4.** Codex reads the rewritten README +
  guides **as a new user would**: is the install path unambiguous? can a user get a
  first tunnel working from the docs alone? are the mesh gotchas discoverable? Are all
  commands accurate against the current code? Iterate once on its findings.

## Phase E — Platform & portability

- **E1 · macOS build + `.dylib` signing/notarization (R7).** Required for the mesh
  shims to `dlopen` under Gatekeeper; sign the embedded blob at build; validate
  extract-and-load on osx/arm64 + osx/amd64.
- **E2 · Windows + musl excluded (NG5/R6, C3).** Since the single community artifact
  is `both` (Go), it can't target Windows or musl — both are in `excluded_platforms`.
  (A separate ssh-only Windows build isn't an option: community-extensions is one
  artifact per name.) Document this; no per-platform split.
- **E3 · glibc floor.** Building on the manylinux community container gives a low,
  portable glibc floor (better than a local rolling-distro build). Document it and the
  musl-unsupported behaviour; keep a clear error if a mesh path is ever hit on musl.

## Cross-cutting: Codex review cadence

Four scheduled Codex rounds (above): **#1 build/packaging** (Phase A), **#2 feature
code** (Phase B), **#3 tests** (Phase C), **#4 documentation** (Phase D), plus a
**final pre-PR review** of the whole publishability checklist. Each round: hand Codex
the diff + context, apply verified findings, re-verify.

## Sequencing & the release

1. **A1 (license) is settled — BSL 1.1, CalVer.** No gate here anymore; self-hosted
   is the primary channel, the community PR is best-effort.
2. **A2–A3** (Go-bootstrap + `both` community-toolchain build) in parallel with **B1–B2**
   (telemetry + backend column — cheap, high value).
3. **B3–B5** (taxonomy, logging, robustness) with **C1–C3** (unit + redaction tests),
   Codex round #2/#3.
4. **C4** (quack/RFC) — the heaviest test work; can trail.
5. **D** (docs overhaul) after the surface stabilises, Codex round #4.
6. **E** (platforms) — macOS signing before shipping mesh on macOS.
7. **Cut `v0.1.0`**: tag, self-host all variants on the erpl channel, open the
   community-extensions PR for the SSH-only core (fork `DataZooDE/community-extensions`,
   branch `add-erpl-tunnel`, add `extensions/erpl_tunnel/description.yml`, pin
   `repo.ref`), shepherd through maintainer CI approval + merge.

## Definition of done (publishable)

- [ ] License = BSL 1.1 consistent across `LICENSE` + `description.yml` + headers;
      CalVer version single-sourced (DONE).
- [ ] The `both` extension builds green on the **community toolchain** (Go shims via
      the bootstrapped `go1.25.x`) across glibc linux amd64/arm64 + osx amd64/arm64;
      all sqllogictests + core_tests pass; wasm/windows/musl excluded.
- [ ] `description.yml` declares the Go toolchain + exclusions; both data-plane E2Es
      green in our own CI. Community PR opened against a pinned release ref.
- [ ] Telemetry parity + `TELEMETRY.md`; `tunnels().backend`; error taxonomy; log
      setting.
- [ ] Unit tests incl. redaction; quack/RFC payloads (or documented deferral).
- [ ] README + guides rewritten; a new user can reach a first tunnel from docs alone
      (Codex-verified).
- [ ] macOS signed; Windows/musl decisions documented.
- [ ] community-extensions PR opened against a pinned release ref.
