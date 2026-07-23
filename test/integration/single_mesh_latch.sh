#!/usr/bin/env bash
# Single-mesh latch proof (ADR-011 / BRD FR-26). NO MOCKS.
#
# A `both`-bundled extension embeds BOTH mesh shims. This asserts the process-global
# latch: the first mesh activated in a process wins; activating the OTHER mesh in the
# same process is refused with an actionable message — while the process stays healthy.
# Two independent Go runtimes can never coexist, so the clash is impossible by
# construction (only one shim is ever dlopen'd).
#
# The latch is set at activation (dlopen) time, BEFORE any enrollment. We use
# PRAGMA tunnel_mesh_activate, which exercises the loader + latch WITHOUT enrolling,
# so the test is fast and deterministic. A persistent interactive session (a FIFO)
# lets the expected latch error on the second activate not end the process.
#
# Run for both orders to prove the latch is symmetric.
set -uo pipefail

EXT="${1:?path to erpl_tunnel.duckdb_extension (MESH_BACKEND=both) required}"
DUCKDB_BIN="${2:-${DUCKDB_BIN:-$(command -v duckdb)}}"
[[ -f "$EXT" ]] || { echo "FAIL: extension not found: $EXT"; exit 2; }

run_order() {
  local first="$1" second="$2"
  local pipe out pid
  pipe="$(mktemp -u)"; mkfifo "$pipe"; out="$(mktemp)"
  exec 8<>"$pipe"
  "$DUCKDB_BIN" -unsigned < "$pipe" > "$out" 2>&1 &
  pid=$!
  cat >&8 <<SQL
LOAD '$EXT';
PRAGMA tunnel_mesh_activate('$first');
SELECT 'AFTER_FIRST' AS marker;
PRAGMA tunnel_mesh_activate('$second');
SELECT 'STILL_ALIVE' AS marker;
SQL
  for _ in $(seq 1 30); do grep -q STILL_ALIVE "$out" && break; sleep 1; done
  kill -9 "$pid" 2>/dev/null; exec 8>&-
  cat "$out"; rm -f "$pipe" "$out"
}

echo "== order 1: Tailscale first, then NetBird =="
o1="$(run_order tailscale netbird)"
if grep -q "Tailscale is active in this DuckDB process; NetBird cannot be loaded" <<<"$o1" \
   && grep -q "STILL_ALIVE" <<<"$o1"; then
  echo "PASS: NetBird refused after Tailscale; process healthy"
else
  echo "FAIL: latch did not refuse NetBird as expected"; echo "$o1"; exit 1
fi

echo "== order 2: NetBird first, then Tailscale =="
o2="$(run_order netbird tailscale)"
if grep -q "NetBird is active in this DuckDB process; Tailscale cannot be loaded" <<<"$o2" \
   && grep -q "STILL_ALIVE" <<<"$o2"; then
  echo "PASS: Tailscale refused after NetBird; process healthy"
else
  echo "FAIL: latch did not refuse Tailscale as expected"; echo "$o2"; exit 1
fi

echo "SINGLE-MESH-LATCH OK — one mesh per process, symmetric, process stays healthy."
