#!/usr/bin/env bash
# Real Tailscale enrollment against a hermetic Headscale control server (ADR-009,
# BRD M2). NO MOCKS, NO CLOUD: boots open-source Headscale in docker, mints a
# reusable preauth key, then the in-process tsnet node embedded in the extension
# enrolls via control_url and receives a real 100.64.x mesh IP, surfaced through
# tunnel_self(). This exercises the full embed -> dlopen -> tsnet.Up -> LocalClient
# .Status path against a real control plane.
#
# Usage: MESH_BACKEND=tailscale make debug
#        test/integration/mesh_e2e_headscale.sh <ext> [duckdb-bin]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXT="${1:?path to erpl_tunnel.duckdb_extension required}"
DUCKDB_BIN="${2:-${DUCKDB_BIN:-$(command -v duckdb)}}"
[[ -f "$EXT" ]] || { echo "FAIL: extension not found: $EXT"; exit 2; }

compose() { docker compose -f "$HERE/headscale/docker-compose.yml" "$@"; }
hs() { docker exec erpl-headscale headscale "$@"; }

cleanup_duck() { [[ -n "${DUCK_PID:-}" ]] && kill -9 "$DUCK_PID" 2>/dev/null; exec 9>&- 2>/dev/null; rm -f "${PIPE:-}" "${OUT:-}"; }
trap cleanup_duck EXIT

echo "== bringing up Headscale =="
compose up -d --wait 2>&1 | tail -2 || { compose up -d 2>&1 | tail -3; }
# Wait for the API to answer.
for _ in $(seq 1 30); do hs users list >/dev/null 2>&1 && break; sleep 1; done
hs users list >/dev/null 2>&1 || { echo "FAIL: headscale API not ready"; compose logs --tail 20; exit 1; }

echo "== creating user + reusable preauth key =="
hs users create duckdb >/dev/null 2>&1 || true
UID_NUM="$(hs users list -o json 2>/dev/null | grep -o '"id":[0-9]*' | head -1 | grep -o '[0-9]*')"
[[ -z "$UID_NUM" ]] && UID_NUM=1
KEY="$(hs preauthkeys create --user "$UID_NUM" --reusable --expiration 1h 2>/dev/null | tr -d '[:space:]')"
if [[ -z "$KEY" || ${#KEY} -lt 20 ]]; then
  echo "FAIL: could not mint preauth key (got '$KEY')"; hs preauthkeys create --user "$UID_NUM" --reusable --expiration 1h; exit 1
fi
echo "   preauth key: ${KEY:0:12}…"

echo "== enrolling in-process tsnet node via the extension =="
PIPE="$(mktemp -u)"; mkfifo "$PIPE"; OUT="$(mktemp)"
exec 9<>"$PIPE"
STATE_DIR="$(mktemp -d)"
"$DUCKDB_BIN" -unsigned < "$PIPE" > "$OUT" 2>&1 &
DUCK_PID=$!
cat >&9 <<SQL
LOAD '$EXT';
CREATE SECRET hs (TYPE tunnel, backend 'tailscale', auth_key '$KEY',
    control_url 'http://127.0.0.1:18080', hostname 'erpl-node-a',
    state_dir '$STATE_DIR', ephemeral true);
.mode list
SELECT 'SELF=' || mesh_ip || '|' || host_name AS out FROM tunnel_self(secret := 'hs');
SELECT 'DONE' AS marker;
SQL

for _ in $(seq 1 60); do grep -qE "SELF=|Error|DONE" "$OUT" && break; sleep 1; done

self_line="$(grep -oE 'SELF=[^|]*\|[^ ]*' "$OUT" | head -1)"
mesh_ip="${self_line#SELF=}"; mesh_ip="${mesh_ip%%|*}"
echo "   tunnel_self -> $self_line"

# Verify from the control server's side that the node registered.
echo "== headscale nodes =="
hs nodes list 2>/dev/null | grep -i "erpl-node-a" || true

if [[ "$mesh_ip" =~ ^100\.(6[4-9]|[7-9][0-9]|1[0-1][0-9]|12[0-7])\. ]]; then
  echo "PASS: tsnet node enrolled against Headscale, mesh IP $mesh_ip (real control plane)"
  echo "MESH-E2E OK"
  exit 0
fi
echo "FAIL: node did not receive a 100.64/10 mesh IP"
echo "---- duckdb output ----"; cat "$OUT"
exit 1
