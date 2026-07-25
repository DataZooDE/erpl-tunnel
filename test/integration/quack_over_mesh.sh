#!/usr/bin/env bash
# THE ACCEPTANCE TEST for tunnel_export. NO MOCKS, NO CLOUD.
#
# Reverses the direction of mesh_dataplane_tailscale.sh. There, node A (our
# extension) always initiated and node B was passive. Here node A is the SERVER:
#
#   node A = our extension. Joins the tailnet, serves the quack protocol on
#            127.0.0.1:9494, and publishes it with PRAGMA tunnel_export.
#   node B = a plain DuckDB running inside the netns of the OFFICIAL tailscale
#            daemon (kernel TUN), which ATTACHes to node A's 100.x:9494 and runs
#            a real query.
#
# A pass means a peer that knew nothing but node A's mesh address executed SQL
# inside node A's DuckDB, with the bytes crossing WireGuard — the thing
# tunnel_export exists to make possible.
#
# Node B borrows the tailscale daemon's network namespace rather than enrolling a
# second node: it needs the daemon's TUN interface, not an identity of its own.
#
# quack is loaded from a mounted extension directory, not installed from
# core_nightly inside the containers — a data-plane test must not fail because a
# repository was unreachable. The host fetches it once, up front.
#
# Usage: MESH_BACKEND=tailscale|both make release
#        test/integration/quack_over_mesh.sh <ext> [duckdb-bin]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HS="$HERE/headscale"
EXT="${1:?path to a RELEASE erpl_tunnel.duckdb_extension (tailscale/both) required}"
DUCKDB_BIN="${2:-${DUCKDB_BIN:-$(command -v duckdb)}}"
BASE_IMAGE="${BASE_IMAGE:-archlinux:latest}"   # glibc must match the extension build
QUACK_TOKEN="erpl-tunnel-acceptance-token"
[[ -f "$EXT" ]] || { echo "FAIL: extension not found: $EXT"; exit 2; }
[[ -x "$DUCKDB_BIN" ]] || { echo "FAIL: no duckdb binary: $DUCKDB_BIN"; exit 2; }
# docker -v REQUIRES absolute paths — a relative one is parsed as a named volume and
# fails with a confusing "invalid characters for a local volume name". Normalise here
# so the script works however it was invoked, not just via the Makefile.
EXT="$(realpath "$EXT")"
DUCKDB_BIN="$(realpath "$DUCKDB_BIN")"
NET="$(basename "$HS")_mesh"

EXTS="$(mktemp -d)"
compose() { docker compose -f "$HS/docker-compose.yml" "$@"; }
hs() { docker exec erpl-headscale headscale "$@"; }
cleanup() {
  docker rm -f erpl-node-a >/dev/null 2>&1 || true
  rm -rf "$EXTS"; rm -f "$HS/.env"
  compose --profile peer down -v >/dev/null 2>&1 || true
}
trap cleanup EXIT

# Preflight: a DuckDB that cannot name its own version cannot install anything.
# The extension repository is version-and-platform stamped, so a build reporting
# v0.0.1 (DuckDB's fallback when `git describe` fails on the submodule — see the
# CMake warning "Continuing with dummy version v0.0.1") 404s on every request.
# Checking here turns two confusing 404s into one sentence naming the cause.
DUCKDB_VER="$("$DUCKDB_BIN" -noheader -list -c "SELECT version();" 2>/dev/null | tr -d '[:space:]')"
if [[ "$DUCKDB_VER" == "v0.0.1" || -z "$DUCKDB_VER" ]]; then
  echo "FAIL: this DuckDB reports version '${DUCKDB_VER:-<none>}', so no extension"
  echo "      repository can serve it. The duckdb submodule has no tags — the build"
  echo "      fell back to a dummy version. Fix with:"
  echo "        git -C duckdb fetch --tags --force && <rebuild>"
  echo "      (In CI, actions/checkout does not fetch submodule tags.)"
  exit 2
fi
echo "   duckdb reports $DUCKDB_VER"

echo "== 0. fetch quack once, on the host =="
# Fetched with the SAME binary both containers will run, so the extension
# directory layout and version stamp match what they expect.
#
# `core` first, `core_nightly` second — NOT the other way round. quack is published
# to both, but not for the same set of DuckDB versions: at v1.5.5 core_nightly
# 404s while core serves it. Trying only core_nightly makes this test look like a
# network failure on exactly the version the extension ships against.
for repo in "" " FROM core_nightly"; do
  INSTALL_ERR="$("$DUCKDB_BIN" -c "SET extension_directory='$EXTS'; INSTALL quack$repo;" 2>&1)"
  find "$EXTS" -name 'quack.duckdb_extension' | grep -q . && break
done
# quack's transport is HTTP, so the client autoloads httpfs. The containers mount
# this directory READ-ONLY, so an autoload there fails with a confusing
# "Read-only file system" rather than a network error. Fetch it up front.
HTTPFS_ERR="$("$DUCKDB_BIN" -c "SET extension_directory='$EXTS'; INSTALL httpfs;" 2>&1)"
if ! find "$EXTS" -name 'httpfs.duckdb_extension' | grep -q .; then
  echo "FAIL: could not install httpfs (quack's HTTP transport needs it)"
  echo "--- duckdb said ---"; echo "$HTTPFS_ERR"
  exit 2
fi
if ! find "$EXTS" -name 'quack.duckdb_extension' | grep -q .; then
  echo "FAIL: could not install quack from core or core_nightly."
  echo "--- duckdb said ---"; echo "$INSTALL_ERR"
  echo "--- this DuckDB reports version ---"; "$DUCKDB_BIN" -c "SELECT version();" 2>&1 | tail -3
  echo "NOTE: a version like v0.0.1 means the duckdb submodule has no tags, so the"
  echo "      extension repository has nothing to serve. Run: git -C duckdb fetch --tags"
  exit 2
fi
echo "   quack ready in $EXTS"

echo "== 1. Headscale up =="
compose up -d headscale --wait 2>&1 | tail -1 || compose up -d headscale 2>&1 | tail -2
for _ in $(seq 1 30); do hs users list >/dev/null 2>&1 && break; sleep 1; done
hs users list >/dev/null 2>&1 || { echo "FAIL: headscale not ready"; compose logs --tail 20 headscale; exit 1; }

echo "== 2. user + reusable preauth key =="
hs users create duckdb >/dev/null 2>&1 || true
UID_NUM="$(hs users list -o json 2>/dev/null | grep -o '"id":[0-9]*' | head -1 | grep -o '[0-9]*')"; UID_NUM="${UID_NUM:-1}"
KEY="$(hs preauthkeys create --user "$UID_NUM" --reusable --expiration 1h 2>/dev/null | tr -d '[:space:]')"
[[ ${#KEY} -ge 20 ]] || { echo "FAIL: could not mint preauth key"; exit 1; }

echo "== 3. node B's tailscale daemon (kernel TUN) =="
echo "TS_AUTHKEY=$KEY" > "$HS/.env"
compose --profile peer --env-file "$HS/.env" up -d ts-peer 2>&1 | tail -1
B_IP=""
for _ in $(seq 1 60); do
  B_IP="$(docker exec erpl-ts-peer tailscale ip -4 2>/dev/null | tr -d '[:space:]')"
  [[ "$B_IP" =~ ^100\. ]] && break; sleep 2
done
[[ "$B_IP" =~ ^100\. ]] || { echo "FAIL: node B has no tailnet IP"; docker logs erpl-ts-peer --tail 30; exit 1; }
echo "   node B: $B_IP"

echo "== 4. node A: serve quack + tunnel_export onto the tailnet =="
docker run -d --name erpl-node-a --network "$NET" \
  -v "$EXT":/erpl_tunnel.duckdb_extension:ro \
  -v "$DUCKDB_BIN":/duckdb:ro \
  -v "$EXTS":/exts:ro \
  -v "$HS/nodea_quack_entrypoint.sh":/nodea.sh:ro \
  -e TS_AUTHKEY="$KEY" -e QUACK_TOKEN="$QUACK_TOKEN" \
  "$BASE_IMAGE" bash /nodea.sh >/dev/null

A_IP=""
for _ in $(seq 1 120); do
  logs="$(docker logs erpl-node-a 2>&1)"
  if echo "$logs" | grep -q NODEA_READY; then
    A_IP="$(echo "$logs" | grep -oE 'NODEA_IP=100\.[0-9.]+' | head -1 | cut -d= -f2)"
    break
  fi
  echo "$logs" | grep -q NODEA_FAIL && break
  sleep 2
done
if [[ -z "$A_IP" ]]; then
  echo "FAIL: node A never became ready"; docker logs erpl-node-a 2>&1 | tail -25; exit 1
fi
echo "   node A exporting quack on $A_IP:9494"

# The export must be discoverable through tunnels(), not just functional: direction
# 'export' and the node's own mesh address, which is what a peer needs to dial.
TROW="$(docker logs erpl-node-a 2>&1 | grep -oE 'NODEA_TUNNELS=[^ ]*' | head -1)"
if [[ "$TROW" == "NODEA_TUNNELS=export|$A_IP|9494" ]]; then
  echo "PASS: tunnels() reports the export and where to reach it ($TROW)"
else
  echo "FAIL: tunnels() row wrong or missing: '$TROW' (expected export|$A_IP|9494)"
  docker logs erpl-node-a 2>&1 | tail -20; exit 1
fi

echo "== 5. node B: ATTACH across the tailnet and run a real query =="
# DISABLE_SSL is required — the quack client defaults to HTTPS for a non-local
# address, and WireGuard already encrypts this hop.
set +e
B_OUT="$(docker run --rm --network "container:erpl-ts-peer" \
  -v "$DUCKDB_BIN":/duckdb:ro -v "$EXTS":/exts:ro \
  "$BASE_IMAGE" /duckdb -noheader -list -c "
    SET extension_directory='/exts';
    LOAD httpfs;
    LOAD quack;
    ATTACH 'quack:$A_IP:9494' AS remote (TYPE quack, TOKEN '$QUACK_TOKEN', DISABLE_SSL true);
    SELECT 'ROWS=' || count(*) || ' NAME=' || max(name) FROM remote.main.observations;
  " 2>&1)"
rc=$?
set -e
echo "$B_OUT" | tail -5

if echo "$B_OUT" | grep -q "ROWS=3 NAME=gamma"; then
  echo "PASS: node B queried node A's DuckDB across the tailnet through tunnel_export"
  echo "QUACK-OVER-TAILSCALE OK"
  exit 0
fi
echo "FAIL: node B could not query node A (rc=$rc)"
echo "--- node A ---"; docker logs erpl-node-a 2>&1 | tail -20
exit 1
