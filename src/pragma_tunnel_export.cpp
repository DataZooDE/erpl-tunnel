#include "pragma_tunnel_export.hpp"

#include "duckdb/common/string_util.hpp"
#include "tunnel_manager.hpp"
#include "tunnel_secret.hpp"
#include "telemetry.hpp"

#ifdef ERPL_TUNNEL_HAS_MESH
#include "mesh_backend.hpp"
#endif

namespace duckdb {

string TunnelExport(ClientContext &context, const FunctionParameters &parameters) {
    PostHogTelemetry::Instance().RecordFunctionCall("tunnel_export");

    string local_host = "127.0.0.1";
    int local_port = 0;
    int remote_port = 0;
    string remote_host;
    for (const auto &param : parameters.named_parameters) {
        if (param.first == "local_host") {
            local_host = param.second.ToString();
        } else if (param.first == "local_port") {
            local_port = param.second.GetValue<int>();
        } else if (param.first == "remote_port") {
            remote_port = param.second.GetValue<int>();
        } else if (param.first == "remote_host") {
            remote_host = param.second.ToString();
        }
    }

    if (local_port == 0) {
        throw InvalidInputException(
            "tunnel_export requires local_port — the service on this machine to publish. Example:\n"
            "  PRAGMA tunnel_export(secret='ts', local_port=9494);\n"
            "  PRAGMA tunnel_export(secret='ts', local_port=9494, remote_port=19494);\n"
            "(named parameters use '=' — a PRAGMA does not accept ':=')");
    }
    // Publishing on the same port number is overwhelmingly the common case, so
    // default to it rather than making every call state the port twice.
    if (remote_port == 0) {
        remote_port = local_port;
    }

    auto secret_name = GetTunnelSecretNameFromParams(parameters);
    if (secret_name.empty()) {
        throw InvalidInputException(
            "tunnel_export requires secret = '<name>' naming the network to publish on, e.g. "
            "CREATE SECRET ts (TYPE tunnel, backend 'tailscale', auth_key '…', hostname '…');");
    }

#ifdef ERPL_TUNNEL_HAS_MESH
    auto mesh_kind = SecretMeshKind(context, secret_name);
    if (mesh_kind != MeshKind::None) {
        if (!remote_host.empty()) {
            throw InvalidInputException(
                "tunnel_export: remote_host applies only to the ssh backend. On a mesh the "
                "service is published on this node's own mesh address, which you can see with "
                "SELECT * FROM tunnel_self(secret = '" + secret_name + "');");
        }
        const char *backend_name = (mesh_kind == MeshKind::Tailscale) ? "tailscale" : "netbird";
        PostHogTelemetry::Instance().CaptureFeature("tunnel_export", {{"backend", backend_name}});

        auto backend = MeshBackendFromSecret(context, secret_name);
        const int64_t tunnel_id = g_tunnel_manager->CreateMeshExport(
            std::move(backend), remote_port, local_host, local_port);
        return StringUtil::Format(
            "SELECT %lld as tunnel_id, %d as remote_port, "
            "'Exporting %s:%d on the mesh' as message",
            tunnel_id, remote_port, local_host.c_str(), local_port);
    }
#endif

    // SSH remote-forward is a separate engine (libssh2 forward_listen); until it
    // lands, say so plainly rather than failing somewhere confusing downstream.
    throw InvalidInputException(
        "tunnel_export currently supports the mesh backends (tailscale, netbird). Secret '" +
        secret_name + "' is an SSH secret; SSH remote-forward export is not implemented yet.");
}

static PragmaFunction MakeExportPragma(const char *name) {
    auto pragma = PragmaFunction::PragmaCall(name, TunnelExport, {});
    pragma.named_parameters["secret"] = LogicalType::VARCHAR;
    pragma.named_parameters["local_port"] = LogicalType::INTEGER;
    pragma.named_parameters["local_host"] = LogicalType::VARCHAR;
    pragma.named_parameters["remote_port"] = LogicalType::INTEGER;
    pragma.named_parameters["remote_host"] = LogicalType::VARCHAR;
    pragma.named_parameters["timeout"] = LogicalType::INTEGER;
    return pragma;
}

PragmaFunction CreateTunnelExportPragma() { return MakeExportPragma("tunnel_export"); }

} // namespace duckdb
