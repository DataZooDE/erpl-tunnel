#!/usr/bin/env bash
# Runs INSIDE node A's container (on the mesh network). Enrolls our extension's
# tsnet node, opens a tunnel to node B's HTTP service, and fetches a payload through
# it — all in one netns so curl reaches the tunnel's loopback listener.
# Env: TS_AUTHKEY, B_IP. Mounts: /duckdb, /erpl_tunnel.duckdb_extension.
set -e
# Ensure curl exists, whatever the base image's package manager is.
if ! command -v curl >/dev/null 2>&1; then
  if command -v apt-get >/dev/null 2>&1; then
    apt-get update -qq && apt-get install -y -qq curl >/dev/null 2>&1 || true
  elif command -v pacman >/dev/null 2>&1; then
    pacman -Sy --noconfirm --quiet curl >/dev/null 2>&1 || true
  fi
fi

PIPE=$(mktemp -u); mkfifo "$PIPE"; exec 9<>"$PIPE"
/duckdb -unsigned < "$PIPE" > /tmp/a.out 2>&1 &

cat >&9 <<SQL
LOAD '/erpl_tunnel.duckdb_extension';
CREATE SECRET a (TYPE tunnel, backend 'tailscale', auth_key '${TS_AUTHKEY}',
    control_url 'http://headscale:8080', hostname 'erpl-node-a',
    state_dir '/tmp/tsstate', ephemeral true);
PRAGMA tunnel_create(secret='a', remote_host='${B_IP}', remote_port='8000',
    local_port='9500', timeout='90');
SELECT 'TUNNEL_UP' AS m;
SQL

for _ in $(seq 1 90); do grep -qE "TUNNEL_UP|Error" /tmp/a.out && break; sleep 1; done
grep -q TUNNEL_UP /tmp/a.out || { echo "NODEA_FAIL tunnel"; cat /tmp/a.out; exit 1; }

got=""
for _ in $(seq 1 45); do
  got="$(curl -s --max-time 4 http://127.0.0.1:9500/hello.txt || true)"
  [ "$got" = "hello-from-tailnet-peer" ] && break
  sleep 2
done
if [ "$got" = "hello-from-tailnet-peer" ]; then
  echo "NODEA_OK $got"
else
  echo "NODEA_FAIL payload=$got"; tail -8 /tmp/a.out; exit 1
fi
