#!/usr/bin/env bash
# Zero-dependency proof (BRD NFR-1 / M5). NO MOCKS.
#
# Loads the extension inside a MINIMAL glibc container that has NOTHING installed
# beyond the base image and a stock DuckDB CLI — no libssh2, no openssl, no Go
# runtime, no tailscale/netbird daemon. If LOAD succeeds and the tunnel functions
# register, the single loadable file is genuinely self-contained.
#
# Usage: test/integration/zero_dependency.sh <ext.duckdb_extension> [duckdb_version]
set -euo pipefail

EXT="${1:?path to a RELEASE erpl_tunnel.duckdb_extension required}"
# Base image whose glibc >= the extension's build floor. Locally we build on a
# rolling glibc, so archlinux:latest matches; CI builds on manylinux (old glibc)
# for broad portability, so any glibc distro works there. Override with BASE_IMAGE.
BASE_IMAGE="${BASE_IMAGE:-archlinux:latest}"
[[ -f "$EXT" ]] || { echo "FAIL: extension not found: $EXT"; exit 2; }

echo "== host-side: the loadable file links only the C/C++ runtime, nothing else =="
echo "-- ldd --"; ldd "$EXT" || true
# The core zero-dependency evidence: no dynamic link to libssh2/openssl/go/mesh.
if ldd "$EXT" 2>/dev/null | grep -Eiq "libssh2|libssl|libcrypto|tailscale|netbird|libgo"; then
  echo "FAIL: extension dynamically depends on an external library (should be static)"; exit 1
fi
echo "PASS: statically self-contained — only glibc/libstdc++/libm/libgcc"

echo "== container: minimal box, NOTHING installed, no Go runtime, no mesh daemon =="
DUCKDB_BIN="${DUCKDB_BIN:-$(command -v duckdb)}"
[[ -x "$DUCKDB_BIN" ]] || { echo "SKIP container LOAD: no duckdb CLI to stage"; exit 0; }

# SQL via a mounted file to avoid nested-quoting hazards.
QSQL="$(mktemp)"
cat > "$QSQL" <<'SQL'
LOAD '/erpl_tunnel.duckdb_extension';
SELECT count(*) AS active_tunnels FROM tunnels();
PRAGMA tunnel_close(999);
SQL
trap 'rm -f "$QSQL"' EXIT

docker run --rm --network none \
  -v "$(cd "$(dirname "$EXT")" && pwd)/$(basename "$EXT")":/erpl_tunnel.duckdb_extension:ro \
  -v "$DUCKDB_BIN":/duckdb:ro \
  -v "$QSQL":/q.sql:ro \
  "$BASE_IMAGE" bash -c '
    set -e
    for b in go tailscale tailscaled netbird; do
      command -v "$b" >/dev/null && { echo "unexpected: $b present"; exit 1; }
    done
    echo "box has no go/tailscale/netbird — good"
    /duckdb -unsigned -batch < /q.sql
  ' | tee /tmp/zerodep_out.txt

if grep -q "Tunnel not found or already closed" /tmp/zerodep_out.txt; then
  echo "ZERO-DEP OK — extension loaded and functioned in a bare glibc container"
else
  echo "FAIL: extension did not load/function in the bare container"; exit 1
fi
