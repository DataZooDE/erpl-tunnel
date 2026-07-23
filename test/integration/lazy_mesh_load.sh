#!/usr/bin/env bash
# Lazy-dlopen + zero-Go proof (HLD §6.5, §8.8, ADR-011; BRD M2/M5). NO MOCKS.
#
# 1. LOAD the (tailscale-bundled) extension and assert NO Go/tailscale mapping is
#    present in the process — the mesh shim is embedded but not yet dlopen'd.
# 2. Trigger a mesh activation (tunnel_self against a tailscale secret) and assert a
#    shim mapping now appears — the blob was extracted + dlopen'd on first use.
#
# The activation deliberately uses a bogus control URL so it fails fast without any
# network; failure is fine — we are proving the *loader* ran, not that enrollment
# succeeded. We inspect /proc/<pid>/maps of the live duckdb process.
#
# Usage: test/integration/lazy_mesh_load.sh <ext.duckdb_extension> [duckdb-bin]
set -uo pipefail

EXT="${1:?path to erpl_tunnel.duckdb_extension required}"
DUCKDB_BIN="${2:-${DUCKDB_BIN:-$(command -v duckdb)}}"
[[ -f "$EXT" ]] || { echo "FAIL: extension not found: $EXT"; exit 2; }

PIPE="$(mktemp -u)"; mkfifo "$PIPE"
OUT="$(mktemp)"
DUCK_PID=""
cleanup(){ [[ -n "$DUCK_PID" ]] && kill -9 "$DUCK_PID" 2>/dev/null; exec 9>&- 2>/dev/null; rm -f "$PIPE" "$OUT"; }
trap cleanup EXIT

exec 9<>"$PIPE"
"$DUCKDB_BIN" -unsigned < "$PIPE" > "$OUT" 2>&1 &
DUCK_PID=$!

# Step 1: load only.
cat >&9 <<SQL
LOAD '$EXT';
SELECT 'LOADED' AS marker;
SQL
for _ in $(seq 1 20); do grep -q LOADED "$OUT" && break; sleep 0.3; done
grep -q LOADED "$OUT" || { echo "FAIL: extension did not load"; cat "$OUT"; exit 1; }

maps_have_go() {
  # A dlopen'd Go shim shows up as a mapping of our extracted temp file.
  grep -Eic "erpl_tunnel_mesh|ts_shim|nb_shim" "/proc/$DUCK_PID/maps" 2>/dev/null
}

before="$(maps_have_go)"
if [[ "$before" -eq 0 ]]; then
  echo "PASS: SSH-path load maps NO mesh shim (Go not loaded) — lazy, zero-Go baseline"
else
  echo "FAIL: a mesh shim is mapped at LOAD time ($before regions) — not lazy"; exit 1
fi

# Step 2: trigger activation. Bogus control_url => fails fast, but the loader runs.
cat >&9 <<SQL
CREATE SECRET tsx (TYPE tunnel, backend 'tailscale', auth_key 'tskey-bogus',
    control_url 'http://127.0.0.1:1', hostname 'lazyprobe', ephemeral true);
SELECT * FROM tunnel_self(secret := 'tsx');
SELECT 'ACTIVATED' AS marker;
SQL
for _ in $(seq 1 40); do grep -qE "ACTIVATED|Error" "$OUT" && break; sleep 0.5; done

after="$(maps_have_go)"
if [[ "$after" -ge 1 ]]; then
  echo "PASS: after first mesh use, the shim is dlopen'd and mapped ($after regions)"
else
  echo "FAIL: mesh shim never mapped after activation"; cat "$OUT"; exit 1
fi

echo "LAZY-LOAD OK — mesh Go runtime is loaded on demand, not at extension load."
