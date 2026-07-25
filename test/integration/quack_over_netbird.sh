#!/usr/bin/env bash
# The tunnel_export acceptance test on NETBIRD. NO MOCKS, NO CLOUD, NO IdP.
#
# The NetBird sibling of quack_over_mesh.sh, and the reason it exists separately:
# the Tailscale run proves the export path, but not NetBird's. They share
# meshpair.Exporter and differ in which listener Go gets (client.ListenTCP vs
# tsnet.Server.Listen) — and NetBird adds something Tailscale's run never
# exercises at all: inbound access policy. NetBird is default-deny in BOTH
# directions, so an export can be perfectly functional and still unreachable.
#
#   node A = our extension's client/embed node. Serves quack on 127.0.0.1:9494 and
#            publishes it with PRAGMA tunnel_export.
#   node B = a plain DuckDB inside the netns of the OFFICIAL netbird daemon
#            (kernel TUN), which ATTACHes to node A's 100.x:9494 and queries it.
#
# Both nodes force NetBird's userspace firewall: the iptables+ipset backend is
# default-drop and silently applies ZERO rules when the host kernel lacks
# ip_set_hash_net, which looks exactly like a broken data plane.
#
# Usage: MESH_BACKEND=netbird|both make release
#        test/integration/quack_over_netbird.sh <ext> [duckdb-bin]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NB="$HERE/netbird"
EXT="${1:?path to a RELEASE erpl_tunnel.duckdb_extension (netbird/both) required}"
DUCKDB_BIN="${2:-${DUCKDB_BIN:-$(command -v duckdb)}}"
BASE_IMAGE="${BASE_IMAGE:-archlinux:latest}"
SETUP_KEY="A2C8E62B-38F5-4553-B31E-DD66C696CEBB"   # from NetBird's store.sql fixture
QUACK_TOKEN="erpl-tunnel-acceptance-token"
NET="netbird_mesh"
[[ -f "$EXT" ]] || { echo "FAIL: extension not found: $EXT"; exit 2; }
[[ -x "$DUCKDB_BIN" ]] || { echo "FAIL: no duckdb binary: $DUCKDB_BIN"; exit 2; }
# docker -v REQUIRES absolute paths; a relative one is read as a named volume.
EXT="$(realpath "$EXT")"
DUCKDB_BIN="$(realpath "$DUCKDB_BIN")"

EXTS="$(mktemp -d)"
compose() { docker compose -f "$NB/docker-compose.yml" "$@"; }
cleanup() {
  docker rm -f erpl-node-a >/dev/null 2>&1 || true
  rm -rf "$EXTS"; rm -f "$NB/.env"
  compose --profile peer down -v >/dev/null 2>&1 || true
  rm -rf "$NB/mgmt-data"
}
trap cleanup EXIT

echo "== 0. fetch quack + httpfs once, on the host =="
# `core` before `core_nightly`: quack is in both but not for the same DuckDB
# versions — at v1.5.5 core_nightly 404s and core serves it. httpfs is quack's
# transport and would otherwise autoload into the READ-ONLY mount and fail with a
# confusing "Read-only file system".
for repo in "" " FROM core_nightly"; do
  INSTALL_ERR="$("$DUCKDB_BIN" -c "SET extension_directory='$EXTS'; INSTALL quack$repo;" 2>&1)"
  find "$EXTS" -name 'quack.duckdb_extension' | grep -q . && break
done
HTTPFS_ERR="$("$DUCKDB_BIN" -c "SET extension_directory='$EXTS'; INSTALL httpfs;" 2>&1)"
if ! find "$EXTS" -name 'quack.duckdb_extension' | grep -q .; then
  echo "FAIL: could not install quack (needs DuckDB >= 1.5.2 + network)"
  echo "--- duckdb said ---"; echo "$INSTALL_ERR"
  "$DUCKDB_BIN" -c "SELECT version();" 2>&1 | tail -3
  echo "NOTE: a version like v0.0.1 means the duckdb submodule has no tags."
  exit 2
fi
if ! find "$EXTS" -name 'httpfs.duckdb_extension' | grep -q .; then
  echo "FAIL: could not install httpfs (quack's HTTP transport needs it)"
  echo "--- duckdb said ---"; echo "$HTTPFS_ERR"
  exit 2
fi
echo "   quack + httpfs ready in $EXTS"

echo "== 1. seed the management store (no IdP) =="
[[ -x "$NB/seeder/seeder" ]] || { echo "FAIL: build the seeder first (make nb_seeder)"; exit 2; }
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

echo "== 3. node B: official netbird daemon (kernel TUN) =="
echo "NB_SETUP_KEY=$SETUP_KEY" > "$NB/.env"
compose --profile peer --env-file "$NB/.env" up -d nb-peer 2>&1 | tail -2
B_IP=""
for _ in $(seq 1 50); do
  B_IP="$(docker exec erpl-nb-peer netbird status 2>/dev/null | grep -oE 'NetBird IP: 100\.[0-9]+\.[0-9]+\.[0-9]+' | grep -oE '100\.[0-9.]+' | head -1)"
  [[ -n "$B_IP" ]] && break; sleep 3
done
[[ -n "$B_IP" ]] || { echo "FAIL: node B has no NetBird IP"; docker logs erpl-nb-peer --tail 30; exit 1; }
echo "   node B: $B_IP"

echo "== 4. node A: serve quack + tunnel_export onto the NetBird network =="
docker run -d --name erpl-node-a --network "$NET" \
  --cap-add NET_ADMIN --device /dev/net/tun \
  -v "$EXT":/erpl_tunnel.duckdb_extension:ro \
  -v "$DUCKDB_BIN":/duckdb:ro \
  -v "$EXTS":/exts:ro \
  -v "$NB/nodea_quack_entrypoint.sh":/nodea.sh:ro \
  -e NB_SETUP_KEY="$SETUP_KEY" -e QUACK_TOKEN="$QUACK_TOKEN" \
  -e NB_FORCE_USERSPACE_FIREWALL=true \
  "$BASE_IMAGE" bash /nodea.sh >/dev/null

A_IP=""
for _ in $(seq 1 150); do
  logs="$(docker logs erpl-node-a 2>&1)"
  if echo "$logs" | grep -q NODEA_READY; then
    A_IP="$(echo "$logs" | grep -oE 'NODEA_IP=100\.[0-9.]+' | head -1 | cut -d= -f2)"
    break
  fi
  echo "$logs" | grep -q NODEA_FAIL && break
  sleep 2
done
if [[ -z "$A_IP" ]]; then
  echo "FAIL: node A never became ready"; docker logs erpl-node-a 2>&1 | tail -30; exit 1
fi
echo "   node A exporting quack on $A_IP:9494"

TROW="$(docker logs erpl-node-a 2>&1 | grep -oE 'NODEA_TUNNELS=[^ ]*' | head -1)"
if [[ "$TROW" == "NODEA_TUNNELS=export|$A_IP|9494" ]]; then
  echo "PASS: tunnels() reports the export and where to reach it ($TROW)"
else
  echo "FAIL: tunnels() row wrong or missing: '$TROW' (expected export|$A_IP|9494)"
  docker logs erpl-node-a 2>&1 | tail -20; exit 1
fi

echo "== 5. node B: ATTACH across the NetBird overlay and run a real query =="
# NetBird forms peer connections lazily, so the first attach can race the path
# coming up. Retry rather than declaring failure on a cold link.
B_OUT=""
for i in $(seq 1 30); do
  B_OUT="$(docker run --rm --network "container:erpl-nb-peer" \
    -v "$DUCKDB_BIN":/duckdb:ro -v "$EXTS":/exts:ro \
    "$BASE_IMAGE" /duckdb -noheader -list -c "
      SET extension_directory='/exts';
      LOAD httpfs;
      LOAD quack;
      ATTACH 'quack:$A_IP:9494' AS remote (TYPE quack, TOKEN '$QUACK_TOKEN', DISABLE_SSL true);
      SELECT 'ROWS=' || count(*) || ' NAME=' || max(name) FROM remote.main.observations;
    " 2>&1)"
  echo "$B_OUT" | grep -q "ROWS=3 NAME=gamma" && break
  [[ $((i % 10)) -eq 0 ]] && echo "   attempt $i: $(echo "$B_OUT" | tail -1)"
  sleep 3
done
echo "$B_OUT" | tail -3

if echo "$B_OUT" | grep -q "ROWS=3 NAME=gamma"; then
  echo "PASS: node B queried node A's DuckDB across the NetBird overlay through tunnel_export"
  echo "QUACK-OVER-NETBIRD OK"
  exit 0
fi
echo "FAIL: node B could not query node A"
echo "--- node A ---"; docker logs erpl-node-a 2>&1 | tail -20
echo "--- node B netbird status ---"; docker exec erpl-nb-peer netbird status 2>&1 | tail -20
exit 1
