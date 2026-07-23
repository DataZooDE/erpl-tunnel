#include "tunnel_secret.hpp"
#include "tunnel_connection.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

SecretMatch LookupTunnelSecret(ClientContext &context, const std::string &secret_name) {
    auto &secret_manager = SecretManager::Get(context);
    auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
    // Try the unified 'tunnel' type first, then the 'ssh_tunnel' alias.
    auto match = secret_manager.LookupSecret(transaction, secret_name, TUNNEL_SECRET_TYPE_ALIAS);
    if (match.HasMatch()) {
        return match;
    }
    return secret_manager.LookupSecret(transaction, secret_name, TUNNEL_SECRET_TYPE_NAME);
}

unique_ptr<BaseSecret> CreateTunnelSecretFunction(ClientContext &context, CreateSecretInput &input) {
    // Create a new tunnel secret carrying the type the user wrote (tunnel|ssh_tunnel).
    vector<string> prefix_paths;
    auto result = make_uniq<KeyValueSecret>(prefix_paths, input.type, TUNNEL_SECRET_PROVIDER, input.name);
    
    // Recognised keys across all backends. Unknown keys are rejected. The 'backend'
    // discriminator selects ssh (default) | tailscale | netbird (ADR-005). The
    // ssh_tunnel secret type is retained as a backward-compatible alias.
    static const std::vector<std::string> kKnownKeys = {
        // discriminator
        "backend",
        // ssh
        "ssh_host", "ssh_port", "ssh_user", "password", "private_key_path",
        "passphrase", "auth_method",
        // mesh (tailscale / netbird)
        "auth_key", "setup_key", "hostname", "tags", "groups", "control_url",
        "management_url", "state_dir", "ephemeral"};

    for (const auto &named_param : input.options) {
        auto lower_name = StringUtil::Lower(named_param.first);
        bool known = false;
        for (const auto &k : kKnownKeys) {
            if (lower_name == k) {
                known = true;
                break;
            }
        }
        if (!known) {
            throw InvalidInputException("Unknown named parameter for tunnel secret: " + lower_name);
        }
        result->secret_map[lower_name] = named_param.second.ToString();
    }

    // Redact every sensitive field across backends (FR-22).
    result->redact_keys = {"password", "passphrase", "private_key_path", "auth_key", "setup_key"};
    return std::move(result);
}

void SetTunnelSecretParameters(CreateSecretFunction &function) {
    function.named_parameters["backend"] = LogicalType::VARCHAR;
    // ssh
    function.named_parameters["ssh_host"] = LogicalType::VARCHAR;
    function.named_parameters["ssh_port"] = LogicalType::INTEGER;
    function.named_parameters["ssh_user"] = LogicalType::VARCHAR;
    function.named_parameters["password"] = LogicalType::VARCHAR;
    function.named_parameters["private_key_path"] = LogicalType::VARCHAR;
    function.named_parameters["passphrase"] = LogicalType::VARCHAR;
    function.named_parameters["auth_method"] = LogicalType::VARCHAR;
    // mesh (tailscale / netbird)
    function.named_parameters["auth_key"] = LogicalType::VARCHAR;
    function.named_parameters["setup_key"] = LogicalType::VARCHAR;
    function.named_parameters["hostname"] = LogicalType::VARCHAR;
    function.named_parameters["tags"] = LogicalType::VARCHAR;
    function.named_parameters["groups"] = LogicalType::VARCHAR;
    function.named_parameters["control_url"] = LogicalType::VARCHAR;
    function.named_parameters["management_url"] = LogicalType::VARCHAR;
    function.named_parameters["state_dir"] = LogicalType::VARCHAR;
    function.named_parameters["ephemeral"] = LogicalType::BOOLEAN;
}

void RegisterTunnelSecretType(ExtensionLoader &loader) {
    // Register the unified tunnel secret type plus the ssh_tunnel alias (ADR-005).
    for (const char *type_name : {TUNNEL_SECRET_TYPE_NAME, TUNNEL_SECRET_TYPE_ALIAS}) {
        duckdb::SecretType tunnel_secret_type;
        tunnel_secret_type.name = type_name;
        tunnel_secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
        tunnel_secret_type.default_provider = TUNNEL_SECRET_PROVIDER;
        loader.RegisterSecretType(tunnel_secret_type);

        CreateSecretFunction tunnel_secret_function = {type_name, TUNNEL_SECRET_PROVIDER,
                                                       CreateTunnelSecretFunction};
        SetTunnelSecretParameters(tunnel_secret_function);
        loader.RegisterFunction(tunnel_secret_function);
    }
}

TunnelAuthParams ConvertTunnelSecretToAuthParams(const KeyValueSecret &duck_secret) {
    TunnelAuthParams auth_params;
    
    // Extract SSH host
    auto ssh_host_val = duck_secret.TryGetValue("ssh_host");
    if (!ssh_host_val.IsNull()) {
        auth_params.ssh_host = ssh_host_val.ToString();
    }
    
    // Extract SSH port
    auto ssh_port_val = duck_secret.TryGetValue("ssh_port");
    if (!ssh_port_val.IsNull()) {
        try {
            auth_params.ssh_port = std::stoi(ssh_port_val.ToString());
        } catch (const std::exception &) {
            auth_params.ssh_port = 22; // Default SSH port
        }
    } else {
        auth_params.ssh_port = 22; // Default SSH port
    }
    
    // Extract SSH user
    auto ssh_user_val = duck_secret.TryGetValue("ssh_user");
    if (!ssh_user_val.IsNull()) {
        auth_params.ssh_user = ssh_user_val.ToString();
    }
    
    // Extract password
    auto password_val = duck_secret.TryGetValue("password");
    if (!password_val.IsNull()) {
        auth_params.password = password_val.ToString();
    }
    
    // Extract private key path
    auto private_key_path_val = duck_secret.TryGetValue("private_key_path");
    if (!private_key_path_val.IsNull()) {
        auth_params.private_key_path = private_key_path_val.ToString();
    }
    
    // Extract passphrase
    auto passphrase_val = duck_secret.TryGetValue("passphrase");
    if (!passphrase_val.IsNull()) {
        auth_params.passphrase = passphrase_val.ToString();
    }
    
    // Extract auth method
    auto auth_method_val = duck_secret.TryGetValue("auth_method");
    if (!auth_method_val.IsNull()) {
        auth_params.auth_method = auth_method_val.ToString();
    } else {
        // Determine auth method based on available credentials
        if (!auth_params.password.empty()) {
            auth_params.auth_method = "password";
        } else if (!auth_params.private_key_path.empty()) {
            auth_params.auth_method = "key";
        } else {
            auth_params.auth_method = "agent";
        }
    }
    
    return auth_params;
}

// ------------------------------------------------------------------------------------------------

std::string GetTunnelSecretNameFromParams(const TableFunctionBindInput &input) {
    return GetTunnelSecretNameFromParams(input.named_parameters);
}

std::string GetTunnelSecretNameFromParams(const FunctionParameters &parameters) {
    return GetTunnelSecretNameFromParams(parameters.named_parameters);
}

std::string GetTunnelSecretNameFromParams(const named_parameter_map_t &named_params) {
    if (named_params.find("secret") != named_params.end()) {
        return named_params.at("secret").ToString();
    } else {
        return std::string();
    }
}

TunnelAuthParams GetTunnelAuthParamsFromContext(ClientContext &context, const TableFunctionBindInput &parameters) {
    auto secret_name = GetTunnelSecretNameFromParams(parameters);
    return GetTunnelAuthParamsFromContext(context, secret_name);
}

TunnelAuthParams GetTunnelAuthParamsFromContext(ClientContext &context, const FunctionParameters &parameters) {
    auto secret_name = GetTunnelSecretNameFromParams(parameters);
    return GetTunnelAuthParamsFromContext(context, secret_name);
}

TunnelAuthParams GetTunnelAuthParamsFromContext(ClientContext &context, const std::string &secret_name) {
    TunnelAuthParams auth_params;
    
    if (!secret_name.empty()) {
        // Use secret for connection
        auth_params = TunnelAuthParams::FromContext(context, secret_name);
    } else {
        // Use context settings for connection
        auth_params = TunnelAuthParams::FromContext(context);
    }
    
    return auth_params;
}

} // namespace duckdb 