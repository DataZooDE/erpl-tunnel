#!/usr/bin/env bash
# Tailscale DATA-PLANE test (Tier 2). NO MOCKS, NO CLOUD.
#
# Proves real bytes cross the tailnet WireGuard data plane between our extension and
# a peer running the OFFICIAL tailscale daemon with a real kernel-TUN interface:
#   node A = our extension's in-process userspace tsnet node, run in a container
#            on the mesh subnet (so it forms a DIRECT WireGuard path to B)
#   node B = tailscale/tailscale daemon (kernel TUN) + an HTTP server on :8000
# A `tunnel_create`s to B's 100.64.x:8000 and fetches a known payload through
# localhost:<port> — transport is WireGuard, not SSH.
#
# Why both nodes are containers on one subnet: Headscale's embedded DERP is plain
# HTTP, which the official daemon refuses for relay (needs TLS). Putting A and B on
# the same docker network (172.28.0.0/16) lets them connect DIRECTLY (mutually
# routable endpoints), so the data path never needs DERP relay. This also avoids the
# host→bridge-IP registration quirk (A reaches control at http://headscale:8080).
#
# Usage: MESH_BACKEND=tailscale|both make release   (node A container needs a
#        no-asan release build); test/integration/mesh_dataplane_tailscale.sh <ext> [duckdb-bin]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HS="$HERE/headscale"
EXT="${1:?path to a RELEASE erpl_tunnel.duckdb_extension (tailscale/both) required}"
DUCKDB_BIN="${2:-${DUCKDB_BIN:-$(command -v duckdb)}}"
BASE_IMAGE="${BASE_IMAGE:-archlinux:latest}"   # glibc must match the extension build
LOCAL_PORT=9500
[[ -f "$EXT" ]] || { echo "FAIL: extension not found: $EXT"; exit 2; }
NET="$(basename "$HS")_mesh"                    # compose network name (dir_mesh)

compose() { docker compose -f "$HS/docker-compose.yml" "$@"; }
hs() { docker exec erpl-headscale headscale "$@"; }
cleanup() {
  docker rm -f erpl-node-a >/dev/null 2>&1 || true
  rm -f "$HS/.env"
  compose --profile peer down -v >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "== 1. Headscale up =="
compose up -d headscale --wait 2>&1 | tail -1 || compose up -d headscale 2>&1 | tail -2
for _ in $(seq 1 30); do hs users list >/dev/null 2>&1 && break; sleep 1; done
hs users list >/dev/null 2>&1 || { echo "FAIL: headscale not ready"; compose logs --tail 20 headscale; exit 1; }

echo "== 2. user + reusable preauth key (shared by A and B) =="
hs users create duckdb >/dev/null 2>&1 || true
UID_NUM="$(hs users list -o json 2>/dev/null | grep -o '"id":[0-9]*' | head -1 | grep -o '[0-9]*')"; UID_NUM="${UID_NUM:-1}"
KEY="$(hs preauthkeys create --user "$UID_NUM" --reusable --expiration 1h 2>/dev/null | tr -d '[:space:]')"
[[ ${#KEY} -ge 20 ]] || { echo "FAIL: could not mint preauth key"; exit 1; }
echo "   key ${KEY:0:12}…"

echo "== 3. node B: official tailscale daemon (kernel TUN) + HTTP service =="
echo "TS_AUTHKEY=$KEY" > "$HS/.env"
compose --profile peer --env-file "$HS/.env" up -d ts-peer ts-peer-svc 2>&1 | tail -2
B_IP=""
for _ in $(seq 1 60); do
  B_IP="$(docker exec erpl-ts-peer tailscale ip -4 2>/dev/null | tr -d '[:space:]')"
  [[ "$B_IP" =~ ^100\. ]] && break; sleep 2
done
[[ "$B_IP" =~ ^100\. ]] || { echo "FAIL: node B has no tailnet IP"; docker logs erpl-ts-peer --tail 30; exit 1; }
echo "   node B mesh IP: $B_IP"

echo "== 4+5. node A container: enroll, tunnel to B:8000, fetch through the tunnel =="
# node A runs in a container on the mesh network (direct WireGuard to B). Its logic
# is a mounted script (avoids nested-quoting hazards); control_url=http://headscale:8080.
set +e
OUT="$(docker run --rm --name erpl-node-a --network "$NET" \
  -v "$EXT":/erpl_tunnel.duckdb_extension:ro \
  -v "$DUCKDB_BIN":/duckdb:ro \
  -v "$HS/nodea_entrypoint.sh":/nodea.sh:ro \
  -e TS_AUTHKEY="$KEY" -e B_IP="$B_IP" \
  "$BASE_IMAGE" bash /nodea.sh 2>&1)"
rc=$?
set -e
echo "$OUT" | grep -vE '^\s*(warning:|::|\s+#)' | tail -8

if echo "$OUT" | grep -q "NODEA_OK hello-from-tailnet-peer"; then
  echo "PASS: fetched the payload from node B THROUGH the Tailscale tunnel (direct WireGuard)"
  echo "TAILSCALE-DATAPLANE OK"
  exit 0
fi
echo "FAIL: no payload over the tunnel (rc=$rc)"
echo "--- node B ---"; docker logs erpl-ts-peer --tail 15 2>&1 | tail -15
exit 1
