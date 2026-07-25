#!/usr/bin/env bash
# Real end-to-end payload test — NO MOCKS.
#
# Forwards a local port through the dockerised SSH bastion to the private HTTP
# service (reachable only inside the docker network as http-service:8000) and
# fetches a real HTTP response + CSV over the forwarded port with curl. This
# exercises the full accept-loop + libssh2 direct-tcpip byte pump against a real
# SSH server and a real HTTP server.
#
# Usage:
#   make test_up                       # bring up docker sshd + http-service
#   test/integration/e2e_http_over_tunnel.sh [path-to-erpl_tunnel.duckdb_extension]
#
# Exits non-zero on any mismatch. Self-contained cleanup (no lingering sessions).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Glob rather than hardcode a version/platform directory: DUCKDB_PLATFORM varies
# by host and the DuckDB version moves. The Makefile passes an explicit path.
EXT="${1:-$(ls "$REPO_ROOT"/build/release/repository/*/*/erpl_tunnel.duckdb_extension 2>/dev/null | head -1)}"
DUCKDB_BIN="${DUCKDB_BIN:-$(command -v duckdb || echo "$REPO_ROOT/build/release/duckdb")}"
SSH_HOST="${ERPL_SSH_HOST:-127.0.0.1}"
SSH_PORT="${ERPL_SSH_PORT:-2222}"
SSH_USER="${ERPL_SSH_USER:-root}"
SSH_PASSWORD="${ERPL_SSH_PASSWORD:-testpass}"
LOCAL_PORT="${LOCAL_PORT:-9080}"

if [[ ! -f "$EXT" ]]; then echo "FAIL: extension not found: $EXT" >&2; exit 2; fi

PIPE="$(mktemp -u)"; mkfifo "$PIPE"
OUT="$(mktemp)"
DUCK_PID=""
cleanup() {
  [[ -n "$DUCK_PID" ]] && kill -9 "$DUCK_PID" 2>/dev/null || true
  exec 9>&- 2>/dev/null || true
  rm -f "$PIPE" "$OUT"
}
trap cleanup EXIT

# Hold the FIFO open on fd 9 so the duckdb reader never sees EOF until we choose.
exec 9<>"$PIPE"
"$DUCKDB_BIN" -unsigned < "$PIPE" > "$OUT" 2>&1 &
DUCK_PID=$!

cat >&9 <<SQL
LOAD '$EXT';
CREATE SECRET bastion (TYPE ssh_tunnel, ssh_host '$SSH_HOST', ssh_port '$SSH_PORT',
    ssh_user '$SSH_USER', password '$SSH_PASSWORD', auth_method 'password');
PRAGMA tunnel_create(secret='bastion', remote_host='http-service', remote_port='8000',
    local_port='$LOCAL_PORT', timeout='30');
SELECT 'TUNNEL_READY' AS marker;
SQL

# Wait for the tunnel to come up (bounded).
for _ in $(seq 1 20); do
  grep -q TUNNEL_READY "$OUT" 2>/dev/null && break
  sleep 0.5
done
grep -q TUNNEL_READY "$OUT" || { echo "FAIL: tunnel did not become ready"; cat "$OUT"; exit 1; }

fail=0
root_resp="$(curl -s --max-time 5 "http://localhost:$LOCAL_PORT/" || true)"
if [[ "$root_resp" == "Hello from service" ]]; then
  echo "PASS: root response over tunnel: '$root_resp'"
else
  echo "FAIL: unexpected root response: '$root_resp'"; fail=1
fi

csv_first="$(curl -s --max-time 5 "http://localhost:$LOCAL_PORT/data.csv" | sed -n '2p' || true)"
if [[ "$csv_first" == "1,Alice,New York,30" ]]; then
  echo "PASS: CSV payload over tunnel first row: '$csv_first'"
else
  echo "FAIL: unexpected CSV first row: '$csv_first'"; fail=1
fi

if [[ $fail -eq 0 ]]; then echo "E2E OK — real HTTP payload forwarded over SSH tunnel"; fi
exit $fail
