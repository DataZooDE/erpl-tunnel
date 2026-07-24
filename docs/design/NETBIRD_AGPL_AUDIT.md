# NetBird AGPL audit (BRD R4)

> Verify that `nb_shim` links only permissive NetBird **client** code, not the
> **AGPL-licensed management/signal/relay server** code.

## Why this matters

NetBird's repository is mixed-license: the **client** is BSD-3-Clause, but the
**management server, signal server, and relay server** are **AGPL-3.0**. If the
`client/embed` dependency tree pulled server packages into our shim, the whole
loadable extension would be encumbered by AGPL. R4 requires clearing this before
shipping the NetBird backend.

## Method

```sh
cd shim/nb
# 1. Which netbird packages does the embed build actually link?
go list -deps . | grep 'github.com/netbirdio/netbird/'
# 2. Do any AGPL server packages appear?
go list -deps . | grep -E 'netbird/(management|signal|relay)/server'
```

## Result (netbird v0.74.7, checked 2026-07-23)

- The linked `netbirdio/netbird/*` packages are all under `client/*`, `shared/*`,
  `formatter/*`, `route`, `util`, and generated `shared/management/{domain,
  operations,status}` proto/type stubs.
- **No `management/server`, `signal/server`, or `relay/server` package is linked.**
  The AGPL server code does not enter the dependency graph of `client/embed`.
- `shared/management/*` are generated protobuf message/type definitions and status
  enums (permissive), not the server implementation.

**Conclusion:** the NetBird backend links only permissive (BSD-3) client + shared
code. R4 is cleared for v0.74.7. Re-run the two `go list -deps` checks on every
NetBird version bump (pinned in `shim/nb/go.mod`); treat any `*/server` package
appearing in the output as a release blocker.

## Notes

- NetBird's `client/embed` requires NetBird's own go.mod **replace** directives
  (forked `pion/ice`, `cloudflare/circl`, `wireguard-go`, …) to resolve. These are
  mirrored verbatim in `shim/nb/go.mod` and pinned; bumping NetBird means
  re-syncing that replace block (BRD R3, the shim-maintenance surface).
