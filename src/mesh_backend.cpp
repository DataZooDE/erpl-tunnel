#include "mesh_backend.hpp"

#include <map>
#include <mutex>
#include "mesh_peers_json.hpp"
#include "tunnel_secret.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "telemetry.hpp"

#include "yyjson.hpp"

#include <vector>
#include "erpl_tunnel_banner.hpp"

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

MeshStream MeshBackend::Dial(const std::string &host, int port) {
    EnsureUp();
    std::lock_guard<std::mutex> lock(mu_);
    MeshStream stream = kMeshStreamInvalid;
    if (api_->dial(node_, host.c_str(), port, &stream) != 0 || stream == kMeshStreamInvalid) {
        throw IOException("Tunnel: " + LastError());
    }
    return stream;
}

long MeshBackend::Export(int mesh_port, const std::string &local_host, int local_port) {
    EnsureUp();
    std::lock_guard<std::mutex> lock(mu_);
    long handle = 0;
    if (api_->mesh_export(node_, mesh_port, local_host.c_str(), local_port, &handle) != 0) {
        throw IOException("Tunnel: " + LastError());
    }
    return handle;
}

void MeshBackend::Unexport(long export_handle) noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    if (api_ != nullptr && node_ > 0 && export_handle != 0) {
        api_->mesh_unexport(node_, export_handle); // idempotent by contract
    }
}

// Cap the buffer the shim can ask us to allocate. `need` crosses the C ABI from the
// Go shim; a bug there must not drive unbounded allocation. 64 MB is far beyond any
// realistic peer-status payload (thousands of peers).
static constexpr size_t kMaxMeshJson = 64u * 1024u * 1024u;

std::string MeshBackend::PeersJson() {
    EnsureUp();
    std::lock_guard<std::mutex> lock(mu_);
    // Two-shot buffer contract: query the required size, then fill.
    size_t need = 0;
    std::vector<char> buf(8192);
    int rc = api_->peers_json(node_, buf.data(), buf.size(), &need);
    if (rc == 2 && need > buf.size()) {
        if (need > kMaxMeshJson) {
            throw IOException("Tunnel: mesh peer status is implausibly large; aborting.");
        }
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
        if (need > kMaxMeshJson) {
            throw IOException("Tunnel: mesh self status is implausibly large; aborting.");
        }
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
            "Tunnel: tunnel_peers/tunnel_self require secret = '<name>' naming a mesh tunnel "
            "secret, e.g. CREATE SECRET ts (TYPE tunnel, backend 'tailscale', auth_key '…', "
            "hostname '…');");
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

    // ONE node per secret, for the life of the process.
    //
    // This used to build a fresh MeshBackend on every call, which meant every
    // tunnel_self()/tunnel_peers()/tunnel_import/tunnel_export enrolled a SEPARATE
    // mesh node, then tore it down when the shared_ptr died. Three consequences,
    // all bad and only one of them obvious:
    //   - tunnel_self() reported an address that was not the address the export was
    //     published on, because they were different nodes. Actively misleading.
    //   - every call registered a new peer with the control plane (ephemeral peers
    //     piled up) and started a fresh WireGuard stack.
    //   - a node was started and stopped just to answer one status query.
    // Tailscale hid this whenever a state_dir was set — tsnet reuses the stored
    // identity, so the nodes happened to share an address. NetBird, whose ephemeral
    // peers get a fresh IP each enrolment, made it visible.
    //
    // Keyed on the full option set, not just the secret name, so redefining a
    // secret yields a new node rather than silently reusing the old credentials.
    const std::string key = secret_name + "\x1f" + std::to_string(static_cast<int>(opts.kind)) +
                            "\x1f" + opts.auth_key + "\x1f" + opts.setup_key + "\x1f" +
                            opts.hostname + "\x1f" + opts.tags + "\x1f" + opts.groups + "\x1f" +
                            opts.control_url + "\x1f" + opts.mgmt_url + "\x1f" + opts.state_dir +
                            "\x1f" + (opts.ephemeral ? "1" : "0");

    static std::mutex cache_mu;
    // Deliberately never destroyed: a MeshBackend destructor calls into the Go
    // runtime, and running that during static destruction — after the Go runtime
    // may already be tearing down — is how you get a crash on exit that nobody can
    // reproduce. The OS reclaims this at process end.
    static auto &cache = *new std::map<std::string, std::shared_ptr<MeshBackend>>();

    std::lock_guard<std::mutex> lock(cache_mu);
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }
    auto backend = std::make_shared<MeshBackend>(std::move(opts));
    cache.emplace(key, backend);
    return backend;
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
    TableFunction fn(name, {}, DATAZOO_GUARD(ERPL_TUNNEL_BANNER, MeshPeersEmit), bind);
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
    return PragmaFunction::PragmaCall("tunnel_mesh_activate",
                                      DATAZOO_GUARD(ERPL_TUNNEL_BANNER, MeshActivate),
                                      {LogicalType::VARCHAR});
}

TableFunction CreateTunnelPeersFunction() {
    return MakeMeshTableFunction("tunnel_peers", DATAZOO_GUARD(ERPL_TUNNEL_BANNER, TunnelPeersBind));
}

TableFunction CreateTunnelSelfFunction() {
    return MakeMeshTableFunction("tunnel_self", DATAZOO_GUARD(ERPL_TUNNEL_BANNER, TunnelSelfBind));
}

} // namespace duckdb
