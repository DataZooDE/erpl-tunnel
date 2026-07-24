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

## The publication strategy (the key decision)

The DuckDB **community-extensions** registry builds every extension from source on
its **own standardized CI**, which has **no Go toolchain** and no hook to run custom
build steps (`extra_extension_config` is not passed through — confirmed from the
`erpl_idoc` submission, PR duckdb/community-extensions#2203). Consequences:

- **Only the SSH-only (`MESH_BACKEND=ssh`, no Go) build can be a community
  extension.** It builds with vcpkg + CMake exactly like `erpl_idoc`.
- **The mesh variants (`tailscale`/`netbird`/`both`) must be self-hosted** — they
  require `go build -buildmode=c-shared` at build time, which the registry CI cannot
  run. This is BRD **R2 option (c): OSI core in the community registry, mesh add-ons
  self-hosted** — and it matches how the whole erpl family already distributes (a
  signed repo + `SET custom_extension_repository`).

So "publishable" splits into two deliverables:
1. **Community**: `erpl_tunnel` SSH-only, OSI-licensed, on duckdb/community-extensions.
2. **Self-hosted**: `erpl_tunnel` ssh/tailscale/netbird/both on the erpl channel
   (signed repo), for the mesh features.

---

## Phase A — Publishability blockers (gate to the community registry)

- **A1 · License decision (R2) — OWNER DECISION, blocks everything.** Community
  requires an OSI-approved license; **BSL 1.1 is not OSI**. Options: (a) relicense to
  **MPL-2.0** (BSL's own change-license) so the community core qualifies; (b) keep BSL
  and self-host only. **Recommended: MPL-2.0 for the community core**, self-host mesh
  under the same license. Update `LICENSE`, `description.yml`, and all headers.
  *(Note: `erpl_idoc` submitted BSL-1.1 and its PR is "mergeable" pending a maintainer
  — verify the registry's current license gate before assuming BSL is rejected.)*
- **A2 · Make Go optional for the SSH-only build (concrete bug).**
  `CMakeLists.txt` calls `find_program(GO_EXECUTABLE go REQUIRED)` unconditionally, so
  `MESH_BACKEND=ssh` fails to configure without Go — which breaks the community CI.
  Move the `find_program` (and make it `REQUIRED` only) **inside** the mesh-bundled
  branches; the `ssh` build must configure and compile with **no Go present**.
- **A3 · Build the SSH-only core on the exact community toolchain** (DuckDB v1.5.4 /
  ci-tools `v1.5-variegata` / vcpkg `84bab45…`) and confirm green. The
  `-fno-gnu-unique` GCC guard is already committed in `extension_config.cmake` (the
  `erpl_idoc` lesson — the registry has no `extra_extension_config` passthrough); keep
  it there. Verify the loadable entrypoint name and that all sqllogictests pass.
- **A4 · Finalize `packaging/description.yml`** for the SSH-only core (license per A1,
  a real `docs.hello_world`/`extended_description`, `excluded_platforms` = windows
  mingw/rtools + wasm, matching sibling `erpl_web`/`erpl_idoc`). Pin `repo.ref` to a
  release commit.
- **A5 · Stand up the self-hosted channel for mesh variants** (signed extension repo,
  the erpl `get.erpl.io` flow already in `MainDistributionPipeline.yml` +
  `_extension_deploy.yml`). Document the `SET custom_extension_repository` + `INSTALL
  erpl_tunnel` install path for mesh users.
- **A6 · Codex review round #1 (build/packaging):** have Codex review the CMake
  Go-conditional change, `extension_config.cmake`, and `description.yml` against the
  community-extensions `build.yml` contract and the `erpl_idoc` precedent.

**Exit:** SSH-only core builds green on the community toolchain with no Go; license
resolved; description.yml final; self-hosted mesh channel live.

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
- **E2 · Windows decision (NG5/R6).** libssh2 SSH-only could build on Windows for the
  community core (mesh stays deferred — Go `c-shared` on Windows is the weakest
  target). Decide: enable SSH-only Windows, or keep excluded.
- **E3 · musl / glibc-only guard.** A clear build-time/runtime error on musl for the
  mesh path; document the glibc floor and the manylinux CI build for portability.

## Cross-cutting: Codex review cadence

Four scheduled Codex rounds (above): **#1 build/packaging** (Phase A), **#2 feature
code** (Phase B), **#3 tests** (Phase C), **#4 documentation** (Phase D), plus a
**final pre-PR review** of the whole publishability checklist. Each round: hand Codex
the diff + context, apply verified findings, re-verify.

## Sequencing & the release

1. **A1 (license) is the gate** — owner decision first; nothing ships to the registry
   without it.
2. **A2–A3** (Go-optional + community-toolchain build) in parallel with **B1–B2**
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

- [ ] License OSI-resolved (A1); headers + `LICENSE` + `description.yml` consistent.
- [ ] SSH-only core builds green on the community toolchain **with no Go**; all
      sqllogictests + core_tests pass.
- [ ] Mesh variants self-hosted + install-documented; both data-plane E2Es green in CI.
- [ ] Telemetry parity + `TELEMETRY.md`; `tunnels().backend`; error taxonomy; log
      setting.
- [ ] Unit tests incl. redaction; quack/RFC payloads (or documented deferral).
- [ ] README + guides rewritten; a new user can reach a first tunnel from docs alone
      (Codex-verified).
- [ ] macOS signed; Windows/musl decisions documented.
- [ ] community-extensions PR opened against a pinned release ref.
