// Standalone unit tests (Catch2) for the pure, DuckDB-free peer-status JSON parser.
// Compiled without DuckDB — just mesh_peers_json.cpp + duckdb's third_party yyjson —
// so it runs in seconds (see the `core_tests` Make target). The parser handles
// untrusted input from the Go shim, so the emphasis is on defensive/edge cases.
#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "mesh_peers_json.hpp"

using namespace duckdb;

TEST_CASE("parses a well-formed peer array") {
    const std::string json = R"([
        {"backend":"tailscale","host_name":"node-a","dns_name":"node-a.ts.net",
         "mesh_ip":"100.64.0.1","tags":["tag:duckdb","tag:eu"],"online":true},
        {"backend":"tailscale","host_name":"node-b","dns_name":"node-b.ts.net",
         "mesh_ip":"100.64.0.2","tags":[],"online":false}
    ])";
    auto rows = ParseMeshPeersJson(json);
    REQUIRE(rows.size() == 2);

    CHECK(rows[0].backend == "tailscale");
    CHECK(rows[0].host_name == "node-a");
    CHECK(rows[0].dns_name == "node-a.ts.net");
    CHECK(rows[0].mesh_ip == "100.64.0.1");
    CHECK(rows[0].online == true);
    REQUIRE(rows[0].tags.size() == 2);
    CHECK(rows[0].tags[0] == "tag:duckdb");
    CHECK(rows[0].tags[1] == "tag:eu");

    CHECK(rows[1].host_name == "node-b");
    CHECK(rows[1].online == false);
    CHECK(rows[1].tags.empty());
}

TEST_CASE("empty array and empty string yield no rows") {
    CHECK(ParseMeshPeersJson("[]").empty());
    CHECK(ParseMeshPeersJson("").empty());
}

TEST_CASE("malformed JSON yields no rows (defensive, untrusted input)") {
    CHECK(ParseMeshPeersJson("not json").empty());
    CHECK(ParseMeshPeersJson("{").empty());
    CHECK(ParseMeshPeersJson("[ {").empty());
}

TEST_CASE("a JSON object (not array) at the root yields no rows") {
    // mesh_self_json returns an object; callers wrap it in [..] before parsing.
    CHECK(ParseMeshPeersJson(R"({"backend":"netbird"})").empty());
}

TEST_CASE("missing fields default; non-object elements are skipped") {
    const std::string json = R"([
        {"backend":"netbird"},
        "a string element",
        42,
        {"host_name":"only-host","online":true}
    ])";
    auto rows = ParseMeshPeersJson(json);
    REQUIRE(rows.size() == 2); // the two objects; the string and number are skipped
    CHECK(rows[0].backend == "netbird");
    CHECK(rows[0].host_name.empty());
    CHECK(rows[0].mesh_ip.empty());
    CHECK(rows[0].online == false);
    CHECK(rows[1].host_name == "only-host");
    CHECK(rows[1].online == true);
    CHECK(rows[1].backend.empty());
}

TEST_CASE("non-string tags are ignored, string tags kept") {
    const std::string json = R"([{"host_name":"n","tags":["ok",123,null,"also-ok"]}])";
    auto rows = ParseMeshPeersJson(json);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].tags.size() == 2);
    CHECK(rows[0].tags[0] == "ok");
    CHECK(rows[0].tags[1] == "also-ok");
}

TEST_CASE("wrong-typed fields are treated as absent, not a crash") {
    // online as a string, mesh_ip as a number — must not throw; fields default.
    const std::string json = R"([{"online":"yes","mesh_ip":100,"host_name":"n"}])";
    auto rows = ParseMeshPeersJson(json);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].online == false);   // "yes" is not JSON true
    CHECK(rows[0].mesh_ip.empty());   // number, not string → absent
    CHECK(rows[0].host_name == "n");
}
