#!/usr/bin/env bash
# NetBird DATA-PLANE test (Tier 2). NO MOCKS, NO CLOUD, NO IdP.
#
# Self-hosted NetBird control plane WITHOUT Zitadel: management (IdpManagerConfig:
# none) runs against a store pre-seeded from NetBird's own store.sql fixture (an
# account + a reusable setup key A2C8E62B-38F5-4553-B31E-DD66C696CEBB), plus signal
# and relay. Then:
#   node B = the OFFICIAL netbird daemon (kernel TUN) + an HTTP server on :8000
#   node A = our extension's client/embed node, in a container on the same subnet
# A tunnel_creates to B's NetBird IP:8000 and fetches a payload through the tunnel —
# bytes cross the NetBird overlay (direct WireGuard, same-subnet trick).
#
# Usage: MESH_BACKEND=netbird|both make release
#        test/integration/mesh_dataplane_netbird.sh <ext> [duckdb-bin]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NB="$HERE/netbird"
EXT="${1:?path to a RELEASE erpl_tunnel.duckdb_extension (netbird/both) required}"
DUCKDB_BIN="${2:-${DUCKDB_BIN:-$(command -v duckdb)}}"
BASE_IMAGE="${BASE_IMAGE:-archlinux:latest}"
SETUP_KEY="A2C8E62B-38F5-4553-B31E-DD66C696CEBB"   # from NetBird's store.sql fixture
NET="netbird_mesh"
[[ -f "$EXT" ]] || { echo "FAIL: extension not found: $EXT"; exit 2; }

compose() { docker compose -f "$NB/docker-compose.yml" "$@"; }
cleanup() {
  docker rm -f erpl-node-a >/dev/null 2>&1 || true
  rm -f "$NB/.env"
  compose --profile peer down -v >/dev/null 2>&1 || true
  rm -rf "$NB/mgmt-data"
}
trap cleanup EXIT

echo "== 1. seed the management store (no IdP) =="
if [[ ! -x "$NB/seeder/seeder" ]]; then
  echo "FAIL: build the seeder first (cd $NB/seeder && go build -o seeder .)"; exit 2
fi
rm -rf "$NB/mgmt-data"; mkdir -p "$NB/mgmt-data"
"$NB/seeder/seeder" "$NB/seeder/store.sql" "$NB/mgmt-data" >/dev/null 2>&1
cp "$NB/management.json" "$NB/mgmt-data/management.json"
[[ -f "$NB/mgmt-data/store.db" ]] || { echo "FAIL: seeding produced no store.db"; exit 1; }

echo "== 2. control plane up (management + signal + relay) =="
compose up -d signal relay management 2>&1 | tail -3
for _ in $(seq 1 30); do
  code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 http://127.0.0.1:33073/ 2>/dev/null || true)"
  [[ -n "$code" && "$code" != "000" ]] && break; sleep 1
done

echo "== 3. node B: official netbird daemon (kernel TUN) + HTTP service =="
echo "NB_SETUP_KEY=$SETUP_KEY" > "$NB/.env"
compose --profile peer --env-file "$NB/.env" up -d nb-peer nb-peer-svc 2>&1 | tail -2
B_IP=""
for _ in $(seq 1 50); do
  B_IP="$(docker exec erpl-nb-peer netbird status 2>/dev/null | grep -oE 'NetBird IP: 100\.[0-9]+\.[0-9]+\.[0-9]+' | grep -oE '100\.[0-9.]+' | head -1)"
  [[ -n "$B_IP" ]] && break; sleep 3
done
[[ -n "$B_IP" ]] || { echo "FAIL: node B has no NetBird IP"; docker logs erpl-nb-peer --tail 30; exit 1; }
echo "   node B NetBird IP: $B_IP"

echo "== 4+5. node A container: enroll, tunnel to B:8000, fetch through the tunnel =="
set +e
OUT="$(docker run --rm --name erpl-node-a --network "$NET" \
  --cap-add NET_ADMIN --device /dev/net/tun \
  -v "$EXT":/erpl_tunnel.duckdb_extension:ro \
  -v "$DUCKDB_BIN":/duckdb:ro \
  -v "$NB/nodea_entrypoint.sh":/nodea.sh:ro \
  -e NB_SETUP_KEY="$SETUP_KEY" -e B_IP="$B_IP" \
  -e NB_FORCE_USERSPACE_FIREWALL=true \
  "$BASE_IMAGE" bash /nodea.sh 2>&1)"
rc=$?
set -e
echo "$OUT" | grep -vE '^\s*(warning:|::|\s+#)' | tail -8

if echo "$OUT" | grep -q "NODEA_OK hello-from-tailnet-peer"; then
  echo "PASS: fetched the payload from node B THROUGH the NetBird tunnel (direct WireGuard)"
  echo "NETBIRD-DATAPLANE OK"
  exit 0
fi
echo "FAIL: no payload over the tunnel (rc=$rc)"
echo "--- node B ---"; docker logs erpl-nb-peer --tail 15 2>&1 | tail -15
exit 1
