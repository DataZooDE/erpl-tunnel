#pragma once

#include "duckdb.hpp"
#include "duckdb/function/pragma_function.hpp"

namespace duckdb {

// Publish a local service onto the network so peers can reach it — the reverse of
// tunnel_import. Mesh backends only for now; SSH remote-forward is separate work.
PragmaFunction CreateTunnelExportPragma();

} // namespace duckdb
