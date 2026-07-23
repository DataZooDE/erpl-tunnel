#include "mesh_backend.hpp"
#include "tunnel_secret.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/common/string_util.hpp"

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

struct MeshPeerRow {
    std::string backend;
    std::string host_name;
    std::string dns_name;
    std::string mesh_ip;
    std::vector<std::string> tags;
    bool online = false;
};

std::string JsonStr(yyjson_val *obj, const char *key) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    return (v && yyjson_is_str(v)) ? std::string(yyjson_get_str(v)) : std::string();
}

std::vector<MeshPeerRow> ParsePeerArray(const std::string &json) {
    std::vector<MeshPeerRow> rows;
    yyjson_doc *doc = yyjson_read(json.c_str(), json.size(), 0);
    if (!doc) {
        return rows;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root && yyjson_is_arr(root)) {
        size_t idx, max;
        yyjson_val *el;
        yyjson_arr_foreach(root, idx, max, el) {
            MeshPeerRow r;
            r.backend = JsonStr(el, "backend");
            r.host_name = JsonStr(el, "host_name");
            r.dns_name = JsonStr(el, "dns_name");
            r.mesh_ip = JsonStr(el, "mesh_ip");
            yyjson_val *online = yyjson_obj_get(el, "online");
            r.online = online && yyjson_is_true(online);
            yyjson_val *tags = yyjson_obj_get(el, "tags");
            if (tags && yyjson_is_arr(tags)) {
                size_t ti, tmax;
                yyjson_val *tv;
                yyjson_arr_foreach(tags, ti, tmax, tv) {
                    if (yyjson_is_str(tv)) {
                        r.tags.emplace_back(yyjson_get_str(tv));
                    }
                }
            }
            rows.push_back(std::move(r));
        }
    }
    yyjson_doc_free(doc);
    return rows;
}

// Build a MeshBackend from a tunnel secret (backend != ssh).
unique_ptr<MeshBackend> MeshBackendFromSecret(ClientContext &context, const std::string &secret_name) {
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
    PeersSchema(return_types, names);
    auto secret_name = GetTunnelSecretNameFromParams(input);
    auto backend = MeshBackendFromSecret(context, secret_name);
    auto rows = ParsePeerArray(backend->PeersJson());
    return make_uniq<MeshPeersBindData>(std::move(rows));
}

unique_ptr<FunctionData> TunnelSelfBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
    PeersSchema(return_types, names);
    auto secret_name = GetTunnelSecretNameFromParams(input);
    auto backend = MeshBackendFromSecret(context, secret_name);
    // self is a single object; wrap into a one-element array for uniform emission.
    auto self = backend->SelfJson();
    auto rows = ParsePeerArray("[" + self + "]");
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

TableFunction CreateTunnelPeersFunction() {
    return MakeMeshTableFunction("tunnel_peers", TunnelPeersBind);
}

TableFunction CreateTunnelSelfFunction() {
    return MakeMeshTableFunction("tunnel_self", TunnelSelfBind);
}

} // namespace duckdb
