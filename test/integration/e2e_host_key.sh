#!/usr/bin/env bash
# Host-key verification against a real sshd — NO MOCKS (issue #16).
#
# Proves, against the dockerised bastion, what the sqllogictest cannot see:
#   1. The fingerprint the extension reports is byte-identical to what
#      `ssh-keyscan | ssh-keygen -lf -` prints for the same server.
#   2. A WRONG host_key_fingerprint fails BEFORE authentication: sshd's own log
#      records no password and no publickey attempt for that connection, even
#      though the credentials are valid (they succeed in step 4).
#   3. Without a fingerprint the tunnel connects and warns, naming the real key.
#   4. With the right fingerprint it connects and authenticates, with no warning.
#
# The bastion logs to /var/log/sshd.log (sshd -E, see docker-compose.yml).
#
# Usage:
#   make test_up
#   test/integration/e2e_host_key.sh [path-to-erpl_tunnel.duckdb_extension]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXT="${1:-$(ls "$REPO_ROOT"/build/release/repository/*/*/erpl_tunnel.duckdb_extension 2>/dev/null | head -1)}"
DUCKDB_BIN="${DUCKDB_BIN:-$(command -v duckdb || echo "$REPO_ROOT/build/release/duckdb")}"
SSH_HOST="${ERPL_SSH_HOST:-127.0.0.1}"
SSH_PORT="${ERPL_SSH_PORT:-2222}"
SSH_USER="${ERPL_SSH_USER:-root}"
SSH_PASSWORD="${ERPL_SSH_PASSWORD:-testpass}"
LOCAL_PORT="${LOCAL_PORT:-9082}"
SSH_CONTAINER="${SSH_CONTAINER:-}"
SSHD_LOG=/var/log/sshd.log
# A well-formed fingerprint that is not the bastion's.
WRONG_FP='SHA256:slLJ2h9qY3fXo2afsGBLpeakUA6ZtMz6NRaeNJBNiwA'

fail() { echo "FAIL: $*" >&2; exit 1; }

[[ -f "$EXT" ]] || { echo "FAIL: extension not found: $EXT" >&2; exit 2; }
if [[ -z "$SSH_CONTAINER" ]]; then
  SSH_CONTAINER="$(docker ps --filter "publish=$SSH_PORT" --format '{{.Names}}' | head -1)"
fi
[[ -n "$SSH_CONTAINER" ]] || { echo "FAIL: no container publishes port $SSH_PORT — run 'make test_up'" >&2; exit 2; }
docker exec "$SSH_CONTAINER" test -f "$SSHD_LOG" \
  || { echo "FAIL: $SSH_CONTAINER has no $SSHD_LOG (sshd not started with -E?)" >&2; exit 2; }

# Independent reference: what OpenSSH itself says the server's key is.
REAL_FP="$(ssh-keyscan -p "$SSH_PORT" "$SSH_HOST" 2>/dev/null | ssh-keygen -lf - | awk '{print $2}' | head -1)"
[[ "$REAL_FP" == SHA256:* ]] || fail "could not obtain the bastion's fingerprint with ssh-keyscan"
[[ "$REAL_FP" != "$WRONG_FP" ]] || fail "the 'wrong' fingerprint is the real one; pick another"
echo "bastion host key (ssh-keygen): $REAL_FP"

auth_lines() {
  # Every authentication attempt sshd logs, successful or not.
  docker exec "$SSH_CONTAINER" sh -c "grep -cE '(Accepted|Failed) (password|publickey)|authentication failure' $SSHD_LOG || true"
}
preauth_lines() {
  # Connections that ended before authentication — proves the client got that far.
  docker exec "$SSH_CONTAINER" sh -c "grep -cE '\\[preauth\\]' $SSHD_LOG || true"
}

run_sql() { # $1 = fingerprint clause (may be empty)
  # -csv: the box renderer would truncate the long message column. AI_AGENT
  # switches DuckDB 2.0's CLI to JSON "agent mode"; keep the plain output.
  env -u AI_AGENT "$DUCKDB_BIN" -unsigned -csv -c "LOAD '$EXT';
CREATE SECRET bastion (TYPE ssh_tunnel, host '$SSH_HOST', port '$SSH_PORT',
    user '$SSH_USER', password '$SSH_PASSWORD' $1);
PRAGMA tunnel_import(secret='bastion', remote_host='http-service', remote_port=8000,
    local_port=$LOCAL_PORT, timeout=30);" 2>&1
}

# --- 2. wrong fingerprint: refused, and sshd saw no authentication -----------
before_auth="$(auth_lines)"; before_pre="$(preauth_lines)"
set +e; out="$(run_sql ", host_key_fingerprint '$WRONG_FP'")"; rc=$?; set -e
echo "$out" | sed 's/^/  wrong> /'
[[ $rc -ne 0 ]] || fail "a wrong host_key_fingerprint still connected"
grep -q "host key verification FAILED" <<<"$out" || fail "no host-key error for a wrong fingerprint"
grep -qF "expected $WRONG_FP, but the server presented $REAL_FP" <<<"$out" \
  || fail "the error does not name both the expected and the actual fingerprint"
sleep 1
after_auth="$(auth_lines)"; after_pre="$(preauth_lines)"
[[ "$after_auth" == "$before_auth" ]] \
  || fail "sshd logged an authentication attempt ($before_auth -> $after_auth) despite the wrong host key"
[[ "$after_pre" -gt "$before_pre" ]] \
  || fail "sshd logged no pre-auth disconnect; the test did not reach the server ($before_pre -> $after_pre)"
echo "ok: wrong fingerprint refused before authentication (auth attempts stayed at $after_auth)"

# --- 3. no fingerprint: connects, warns with the real fingerprint ------------
out="$(run_sql "")" || fail "an unpinned tunnel did not connect: $out"
echo "$out" | sed 's/^/  none> /'
grep -qF "was NOT verified" <<<"$out" || fail "no warning for an unpinned host key"
grep -qF "The server presented $REAL_FP" <<<"$out" || fail "the warning does not carry the real fingerprint"
grep -qF "host_key_fingerprint '$REAL_FP'" <<<"$out" || fail "the warning does not say how to pin it"
echo "ok: unpinned tunnel connected and warned with $REAL_FP"

# --- 4. right fingerprint: connects and authenticates, no warning -------------
before_auth="$(auth_lines)"
out="$(run_sql ", host_key_fingerprint '$REAL_FP'")" || fail "the right fingerprint did not connect: $out"
echo "$out" | sed 's/^/  right> /'
grep -q "Tunnel created successfully" <<<"$out" || fail "no tunnel with the right fingerprint"
! grep -q "WARNING" <<<"$out" || fail "a verified host key still produced a warning"
sleep 1
[[ "$(auth_lines)" -gt "$before_auth" ]] || fail "sshd logged no authentication for the verified connection"
echo "ok: pinned tunnel verified the host key and authenticated"

echo "PASS: host-key verification (issue #16)"
