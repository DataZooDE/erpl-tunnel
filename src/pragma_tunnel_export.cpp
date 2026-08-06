#include "pragma_tunnel_export.hpp"

#include "duckdb/common/string_util.hpp"
#include "tunnel_manager.hpp"
#include "tunnel_secret.hpp"
#include "tunnel_connection.hpp"
#include "ssh_exporter.hpp"
#include "telemetry.hpp"
#include "erpl_tunnel_banner.hpp"

#ifdef ERPL_TUNNEL_HAS_MESH
#include "mesh_backend.hpp"
#endif

namespace duckdb {

string TunnelExport(ClientContext &context, const FunctionParameters &parameters) {
    PostHogTelemetry::Instance().RecordFunctionCall("tunnel_export");

    string local_host = "127.0.0.1";
    int local_port = 0;
    int remote_port = 0;
    // "omitted" and "given as 0" are DIFFERENT: 0 is how you ask an SSH server to
    // allocate a port for you, so collapsing them would make that documented
    // feature unreachable.
    bool remote_port_given = false;
    string remote_host;
    for (const auto &param : parameters.named_parameters) {
        if (param.first == "local_host") {
            local_host = param.second.ToString();
        } else if (param.first == "local_port") {
            local_port = param.second.GetValue<int>();
        } else if (param.first == "remote_port") {
            remote_port = param.second.GetValue<int>();
            remote_port_given = true;
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
    // Validate before anything reaches getaddrinfo, a Go listener, or libssh2 —
    // each mangles an out-of-range port differently, and none of them produce an
    // error a user can act on.
    if (local_port < kMinPortNumber || local_port > kMaxPortNumber) {
        throw InvalidInputException(
            "tunnel_export: local_port must be between " + std::to_string(kMinPortNumber) +
            " and " + std::to_string(kMaxPortNumber) + " (got " + std::to_string(local_port) +
            ").");
    }
    // Publishing on the same port number is overwhelmingly the common case, so
    // default to it rather than making every call state the port twice.
    if (!remote_port_given) {
        remote_port = local_port;
    }
    // 0 means "server, choose one for me", which only an SSH server can do; a mesh
    // export binds a port on this node's own address and needs a real number.
    const bool wants_server_allocated = (remote_port == 0);
    if (!wants_server_allocated &&
        (remote_port < kMinPortNumber || remote_port > kMaxPortNumber)) {
        throw InvalidInputException(
            "tunnel_export: remote_port must be between " + std::to_string(kMinPortNumber) +
            " and " + std::to_string(kMaxPortNumber) + ", or 0 to let an SSH server allocate "
            "one (got " + std::to_string(remote_port) + ").");
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
        if (wants_server_allocated) {
            throw InvalidInputException(
                "tunnel_export: remote_port = 0 asks the server to allocate a port, which only "
                "the ssh backend can do. On a mesh you choose the port yourself — pass the port "
                "peers should connect to, or omit remote_port to reuse local_port.");
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

    // SSH: a real remote-forward (ssh -R) via libssh2_channel_forward_listen.
    PostHogTelemetry::Instance().CaptureFeature("tunnel_export", {{"backend", "ssh"}});
    auto auth_params = GetTunnelAuthParamsFromContext(context, parameters);
    int timeout_seconds = 60;
    for (const auto &param : parameters.named_parameters) {
        if (param.first == "timeout") {
            timeout_seconds = param.second.GetValue<int>();
        }
    }
    const int64_t tunnel_id = g_tunnel_manager->CreateSshExport(
        auth_params, local_host, local_port, remote_host, remote_port, timeout_seconds);
    const int bound = g_tunnel_manager->GetTunnelBoundPort(tunnel_id);
    return StringUtil::Format(
        "SELECT %lld as tunnel_id, %d as remote_port, "
        "'Exporting %s:%d via the SSH server' as message",
        tunnel_id, bound, local_host.c_str(), local_port);
}

static PragmaFunction MakeExportPragma(const char *name) {
    auto pragma = PragmaFunction::PragmaCall(name, DATAZOO_GUARD(ERPL_TUNNEL_BANNER, TunnelExport), {});
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
