#include "mesh_peers_json.hpp"

#include "yyjson.hpp"

using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

namespace {
std::string JsonStr(yyjson_val *obj, const char *key) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    return (v && yyjson_is_str(v)) ? std::string(yyjson_get_str(v)) : std::string();
}
} // namespace

std::vector<MeshPeer> ParseMeshPeersJson(const std::string &json) {
    std::vector<MeshPeer> rows;
    if (json.empty()) {
        return rows;
    }
    yyjson_doc *doc = yyjson_read(json.c_str(), json.size(), 0);
    if (!doc) {
        return rows; // malformed → empty (untrusted input, defensive)
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root && yyjson_is_arr(root)) {
        size_t idx, max;
        yyjson_val *el;
        yyjson_arr_foreach(root, idx, max, el) {
            if (!yyjson_is_obj(el)) {
                continue; // skip non-object elements
            }
            MeshPeer r;
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

} // namespace duckdb
