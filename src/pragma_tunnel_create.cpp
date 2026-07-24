#include "pragma_tunnel_create.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"
#include "tunnel_manager.hpp"
#include "tunnel_secret.hpp"
#include "mesh_backend.hpp"
#include "telemetry.hpp"

namespace duckdb {

string TunnelCreate(ClientContext &context, const FunctionParameters &parameters) {
    PostHogTelemetry::Instance().RecordFunctionCall("tunnel_create");

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
        throw InvalidInputException("tunnel_create requires remote_host, remote_port, and local_port parameters");
    }

    // Route by the secret's backend (ADR-003 uniform engine). Mesh secrets forward
    // over the mesh node; ssh/absent secrets use the libssh2 path.
    auto secret_name = GetTunnelSecretNameFromParams(parameters);
    auto mesh_kind = SecretMeshKind(context, secret_name);
    // Record only the backend as a safe enum dimension — never the host, port, or
    // secret (privacy contract). Lets us see backend mix without leaking anything.
    const char *backend_name =
        (mesh_kind == MeshKind::Tailscale) ? "tailscale" : (mesh_kind == MeshKind::NetBird) ? "netbird" : "ssh";
    PostHogTelemetry::Instance().CaptureFeature("tunnel_create", {{"backend", backend_name}});

    int64_t tunnel_id;
    if (mesh_kind != MeshKind::None) {
        auto backend = MeshBackendFromSecret(context, secret_name);
        tunnel_id = g_tunnel_manager->CreateMeshTunnel(std::move(backend), remote_host, remote_port,
                                                       local_port, timeout_seconds, bind_all);
    } else {
        auto auth_params = GetTunnelAuthParamsFromContext(context, parameters);
        tunnel_id = g_tunnel_manager->CreateTunnel(auth_params, remote_host, remote_port, local_port,
                                                   auth_params.ssh_host, auth_params.ssh_port,
                                                   auth_params.ssh_user, timeout_seconds, bind_all);
    }

    auto pragma_query = StringUtil::Format("SELECT %lld as tunnel_id, 'Tunnel created successfully' as message", tunnel_id);
    return pragma_query;
}

PragmaFunction CreateTunnelCreatePragma() {
    auto tunnel_create_pragma = PragmaFunction::PragmaCall("tunnel_create", TunnelCreate, {});
    tunnel_create_pragma.named_parameters["secret"] = LogicalType::VARCHAR;
    tunnel_create_pragma.named_parameters["remote_host"] = LogicalType::VARCHAR;
    tunnel_create_pragma.named_parameters["remote_port"] = LogicalType::INTEGER;
    tunnel_create_pragma.named_parameters["local_port"] = LogicalType::INTEGER;
    tunnel_create_pragma.named_parameters["timeout"] = LogicalType::INTEGER;
    tunnel_create_pragma.named_parameters["bind_all"] = LogicalType::BOOLEAN;

    return tunnel_create_pragma;
}

} // namespace duckdb 