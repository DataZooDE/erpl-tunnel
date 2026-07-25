#pragma once

#include "duckdb.hpp"
#include "duckdb/function/pragma_function.hpp"

namespace duckdb {

string TunnelCreate(ClientContext &context, const FunctionParameters &parameters);
// Consume a remote service: bind a local port, dial the far side.
PragmaFunction CreateTunnelImportPragma();
// Deprecated spelling of the same thing, kept so existing scripts keep working.
PragmaFunction CreateTunnelCreatePragma();

} // namespace duckdb 