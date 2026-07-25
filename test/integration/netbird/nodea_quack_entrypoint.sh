#!/usr/bin/env bash
# Runs INSIDE node A's container. The EXPORT direction on NetBird: enrol our
# client/embed node, serve the quack protocol on loopback, and publish that port
# onto the NetBird network with tunnel_export. Then stay alive for node B.
#
# Env: NB_SETUP_KEY, QUACK_TOKEN. Mounts: /duckdb, /erpl_tunnel.duckdb_extension,
# /exts (an extension directory already holding quack + httpfs).
set -e

PIPE=$(mktemp -u); mkfifo "$PIPE"; exec 9<>"$PIPE"
/duckdb -unsigned < "$PIPE" > /tmp/a.out 2>&1 &

# quack (and httpfs, its transport) come from a mounted directory: the mesh
# containers have no reason to reach the extension repository, and a network fetch
# here would make a data-plane test fail for an unrelated reason.
cat >&9 <<SQL
SET extension_directory='/exts';
LOAD httpfs;
LOAD quack;
LOAD '/erpl_tunnel.duckdb_extension';
CREATE TABLE observations AS SELECT * FROM (VALUES (1,'alpha'),(2,'beta'),(3,'gamma')) t(id,name);
CREATE SECRET a (TYPE tunnel, backend 'netbird', setup_key '${NB_SETUP_KEY}',
    management_url 'http://management:80', hostname 'erpl-node-a',
    state_dir '/tmp/nbstate', ephemeral true);
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
SELECT 'NODEA_EXPORTING' AS m;
SQL

for _ in $(seq 1 60); do grep -qE "NODEA_EXPORTING|Error" /tmp/a.out && break; sleep 1; done
grep -q NODEA_EXPORTING /tmp/a.out || { echo "NODEA_FAIL export"; tail -30 /tmp/a.out; exit 1; }

# tunnels() must describe the export usefully: direction, and the mesh address a
# peer should dial. An empty remote_host means reachable but undiscoverable.
cat >&9 <<'SQL'
SELECT 'NODEA_TUNNELS=' || direction || '|' || remote_host || '|' || remote_port AS m
  FROM tunnels() WHERE direction = 'export';
SQL
for _ in $(seq 1 20); do grep -q "NODEA_TUNNELS=" /tmp/a.out && break; sleep 1; done
grep -oE "NODEA_TUNNELS=[^ ]*" /tmp/a.out | head -1 || true

echo "NODEA_READY"

# Stay alive for node B. The driver kills the container.
sleep 600
