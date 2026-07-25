#!/usr/bin/env bash
# Real end-to-end export test — NO MOCKS. The mirror image of
# e2e_http_over_tunnel.sh: instead of pulling a remote service to a local port,
# it publishes a LOCAL service onto the SSH server and fetches it from there.
#
#   host: python http_server.py on $LOCAL_PORT
#     -> PRAGMA tunnel_export(secret='bastion', local_port=$LOCAL_PORT, remote_port=$REMOTE_PORT)
#        -> sshd binds $REMOTE_PORT inside the container
#           -> curl (run INSIDE the container) hits 127.0.0.1:$REMOTE_PORT
#
# The curl runs inside the bastion, so a pass means bytes traversed
# forwarded-tcpip channels in the direction only an export can produce. M0
# (ssh_export_matrix.sh) established that every GatewayPorts x bind-address
# combination routes, so this uses the default bind (remote_host omitted).
#
# Usage:
#   make test_up
#   test/integration/e2e_ssh_export.sh [path-to-erpl_tunnel.duckdb_extension]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXT="${1:-$(ls "$REPO_ROOT"/build/release/repository/*/*/erpl_tunnel.duckdb_extension 2>/dev/null | head -1)}"
DUCKDB_BIN="${DUCKDB_BIN:-$(command -v duckdb || echo "$REPO_ROOT/build/release/duckdb")}"
SSH_HOST="${ERPL_SSH_HOST:-127.0.0.1}"
SSH_PORT="${ERPL_SSH_PORT:-2222}"
SSH_USER="${ERPL_SSH_USER:-root}"
SSH_PASSWORD="${ERPL_SSH_PASSWORD:-testpass}"
LOCAL_PORT="${LOCAL_PORT:-9081}"
REMOTE_PORT="${REMOTE_PORT:-18081}"
# The compose service name of the sshd container we exec curl inside.
SSH_CONTAINER="${SSH_CONTAINER:-}"

if [[ ! -f "$EXT" ]]; then echo "FAIL: extension not found: $EXT" >&2; exit 2; fi

# Resolve the sshd container. Hardcoding a name breaks whenever compose changes
# its project prefix, so find it by the published SSH port instead.
if [[ -z "$SSH_CONTAINER" ]]; then
  SSH_CONTAINER="$(docker ps --filter "publish=$SSH_PORT" --format '{{.Names}}' | head -1)"
fi
if [[ -z "$SSH_CONTAINER" ]]; then
  echo "FAIL: no running container publishes port $SSH_PORT — run 'make test_up' first" >&2
  exit 2
fi

# The bastion image ships without curl (an M0 lesson: its absence made every
# case look like a routing failure, rc=127 read as 'no route'). Verify a fetch
# tool exists BEFORE the test, so a missing tool fails loudly as setup, never
# silently as "the export did not work".
FETCH=""
if docker exec "$SSH_CONTAINER" sh -c 'command -v curl' >/dev/null 2>&1; then
  FETCH="curl -s --max-time 5"
elif docker exec "$SSH_CONTAINER" sh -c 'command -v wget' >/dev/null 2>&1; then
  FETCH="wget -q -O - -T 5"
else
  echo "FAIL: neither curl nor wget inside $SSH_CONTAINER — cannot verify the export" >&2
  exit 2
fi

PIPE="$(mktemp -u)"; mkfifo "$PIPE"
OUT="$(mktemp)"
DUCK_PID=""; HTTP_PID=""
cleanup() {
  [[ -n "$DUCK_PID" ]] && kill -9 "$DUCK_PID" 2>/dev/null || true
  [[ -n "$HTTP_PID" ]] && kill -9 "$HTTP_PID" 2>/dev/null || true
  exec 9>&- 2>/dev/null || true
  rm -f "$PIPE" "$OUT"
}
trap cleanup EXIT

# The local service to publish — the same server the import test consumes.
python3 "$REPO_ROOT/test/integration/http_server.py" "$LOCAL_PORT" >/dev/null 2>&1 &
HTTP_PID=$!
for _ in $(seq 1 20); do
  curl -s --max-time 1 "http://127.0.0.1:$LOCAL_PORT/" >/dev/null 2>&1 && break
  sleep 0.5
done
curl -s --max-time 2 "http://127.0.0.1:$LOCAL_PORT/" >/dev/null || {
  echo "FAIL: local http server never came up on $LOCAL_PORT"; exit 1; }

exec 9<>"$PIPE"
"$DUCKDB_BIN" -unsigned < "$PIPE" > "$OUT" 2>&1 &
DUCK_PID=$!

cat >&9 <<SQL
LOAD '$EXT';
CREATE SECRET bastion (TYPE ssh_tunnel, ssh_host '$SSH_HOST', ssh_port '$SSH_PORT',
    ssh_user '$SSH_USER', password '$SSH_PASSWORD', auth_method 'password');
PRAGMA tunnel_export(secret='bastion', local_port='$LOCAL_PORT', remote_port='$REMOTE_PORT', timeout='30');
SELECT 'EXPORT_READY' AS marker;
SQL

for _ in $(seq 1 20); do
  grep -q EXPORT_READY "$OUT" 2>/dev/null && break
  sleep 0.5
done
grep -q EXPORT_READY "$OUT" || { echo "FAIL: export did not become ready"; cat "$OUT"; exit 1; }

fail=0
root_resp="$(docker exec "$SSH_CONTAINER" sh -c "$FETCH http://127.0.0.1:$REMOTE_PORT/" 2>/dev/null || true)"
if [[ "$root_resp" == "Hello from service" ]]; then
  echo "PASS: bastion fetched the exported service: '$root_resp'"
else
  echo "FAIL: unexpected response from the exported service: '$root_resp'"; fail=1
fi

csv_first="$(docker exec "$SSH_CONTAINER" sh -c "$FETCH http://127.0.0.1:$REMOTE_PORT/data.csv" 2>/dev/null | sed -n '2p' || true)"
if [[ "$csv_first" == "1,Alice,New York,30" ]]; then
  echo "PASS: CSV payload over the export, first row: '$csv_first'"
else
  echo "FAIL: unexpected CSV first row over the export: '$csv_first'"; fail=1
fi

# A second request must also work: it opens a NEW forwarded-tcpip channel, so
# this is what catches an accept loop that only ever serves one connection.
second="$(docker exec "$SSH_CONTAINER" sh -c "$FETCH http://127.0.0.1:$REMOTE_PORT/" 2>/dev/null || true)"
if [[ "$second" == "Hello from service" ]]; then
  echo "PASS: second connection also served (accept loop is not one-shot)"
else
  echo "FAIL: second connection failed: '$second'"; fail=1
fi

# The export must survive teardown cleanly — tunnels() should still list it, and
# closing must not hang (the join-before-free ordering in SshExporter::Close).
cat >&9 <<'SQL'
SELECT 'DIRECTION=' || direction AS d FROM tunnels() WHERE direction = 'export';
SQL
sleep 2
if grep -q "DIRECTION=export" "$OUT"; then
  echo "PASS: tunnels() reports the export"
else
  echo "FAIL: tunnels() did not report the export"; cat "$OUT"; fail=1
fi

if [[ $fail -eq 0 ]]; then echo "E2E OK — real HTTP payload served FROM DuckDB over an SSH remote-forward"; fi
exit $fail
