#include "pragma_tunnel_create.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"
#include "tunnel_manager.hpp"
#include "tunnel_secret.hpp"
#ifdef ERPL_TUNNEL_HAS_MESH
#include "mesh_backend.hpp"
#endif
#include "telemetry.hpp"
#include "erpl_tunnel_banner.hpp"

namespace duckdb {

string TunnelCreate(ClientContext &context, const FunctionParameters &parameters) {
    PostHogTelemetry::Instance().RecordFunctionCall("tunnel_import");

    // Extract tunnel parameters from named parameters.
    string remote_host;
    int remote_port = 0;
    int local_port = 0;
    int timeout_seconds = 60; // Default timeout
    bool bind_all = false;    // Loopback bind by default (FR-2/ADR-006)

    for (const auto &param : parameters.named_parameters) {
        if (param.first == "remote_host") {
            remote_host = param.second.ToString();
        } else if (param.first == "remote_port") {
            remote_port = param.second.GetValue<int32_t>();
        } else if (param.first == "local_port") {
            local_port = param.second.GetValue<int32_t>();
        } else if (param.first == "timeout") {
            timeout_seconds = param.second.GetValue<int32_t>();
        } else if (param.first == "bind_all") {
            bind_all = param.second.GetValue<bool>();
        }
    }

    if (remote_host.empty() || remote_port == 0 || local_port == 0) {
        throw InvalidInputException(
            "tunnel_import requires remote_host, remote_port, and local_port. Example:\n"
            "  PRAGMA tunnel_import(secret='my_secret', remote_host='host.internal', "
            "remote_port=8000, local_port=9000 [, timeout=60, bind_all=false]);\n"
            "(named parameters use '=' — a PRAGMA does not accept ':=')");
    }

    // Route by the secret's backend (ADR-003 uniform engine). Mesh secrets forward
    // over the mesh node; ssh/absent secrets use the libssh2 path. On builds without
    // mesh (Windows/musl), only the SSH path exists.
    auto secret_name = GetTunnelSecretNameFromParams(parameters);
    int64_t tunnel_id;
#ifdef ERPL_TUNNEL_HAS_MESH
    auto mesh_kind = SecretMeshKind(context, secret_name);
    // Record only the backend as a safe enum dimension — never the host, port, or
    // secret (privacy contract). Lets us see backend mix without leaking anything.
    const char *backend_name =
        (mesh_kind == MeshKind::Tailscale) ? "tailscale" : (mesh_kind == MeshKind::NetBird) ? "netbird" : "ssh";
    PostHogTelemetry::Instance().CaptureFeature("tunnel_import", {{"backend", backend_name}});

    if (mesh_kind != MeshKind::None) {
        auto backend = MeshBackendFromSecret(context, secret_name);
        tunnel_id = g_tunnel_manager->CreateMeshTunnel(std::move(backend), remote_host, remote_port,
                                                       local_port, timeout_seconds, bind_all);
    } else
#else
    PostHogTelemetry::Instance().CaptureFeature("tunnel_import", {{"backend", "ssh"}});
#endif
    {
        auto auth_params = GetTunnelAuthParamsFromContext(context, parameters);
        tunnel_id = g_tunnel_manager->CreateTunnel(auth_params, remote_host, remote_port, local_port,
                                                   auth_params.ssh_host, auth_params.ssh_port,
                                                   auth_params.ssh_user, timeout_seconds, bind_all);
    }

    auto pragma_query = StringUtil::Format("SELECT %lld as tunnel_id, 'Tunnel created successfully' as message", tunnel_id);
    return pragma_query;
}

// One handler, registered under two names. `tunnel_import` says which way the
// tunnel points, which `tunnel_create` never did — that ambiguity is what made the
// signature hard to read for mesh backends, where the node has its own address.
// `tunnel_create` stays as an undocumented alias so existing scripts keep working.
static PragmaFunction MakeImportPragma(const char *name) {
    auto pragma = PragmaFunction::PragmaCall(name, DATAZOO_GUARD(ERPL_TUNNEL_BANNER, TunnelCreate), {});
    pragma.named_parameters["secret"] = LogicalType::VARCHAR;
    pragma.named_parameters["remote_host"] = LogicalType::VARCHAR;
    pragma.named_parameters["remote_port"] = LogicalType::INTEGER;
    pragma.named_parameters["local_port"] = LogicalType::INTEGER;
    pragma.named_parameters["timeout"] = LogicalType::INTEGER;
    pragma.named_parameters["bind_all"] = LogicalType::BOOLEAN;
    return pragma;
}

PragmaFunction CreateTunnelImportPragma() { return MakeImportPragma("tunnel_import"); }
PragmaFunction CreateTunnelCreatePragma() { return MakeImportPragma("tunnel_create"); }

} // namespace duckdb 