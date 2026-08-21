#define DUCKDB_EXTENSION_MAIN

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "tunnel_manager.hpp"
#include "tunnel_secret.hpp"
#include "erpl_tunnel_extension.hpp"
#include "pragma_tunnel_create.hpp"
#include "pragma_tunnel_export.hpp"
#include "pragma_tunnel_close.hpp"
#include "pragma_tunnel_close_all.hpp"
#include "scanner_tunnels.hpp"
#ifdef ERPL_TUNNEL_HAS_MESH
#include "mesh_backend.hpp"
#endif
#include "telemetry.hpp"
#include "erpl_tunnel_banner.hpp"

// Needed for OPENSSL_init_ssl / OPENSSL_INIT_NO_ATEXIT
#include <openssl/ssl.h>

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

// Windows headers may redefine macros after our tracing.hpp include
#ifdef _WIN32
#ifdef DEBUG
    #undef DEBUG
#endif
#ifdef INFO
    #undef INFO
#endif
#ifdef WARN
    #undef WARN
#endif
#ifdef ERROR
    #undef ERROR
#endif
#ifdef TRACE
    #undef TRACE
#endif
#ifdef NONE
    #undef NONE
#endif
#ifdef CONSOLE
    #undef CONSOLE
#endif
#endif

namespace duckdb {

// Global tunnel manager instance
std::unique_ptr<TunnelManager> g_tunnel_manager;

// CalVer version, matching the erpl / erpl-web scheme: git tags are vYYYY.MM.DD
// (with an optional .N for same-day re-releases) and the same YYYY.MM.DD string is
// stamped on telemetry (SetProduct + CaptureExtensionLoad). Bump on release.
static constexpr const char *ERPL_TUNNEL_VERSION = "2026.08.07";

} // namespace duckdb

// Deliberately outside namespace duckdb: the banner library is DuckDB-agnostic
// (the same header serves erpl-adt and flapi), and keeping the object in the
// global namespace lets guarded translation units refer to it without dragging
// duckdb:: into the banner's own template machinery.
const datazoo::BannerInfo ERPL_TUNNEL_BANNER {"erpl_tunnel", duckdb::ERPL_TUNNEL_VERSION,
                                              "https://github.com/DataZooDE/erpl-tunnel"};

namespace duckdb {

static void OnTelemetryEnabled(ClientContext &context, SetScope scope, Value &parameter)
{
    PostHogTelemetry::Instance().SetEnabled(parameter.GetValue<bool>());
}

static void OnAPIKey(ClientContext &context, SetScope scope, Value &parameter)
{
    PostHogTelemetry::Instance().SetAPIKey(parameter.GetValue<string>());
}

static void RegisterConfiguration(ExtensionLoader &loader)
{
    auto &instance = loader.GetDatabaseInstance();
    auto &config = DBConfig::GetConfig(instance);
    config.AddExtensionOption("erpl_telemetry_enabled", "Enable ERPL telemetry, see https://erpl.io/telemetry for details.", 
                              LogicalType::BOOLEAN, Value(true), OnTelemetryEnabled);
    config.AddExtensionOption("erpl_telemetry_key", "Telemetry key, see https://erpl.io/telemetry for details.", LogicalType::VARCHAR,
                              Value("phc_t3wwRLtpyEmLHYaZCSszG0MqVr74J6wnCrj9D41zk2t"), OnAPIKey);
    datazoo::RegisterBannerOption(loader);
}

// Registers a pragma, replacing any entry that already holds the name.
//
// ExtensionLoader::RegisterFunction(PragmaFunction) leaves CreateInfo::on_conflict at its
// ERROR_ON_CONFLICT default, so a name already in the catalog aborts the whole LOAD with
// "Pragma Function with name ... already exists!".
//
// That matters during the migration off erpl's bundled tunnel. erpl >= the release that
// dropped it registers deprecation stubs under these same names, pointing callers here. A
// user who does `LOAD erpl` and then follows that message with `LOAD erpl_tunnel` would
// otherwise hit the abort -- the migration path the message itself recommends. Replacing
// is correct in that situation and harmless otherwise: this extension is the owner of
// these names.
static void RegisterPragmaReplacing(ExtensionLoader &loader, PragmaFunction pragma) {
    auto name = pragma.name;
    PragmaFunctionSet set(name);
    set.AddFunction(std::move(pragma));

    CreatePragmaFunctionInfo info(std::move(name), std::move(set));
    info.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;

    auto &db = loader.GetDatabaseInstance();
    auto &system_catalog = Catalog::GetSystemCatalog(db);
    auto transaction = CatalogTransaction::GetSystemTransaction(db);
    system_catalog.CreatePragmaFunction(transaction, info);
}

static void RegisterTunnelFunctions(ExtensionLoader &loader) {
    // Register tunnel secret type
    RegisterTunnelSecretType(loader);

    // Initialize tunnel manager
    g_tunnel_manager = std::make_unique<TunnelManager>();

    // Register pragma functions
    RegisterPragmaReplacing(loader, CreateTunnelImportPragma());
    RegisterPragmaReplacing(loader, CreateTunnelCreatePragma()); // deprecated alias
    RegisterPragmaReplacing(loader, CreateTunnelExportPragma());
    RegisterPragmaReplacing(loader, CreateTunnelClosePragma());
    RegisterPragmaReplacing(loader, CreateTunnelCloseAllPragma());
#ifdef ERPL_TUNNEL_HAS_MESH
    RegisterPragmaReplacing(loader, CreateMeshActivatePragma());
#endif

    {
        CreateTableFunctionInfo info(CreateTunnelsTableFunction());
        FunctionDescription desc;
        desc.description = "List all active tunnels (SSH and mesh) with their backend, connection details, and status.";
        desc.examples    = {"SELECT * FROM tunnels()"};
        desc.categories  = {"tunnel"};
        info.descriptions.push_back(std::move(desc));
        // Same reason as RegisterPragmaReplacing: this overload keeps CreateInfo's
        // ERROR_ON_CONFLICT default, so a deprecation stub left by an older erpl would
        // abort the LOAD instead of being replaced.
        info.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
        loader.RegisterFunction(std::move(info));
    }

#ifdef ERPL_TUNNEL_HAS_MESH
    // Mesh discovery (Tailscale/NetBird): peer-local enumeration, no control-plane
    // token. These trigger the lazy mesh-shim dlopen on first use for a given secret.
    // Only present on builds that bundle a mesh backend (glibc Linux + macOS).
    {
        CreateTableFunctionInfo info(CreateTunnelPeersFunction());
        FunctionDescription desc;
        desc.description = "Enumerate mesh peers for a tunnel secret (peer-local, no API token).";
        desc.examples    = {"SELECT * FROM tunnel_peers(secret = 'ts')"};
        desc.categories  = {"tunnel", "mesh"};
        info.descriptions.push_back(std::move(desc));
        // Same reason as RegisterPragmaReplacing: this overload keeps CreateInfo's
        // ERROR_ON_CONFLICT default, so a deprecation stub left by an older erpl would
        // abort the LOAD instead of being replaced.
        info.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
        loader.RegisterFunction(std::move(info));
    }
    {
        CreateTableFunctionInfo info(CreateTunnelSelfFunction());
        FunctionDescription desc;
        desc.description = "Show this node's own mesh identity (name/ip/tags) for a tunnel secret.";
        desc.examples    = {"SELECT * FROM tunnel_self(secret = 'ts')"};
        desc.categories  = {"tunnel", "mesh"};
        info.descriptions.push_back(std::move(desc));
        // Same reason as RegisterPragmaReplacing: this overload keeps CreateInfo's
        // ERROR_ON_CONFLICT default, so a deprecation stub left by an older erpl would
        // abort the LOAD instead of being replaced.
        info.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
        loader.RegisterFunction(std::move(info));
    }
#endif
}


static void LoadInternal(ExtensionLoader &loader)
{
    // Pin this DSO so DuckDB's dlclose between connections cannot unmap it while
    // the PostHog background telemetry thread is still executing in its pages.
#if defined(__linux__) || defined(__APPLE__)
    {
        static bool pinned = false;
        if (!pinned) {
            Dl_info info;
            if (dladdr((void*)LoadInternal, &info) && info.dli_fname) {
                pinned = (dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE) != nullptr);
            }
        }
    }
#endif

    // Suppress OpenSSL's atexit cleanup handler — see erpl_rfc_extension.cpp for full rationale.
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    OPENSSL_init_ssl(OPENSSL_INIT_NO_ATEXIT, nullptr);
#endif

    loader.SetDescription("Reach any TCP service from DuckDB through an SSH bastion, a Tailscale tailnet, or a NetBird network — one localhost:PORT, no daemon, no root.");

    // Anonymous, opt-out telemetry (SET erpl_telemetry_enabled=false to disable).
    // Standalone: use the shared posthog-telemetry library directly (no erpl_rfc
    // coupling / ../rfc/src/include), matching the erpl_idoc standalone pattern.
    PostHogTelemetry::Instance().SetProduct("erpl_tunnel", ERPL_TUNNEL_VERSION, "oss");
    PostHogTelemetry::Instance().AssociateGroup("deployment", PostHogTelemetry::GetDistinctId());
    PostHogTelemetry::Instance().CaptureExtensionLoad("erpl_tunnel", ERPL_TUNNEL_VERSION);

    RegisterConfiguration(loader);
    RegisterTunnelFunctions(loader);

    // Last, so a load that fails earlier never advertises itself. Silent unless
    // stderr is a terminal and the ~/.duckdb stamp is over a day old, so piped
    // CLI output, notebooks, CI and the test suite see nothing.
    datazoo::ShowBanner(ERPL_TUNNEL_BANNER);
}

void ErplTunnelExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
}

std::string ErplTunnelExtension::Name() {
    return "erpl_tunnel";
}

} // namespace duckdb

extern "C" {
    DUCKDB_CPP_EXTENSION_ENTRY(erpl_tunnel, loader) {
        duckdb::LoadInternal(loader);
    }
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
