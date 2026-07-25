#!/usr/bin/env bash
# M0 matrix for `tunnel_export` over SSH. NO MOCKS — a real sshd, a real payload.
#
# Answers the one question whose failure mode is silent: which bind-address string do
# we pass to libssh2_channel_forward_listen_ex so that inbound connections actually
# get routed to us, under each of OpenSSH's GatewayPorts modes?
#
# libssh2 matches an inbound forwarded-tcpip open against the listener by comparing
# the bind string byte-for-byte with what sshd echoes. OpenSSH canonicalises that
# address (notably to 127.0.0.1 when GatewayPorts is `no`, the default). On mismatch,
# forward_listen_ex SUCCEEDS and every connection is silently dropped — no error, no
# log, just a tunnel that never works. So we push a real HTTP payload through and see.
#
# Usage: test/integration/ssh_export_matrix.sh
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

# Single instance only. Two concurrent runs reconfigure the same sshd and append to
# the same log, which produced a result table containing the same case twice with
# opposite verdicts — worse than no data, because it looks like data.
exec 9>"${TMPDIR:-/tmp}/erpl_ssh_export_matrix.lock"
flock -n 9 || { echo "FAIL: another ssh_export_matrix run holds the lock"; exit 2; }

SSH_HOST=127.0.0.1 SSH_PORT=2222 SSH_USER=root SSH_PASS=testpass
LOCAL_PORT=8099          # the service we export, running here on the host
REMOTE_PORT_BASE=18099   # each case uses BASE+n so a lingering listener
                         # from the previous case cannot poison the next
PAYLOAD="hello-from-exported-service"

VCPKG_INC="${VCPKG_ROOT:-$HOME/.local/share/vcpkg}/installed/x64-linux/include"
VCPKG_LIB="${VCPKG_ROOT:-$HOME/.local/share/vcpkg}/installed/x64-linux/lib"
SPIKE=build/ssh_export_spike

cleanup() {
  [[ -n "${HTTP_PID:-}" ]] && kill "$HTTP_PID" 2>/dev/null
  rm -rf "${WWW:-}"
  docker compose -f test/integration/docker-compose.yml down -v >/dev/null 2>&1
}
trap cleanup EXIT

echo "== build the spike =="
mkdir -p build
cc -O1 -o "$SPIKE" test/integration/ssh_export_spike.c \
   -I"$VCPKG_INC" "$VCPKG_LIB/libssh2.a" "$VCPKG_LIB/libssl.a" "$VCPKG_LIB/libcrypto.a" \
   -lz -lpthread -ldl || { echo "FAIL: could not build the spike"; exit 2; }

echo "== local service to export =="
# A previous run killed mid-flight leaves its server holding this port while its
# document root is gone, so the new server silently fails to bind and every probe
# 404s against the corpse. Reap it first.
if stale=$(ss -ltnp 2>/dev/null | grep ":$LOCAL_PORT " | grep -oP 'pid=\K[0-9]+' | head -1); then
  [[ -n "$stale" ]] && { echo "  reaping stale listener on $LOCAL_PORT (pid $stale)"; kill -9 "$stale" 2>/dev/null; sleep 1; }
fi
WWW="$(mktemp -d)"; printf '%s' "$PAYLOAD" > "$WWW/probe.txt"
( cd "$WWW" && exec python3 -m http.server "$LOCAL_PORT" --bind 127.0.0.1 >/dev/null 2>&1 ) &
HTTP_PID=$!
sleep 1
got="$(curl -fsS --max-time 5 "http://127.0.0.1:$LOCAL_PORT/probe.txt" 2>/dev/null)"
[[ "$got" == "$PAYLOAD" ]] || {
  echo "FAIL: local service did not start correctly (got '${got:-<nothing>}', want '$PAYLOAD')"; exit 2; }

CASE_N=0
run_case() { # $1=gatewayports  $2=bind-host-arg
  local gp="$1" bind="$2"
  CASE_N=$((CASE_N+1))
  local REMOTE_PORT=$((REMOTE_PORT_BASE + CASE_N))
  # Restart sshd inside the bastion with this GatewayPorts setting.
  docker exec ssh-bastion sh -c "
      pkill sshd 2>/dev/null;
      /usr/sbin/sshd -D -f /etc/ssh/sshd_config -o GatewayPorts=$gp -o AllowTcpForwarding=yes &
      sleep 1" >/dev/null 2>&1
  sleep 2

  # The spike listens; meanwhile, from INSIDE the bastion, curl the forwarded port.
  # Trigger the inbound connection from INSIDE the bastion's network namespace.
  # Not `docker exec ssh-bastion curl` — that image has no curl, and rc=127 looks
  # exactly like "the tunnel didn't route", which silently invalidated every row.
  ( sleep 3
    docker run --rm --network "container:ssh-bastion" curlimages/curl:latest \
      -fsS --max-time 8 "http://127.0.0.1:$REMOTE_PORT/probe.txt" >/dev/null 2>&1
  ) &
  local curl_pid=$!
  local out
  out="$("$SPIKE" "$SSH_HOST" "$SSH_PORT" "$SSH_USER" "$SSH_PASS" \
          "$bind" "$REMOTE_PORT" 127.0.0.1 "$LOCAL_PORT" 2>&1)"
  # Wait for THIS case's curl only. A bare `wait` also waits on the long-lived
  # python http server, which never exits — that hung the entire matrix.
  wait "$curl_pid" 2>/dev/null

  local verdict="FAIL"
  echo "$out" | grep -q SPIKE_OK && verdict="OK"
  echo "$out" | grep -q SPIKE_NO_CONNECTION && verdict="NO-ROUTE(silent)"
  echo "$out" | grep -q SPIKE_LISTEN_FAIL && verdict="LISTEN-REFUSED"
  printf '  %-16s bind=%-12s -> %s\n' "GatewayPorts=$gp" "$bind" "$verdict"
  echo "$out" | sed 's/^/      | /'
}

echo "== bring up the bastion =="
docker compose -f test/integration/docker-compose.yml up -d --wait >/dev/null 2>&1 || {
  echo "FAIL: bastion did not come up"; exit 2; }

echo
echo "== matrix: which bind string actually routes? =="
for gp in no yes clientspecified; do
  for bind in - 127.0.0.1 0.0.0.0 localhost; do
    run_case "$gp" "$bind"
  done
done
echo
echo "Pick the tunnel_export default from the OK rows above (GatewayPorts=no is the"
echo "OpenSSH default, so that block is the one that decides it)."
