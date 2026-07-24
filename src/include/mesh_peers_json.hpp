#pragma once

// Pure, DuckDB-free parser for the peer-status JSON the mesh shims return
// (mesh_peers_json / mesh_self_json over the C ABI). Kept free of DuckDB and mesh
// types so it can be unit-tested standalone (test/cpp), like erpl-idoc's format core.
// The input is untrusted (it crosses the C ABI from the Go shim), so the parser is
// defensive: malformed input yields an empty vector, missing fields default.

#include <string>
#include <vector>

namespace duckdb {

struct MeshPeer {
    std::string backend;
    std::string host_name;
    std::string dns_name;
    std::string mesh_ip;
    std::vector<std::string> tags;
    bool online = false;
};

// Parse a JSON array of peer objects into MeshPeer rows. Returns empty on any parse
// error. Non-object array elements are skipped; unknown keys are ignored.
std::vector<MeshPeer> ParseMeshPeersJson(const std::string &json);

} // namespace duckdb
