#include "mesh_backend.hpp"
#include "mesh_peers_json.hpp"
#include "tunnel_secret.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "telemetry.hpp"

#include "yyjson.hpp"

#include <unistd.h>
#include <vector>

using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

MeshKind ParseMeshKind(const std::string &backend) {
    auto b = StringUtil::Lower(backend);
    if (b == "tailscale") {
        return MeshKind::Tailscale;
    }
    if (b == "netbird") {
        return MeshKind::NetBird;
    }
    return MeshKind::None;
}

// ---------------------------------------------------------------------------
// MeshBackend
// ---------------------------------------------------------------------------

MeshBackend::MeshBackend(MeshOptions opts) : opts_(std::move(opts)) {}

MeshBackend::~MeshBackend() {
    std::lock_guard<std::mutex> lock(mu_);
    if (api_ && node_ > 0) {
        api_->close(node_);
        node_ = 0;
    }
}

std::string MeshBackend::LastError() {
    if (!api_ || node_ <= 0) {
        return "";
    }
    char buf[1024] = {0};
    api_->errmsg(node_, buf, sizeof(buf));
    return std::string(buf);
}

void MeshBackend::EnsureUp() {
    std::lock_guard<std::mutex> lock(mu_);
    if (up_) {
        return;
    }
    // Lazy dlopen + single-mesh latch happens here, on first activation.
    api_ = &MeshLoader::Activate(opts_.kind);
    if (node_ <= 0) {
        node_ = api_->node_new();
        if (node_ <= 0) {
            throw IOException("Tunnel: could not allocate a mesh node.");
        }
        auto set = [&](const char *k, const std::string &v) {
            if (!v.empty()) {
                api_->set_str(node_, k, v.c_str());
            }
        };
        set("auth_key", opts_.auth_key);
        set("setup_key", opts_.setup_key);
        set("hostname", opts_.hostname);
        set("tags", opts_.tags);
        set("groups", opts_.groups);
        set("control_url", opts_.control_url);
        set("mgmt_url", opts_.mgmt_url);
        set("state_dir", opts_.state_dir);
        api_->set_bool(node_, "ephemeral", opts_.ephemeral ? 1 : 0);
    }
    if (api_->up(node_) != 0) {
        throw IOException("Tunnel: " + LastError());
    }
    up_ = true;
}

int MeshBackend::Dial(const std::string &host, int port) {
    EnsureUp();
    std::lock_guard<std::mutex> lock(mu_);
    int fd = -1;
    if (api_->dial(node_, host.c_str(), port, &fd) != 0 || fd < 0) {
        throw IOException("Tunnel: " + LastError());
    }
    return fd;
}

std::string MeshBackend::PeersJson() {
    EnsureUp();
    std::lock_guard<std::mutex> lock(mu_);
    // Two-shot buffer contract: query the required size, then fill.
    size_t need = 0;
    std::vector<char> buf(8192);
    int rc = api_->peers_json(node_, buf.data(), buf.size(), &need);
    if (rc == 2 && need > buf.size()) {
        buf.resize(need);
        rc = api_->peers_json(node_, buf.data(), buf.size(), &need);
    }
    if (rc != 0) {
        throw IOException("Tunnel: " + LastError());
    }
    return std::string(buf.data());
}

std::string MeshBackend::SelfJson() {
    EnsureUp();
    std::lock_guard<std::mutex> lock(mu_);
    size_t need = 0;
    std::vector<char> buf(4096);
    int rc = api_->self_json(node_, buf.data(), buf.size(), &need);
    if (rc == 2 && need > buf.size()) {
        buf.resize(need);
        rc = api_->self_json(node_, buf.data(), buf.size(), &need);
    }
    if (rc != 0) {
        throw IOException("Tunnel: " + LastError());
    }
    return std::string(buf.data());
}

// ---------------------------------------------------------------------------
// tunnel_peers() / tunnel_self() table functions
// ---------------------------------------------------------------------------

namespace {

// The peer-status JSON parsing lives in the pure, DuckDB-free mesh_peers_json.* so it
// can be unit-tested standalone (test/cpp). MeshPeer is that module's row type.
using MeshPeerRow = MeshPeer;

} // namespace

MeshKind SecretMeshKind(ClientContext &context, const std::string &secret_name) {
    if (secret_name.empty()) {
        return MeshKind::None;
    }
    auto match = LookupTunnelSecret(context, secret_name);
    if (!match.HasMatch()) {
        return MeshKind::None;
    }
    const auto &secret = dynamic_cast<const KeyValueSecret &>(match.GetSecret());
    auto v = secret.TryGetValue("backend");
    return v.IsNull() ? MeshKind::None : ParseMeshKind(v.ToString());
}

// Build a MeshBackend from a tunnel secret (backend != ssh).
std::shared_ptr<MeshBackend> MeshBackendFromSecret(ClientContext &context, const std::string &secret_name) {
    if (secret_name.empty()) {
        throw InvalidInputException(
            "Tunnel: tunnel_peers/tunnel_self require secret := '<name>' naming a mesh "
            "tunnel secret (backend 'tailscale' or 'netbird').");
    }
    auto match = LookupTunnelSecret(context, secret_name);
    if (!match.HasMatch()) {
        throw InvalidInputException("Tunnel: secret '" + secret_name + "' not found.");
    }
    const auto &secret = dynamic_cast<const KeyValueSecret &>(match.GetSecret());
    auto get = [&](const char *k) -> std::string {
        auto v = secret.TryGetValue(k);
        return v.IsNull() ? std::string() : v.ToString();
    };
    MeshOptions opts;
    opts.kind = ParseMeshKind(get("backend"));
    if (opts.kind == MeshKind::None) {
        throw InvalidInputException(
            "Tunnel: secret '" + secret_name + "' is not a mesh secret. Set backend "
            "'tailscale' or 'netbird' (with auth_key/setup_key) to use mesh discovery.");
    }
    opts.auth_key = get("auth_key");
    opts.setup_key = get("setup_key");
    opts.hostname = get("hostname");
    opts.tags = get("tags");
    opts.groups = get("groups");
    opts.control_url = get("control_url");
    opts.mgmt_url = get("management_url");
    opts.state_dir = get("state_dir");
    auto eph = get("ephemeral");
    opts.ephemeral = (eph == "true" || eph == "1");
    return make_uniq<MeshBackend>(std::move(opts));
}

namespace {

struct MeshPeersBindData : public FunctionData {
    explicit MeshPeersBindData(std::vector<MeshPeerRow> rows_p) : rows(std::move(rows_p)) {}
    std::vector<MeshPeerRow> rows;
    idx_t cursor = 0;
    unique_ptr<FunctionData> Copy() const override { return make_uniq<MeshPeersBindData>(rows); }
    bool Equals(const FunctionData &) const override { return false; }
};

void PeersSchema(vector<LogicalType> &types, vector<string> &names) {
    types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
             LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR), LogicalType::BOOLEAN};
    names = {"backend", "host_name", "dns_name", "mesh_ip", "tags", "online"};
}

unique_ptr<FunctionData> TunnelPeersBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("tunnel_peers");
    PeersSchema(return_types, names);
    auto secret_name = GetTunnelSecretNameFromParams(input);
    auto backend = MeshBackendFromSecret(context, secret_name);
    auto rows = ParseMeshPeersJson(backend->PeersJson());
    return make_uniq<MeshPeersBindData>(std::move(rows));
}

unique_ptr<FunctionData> TunnelSelfBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("tunnel_self");
    PeersSchema(return_types, names);
    auto secret_name = GetTunnelSecretNameFromParams(input);
    auto backend = MeshBackendFromSecret(context, secret_name);
    // self is a single object; wrap into a one-element array for uniform emission.
    auto self = backend->SelfJson();
    auto rows = ParseMeshPeersJson("[" + self + "]");
    return make_uniq<MeshPeersBindData>(std::move(rows));
}

void MeshPeersEmit(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind = data_p.bind_data->CastNoConst<MeshPeersBindData>();
    idx_t count = 0;
    while (bind.cursor < bind.rows.size() && count < STANDARD_VECTOR_SIZE) {
        const auto &r = bind.rows[bind.cursor];
        output.SetValue(0, count, Value(r.backend));
        output.SetValue(1, count, Value(r.host_name));
        output.SetValue(2, count, Value(r.dns_name));
        output.SetValue(3, count, Value(r.mesh_ip));
        vector<Value> tagvals;
        for (const auto &t : r.tags) {
            tagvals.emplace_back(Value(t));
        }
        output.SetValue(4, count, Value::LIST(LogicalType::VARCHAR, tagvals));
        output.SetValue(5, count, Value::BOOLEAN(r.online));
        bind.cursor++;
        count++;
    }
    output.SetCardinality(count);
}

TableFunction MakeMeshTableFunction(const char *name, table_function_bind_t bind) {
    TableFunction fn(name, {}, MeshPeersEmit, bind);
    fn.named_parameters["secret"] = LogicalType::VARCHAR;
    return fn;
}

} // namespace

string MeshActivate(ClientContext &, const FunctionParameters &parameters) {
    PostHogTelemetry::Instance().RecordFunctionCall("tunnel_mesh_activate");
    if (parameters.values.empty()) {
        throw InvalidInputException("tunnel_mesh_activate('tailscale'|'netbird')");
    }
    auto backend = parameters.values[0].ToString();
    auto kind = ParseMeshKind(backend);
    if (kind == MeshKind::None) {
        throw InvalidInputException("tunnel_mesh_activate: backend must be 'tailscale' or 'netbird'.");
    }
    MeshLoader::Activate(kind); // lazy dlopen + single-mesh latch (throws actionably)
    return StringUtil::Format("SELECT '%s' AS active_mesh", MeshKindName(kind));
}

PragmaFunction CreateMeshActivatePragma() {
    return PragmaFunction::PragmaCall("tunnel_mesh_activate", MeshActivate, {LogicalType::VARCHAR});
}

TableFunction CreateTunnelPeersFunction() {
    return MakeMeshTableFunction("tunnel_peers", TunnelPeersBind);
}

TableFunction CreateTunnelSelfFunction() {
    return MakeMeshTableFunction("tunnel_self", TunnelSelfBind);
}

} // namespace duckdb
