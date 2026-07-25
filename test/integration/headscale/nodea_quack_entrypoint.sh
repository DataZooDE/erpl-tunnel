#!/usr/bin/env bash
# Runs INSIDE node A's container. This is the EXPORT direction: node A joins the
# tailnet with our extension, serves the quack protocol on loopback, and publishes
# that port onto the tailnet with tunnel_export. It then stays alive so node B can
# query it.
#
# Env: TS_AUTHKEY, QUACK_TOKEN. Mounts: /duckdb, /erpl_tunnel.duckdb_extension,
# /exts (a DuckDB extension directory already containing quack).
set -e

PIPE=$(mktemp -u); mkfifo "$PIPE"; exec 9<>"$PIPE"
/duckdb -unsigned < "$PIPE" > /tmp/a.out 2>&1 &

# quack is loaded from a mounted extension directory rather than installed from
# core_nightly: the mesh containers have no reason to have internet access, and a
# network fetch here would make a data-plane test fail for a repository reason.
cat >&9 <<SQL
SET extension_directory='/exts';
LOAD httpfs;
LOAD quack;
LOAD '/erpl_tunnel.duckdb_extension';
CREATE TABLE observations AS SELECT * FROM (VALUES (1,'alpha'),(2,'beta'),(3,'gamma')) t(id,name);
CREATE SECRET a (TYPE tunnel, backend 'tailscale', auth_key '${TS_AUTHKEY}',
    control_url 'http://headscale:8080', hostname 'erpl-node-a',
    tags 'duckdb-export', state_dir '/tmp/tsstate', ephemeral true);
SELECT 'NODEA_IP=' || mesh_ip AS m FROM tunnel_self(secret='a');
SQL

# The mesh IP proves enrolment finished; without it there is nothing to export onto.
# tunnel_self() can answer before the overlay address is assigned, so re-ask
# rather than accepting the first (possibly empty) answer.
for _ in $(seq 1 60); do
  grep -qE "NODEA_IP=100\." /tmp/a.out && break
  sleep 2
  cat >&9 <<'SQL'
SELECT 'NODEA_IP=' || mesh_ip AS m FROM tunnel_self(secret='a');
SQL
done
grep -qE "NODEA_IP=100\." /tmp/a.out || { echo "NODEA_FAIL enrol (no overlay address)"; tail -30 /tmp/a.out; exit 1; }
grep -oE "NODEA_IP=100\.[0-9.]+" /tmp/a.out | head -1

cat >&9 <<SQL
CALL quack_serve('quack:127.0.0.1:9494', token => '${QUACK_TOKEN}',
    allow_other_hostname => true);
PRAGMA tunnel_export(secret='a', local_port=9494);
-- A SECOND export from the same node: multi-port has to work on ONE mesh identity,
-- which is exactly what the old per-call node construction broke (each export got
-- its own node and its own address).
PRAGMA tunnel_export(secret='a', local_port=9494, remote_port=9495);
SELECT 'NODEA_EXPORTING' AS m;
SQL

for _ in $(seq 1 60); do grep -qE "NODEA_EXPORTING|Error" /tmp/a.out && break; sleep 1; done
grep -q NODEA_EXPORTING /tmp/a.out || { echo "NODEA_FAIL export"; cat /tmp/a.out; exit 1; }
# tunnels() must describe the export usefully: direction, and the mesh address a
# peer should dial. remote_host is filled in from the node's own identity, so an
# empty one here means the export is reachable but undiscoverable.
cat >&9 <<'SQL'
SELECT 'NODEA_TUNNELS=' || direction || '|' || remote_host || '|' || remote_port AS m
  FROM tunnels() WHERE direction = 'export' AND remote_port = 9494;
SELECT 'NODEA_EXPORTS=' || count(*) || '|' || count(DISTINCT remote_host) AS m
  FROM tunnels() WHERE direction = 'export';
-- The tags the CONTROL PLANE actually granted, not what we asked for. `tags` was a
-- silent no-op before, so this is the assertion that it is really applied.
SELECT 'NODEA_TAGS=[' || coalesce(array_to_string(list_sort(tags), ','), '') || ']' AS m
  FROM tunnel_self(secret='a');
SQL
# Wait for BOTH marker rows: /tmp/a.out is inside the container, so anything the
# driver needs must be echoed to stdout where `docker logs` can see it.
for _ in $(seq 1 30); do grep -q "NODEA_TAGS=" /tmp/a.out && break; sleep 1; done
grep -oE "NODEA_TUNNELS=[^ ]*" /tmp/a.out | head -1 || true
grep -oE "NODEA_EXPORTS=[0-9]+.[0-9]+" /tmp/a.out | head -1 || true
grep -oE "NODEA_TAGS=[^ ]*" /tmp/a.out | head -1 || true

echo "NODEA_READY"

# Stay alive for node B. The driver kills the container.
sleep 600
