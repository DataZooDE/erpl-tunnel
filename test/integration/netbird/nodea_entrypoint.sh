#!/usr/bin/env bash
# Runs INSIDE node A's container (on the mesh network). Enrolls our extension's
# NetBird client/embed node, opens a tunnel to node B's HTTP service, and fetches a
# payload through it. Env: NB_SETUP_KEY, B_IP. Mounts: /duckdb, /erpl_tunnel.duckdb_extension.
set -e
if ! command -v curl >/dev/null 2>&1; then
  if command -v apt-get >/dev/null 2>&1; then apt-get update -qq && apt-get install -y -qq curl >/dev/null 2>&1 || true
  elif command -v pacman >/dev/null 2>&1; then pacman -Sy --noconfirm --quiet curl >/dev/null 2>&1 || true; fi
fi

PIPE=$(mktemp -u); mkfifo "$PIPE"; exec 9<>"$PIPE"
/duckdb -unsigned < "$PIPE" > /tmp/a.out 2>&1 &

cat >&9 <<SQL
LOAD '/erpl_tunnel.duckdb_extension';
CREATE SECRET a (TYPE tunnel, backend 'netbird', setup_key '${NB_SETUP_KEY}',
    management_url 'http://management:80', hostname 'erpl-node-a',
    state_dir '/tmp/nbstate', ephemeral true);
PRAGMA tunnel_create(secret='a', remote_host='${B_IP}', remote_port='8000',
    local_port='9500', timeout='120');
SELECT 'TUNNEL_UP' AS m;
SQL

for _ in $(seq 1 120); do grep -qE "TUNNEL_UP|Error" /tmp/a.out && break; sleep 1; done
if ! grep -q TUNNEL_UP /tmp/a.out; then echo "NODEA_FAIL tunnel_create did not complete"; tail -20 /tmp/a.out; exit 1; fi
echo "NODEA_TUNNEL_UP ok"

# NetBird uses lazy peer connections: the path to B forms on first traffic and can
# take a little to settle. Give it a generous, retrying window.
got=""
for i in $(seq 1 60); do
  got="$(curl -s --max-time 5 http://127.0.0.1:9500/hello.txt || true)"
  [ "$got" = "hello-from-tailnet-peer" ] && break
  [ $((i % 10)) -eq 0 ] && echo "  curl attempt $i: got='${got}'"
  sleep 2
done
if [ "$got" = "hello-from-tailnet-peer" ]; then
  echo "NODEA_OK $got"
else
  echo "NODEA_FAIL payload=$got"
  echo "---- node A duckdb/embed log ----"; tail -30 /tmp/a.out
  exit 1
fi
