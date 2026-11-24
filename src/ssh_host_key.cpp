#include "ssh_host_key.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"

#include <cctype>

namespace duckdb {

namespace {

constexpr const char *kPrefix = "SHA256:";
constexpr size_t kPrefixLen = 7;
// 32 bytes of SHA256 = 43 base64 characters without padding.
constexpr size_t kSha256Len = 32;
constexpr size_t kBase64Len = 43;
constexpr const char *kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string HowToObtain() {
    return "Obtain it from a trusted machine with: ssh-keyscan -p <port> <host> 2>/dev/null | ssh-keygen -lf -";
}

bool IsBase64Char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/';
}

// Unpadded standard base64, the encoding ssh-keygen uses for SHA256 fingerprints.
std::string Base64NoPad(const unsigned char *data, size_t len) {
    std::string out;
    out.reserve((len * 4 + 2) / 3);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kBase64Alphabet[(v >> 18) & 63];
        out += kBase64Alphabet[(v >> 12) & 63];
        out += kBase64Alphabet[(v >> 6) & 63];
        out += kBase64Alphabet[v & 63];
    }
    if (len - i == 1) {
        const unsigned v = data[i] << 16;
        out += kBase64Alphabet[(v >> 18) & 63];
        out += kBase64Alphabet[(v >> 12) & 63];
    } else if (len - i == 2) {
        const unsigned v = (data[i] << 16) | (data[i + 1] << 8);
        out += kBase64Alphabet[(v >> 18) & 63];
        out += kBase64Alphabet[(v >> 12) & 63];
        out += kBase64Alphabet[(v >> 6) & 63];
    }
    return out;
}

} // namespace

std::string NormalizeHostKeyFingerprint(const std::string &value) {
    // Surrounding whitespace is a copy/paste artefact, never part of a fingerprint.
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
        begin++;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        end--;
    }
    std::string body = value.substr(begin, end - begin);

    if (body.size() >= kPrefixLen) {
        std::string head = body.substr(0, kPrefixLen);
        for (auto &c : head) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (head == kPrefix) {
            body = body.substr(kPrefixLen);
        }
    }
    // Tools other than ssh-keygen print the padded form; one '=' is all SHA256 has.
    if (body.size() == kBase64Len + 1 && body.back() == '=') {
        body.pop_back();
    }

    bool valid = body.size() == kBase64Len;
    for (size_t i = 0; valid && i < body.size(); i++) {
        valid = IsBase64Char(body[i]);
    }
    // The last character carries only 4 significant bits of the 32-byte digest; the
    // low 2 bits must be zero, so only every 4th alphabet letter can end a real one.
    if (valid) {
        const char *pos = std::char_traits<char>::find(kBase64Alphabet, 64, body.back());
        valid = pos != nullptr && ((pos - kBase64Alphabet) & 3) == 0;
    }
    if (!valid) {
        throw InvalidInputException(
            "Invalid host_key_fingerprint '" + value +
            "'. Expected the server's SHA256 host-key fingerprint exactly as `ssh-keygen -lf` prints it: "
            "'SHA256:' followed by 43 base64 characters, e.g. "
            "'SHA256:ATJVJJo261pwXqvVsiQkDjp1txG4h5V/rAtC0Q9nQo8' (MD5 fingerprints are not accepted). " +
            HowToObtain());
    }
    return std::string(kPrefix) + body;
}

std::string ComputeHostKeyFingerprint(LIBSSH2_SESSION *session) {
    if (session == nullptr) {
        return "";
    }
    const char *hash = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (hash == nullptr) {
        return "";
    }
    return std::string(kPrefix) + Base64NoPad(reinterpret_cast<const unsigned char *>(hash), kSha256Len);
}

std::string HostKeyTypeLabel(LIBSSH2_SESSION *session) {
    if (session == nullptr) {
        return "unknown";
    }
    size_t len = 0;
    int type = LIBSSH2_HOSTKEY_TYPE_UNKNOWN;
    if (libssh2_session_hostkey(session, &len, &type) == nullptr) {
        return "unknown";
    }
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:
        return "RSA";
    case LIBSSH2_HOSTKEY_TYPE_DSS:
        return "DSA";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
        return "ECDSA";
    case LIBSSH2_HOSTKEY_TYPE_ED25519:
        return "ED25519";
    default:
        return "unknown";
    }
}

std::string VerifyHostKey(LIBSSH2_SESSION *session, const std::string &expected, const std::string &host,
                          int port, const std::string &context_label) {
    const std::string actual = ComputeHostKeyFingerprint(session);
    if (expected.empty()) {
        return actual;
    }
    const std::string where = host + ":" + std::to_string(port);
    if (actual.empty()) {
        throw IOException(context_label + ": host key verification FAILED for " + where +
                          ": could not compute the SHA256 fingerprint of the server's host key, so it "
                          "cannot be checked against host_key_fingerprint. The connection was closed before "
                          "authenticating, so no password or key was sent.");
    }
    if (actual != expected) {
        throw IOException(context_label + ": host key verification FAILED for " + where + ": expected " + expected +
                          ", but the server presented " + actual + " (" + HostKeyTypeLabel(session) +
                          "). The connection was closed before authenticating, so no password or key was sent. "
                          "Someone may be intercepting the connection, or the server's host key changed. Confirm "
                          "the new key with the server's administrator before updating host_key_fingerprint "
                          "on the secret.");
    }
    return actual;
}

std::string HostKeyNotVerifiedWarning(const std::string &host, int port, const std::string &actual_fingerprint,
                                      const std::string &key_type) {
    const std::string port_str = std::to_string(port);
    return "WARNING: the SSH host key of " + host + ":" + port_str +
           " was NOT verified, because the tunnel secret has no host_key_fingerprint. The server presented " +
           actual_fingerprint + " (" + key_type + "). Confirm it out-of-band (ssh-keyscan -p " + port_str + " " +
           host + " 2>/dev/null | ssh-keygen -lf -), then pin it on the secret: host_key_fingerprint '" +
           actual_fingerprint + "'.";
}

std::string ReportHostKeyStatus(ClientContext &context, const TunnelConnectionAttributes &attrs) {
    if (attrs.backend != "ssh" || attrs.host_key_verified || attrs.host_key_fingerprint.empty()) {
        return "";
    }
    const auto warning =
        HostKeyNotVerifiedWarning(attrs.ssh_host, attrs.ssh_port, attrs.host_key_fingerprint, attrs.host_key_type);
    // Once per tunnel, at creation. The result row carries it for every client; the
    // log entry reaches those who read DuckDB's log (the 2.0 CLI echoes warnings).
    // No extra stderr print: in the CLI it would show the same text a third time.
    DUCKDB_LOG_WARNING(context, "erpl_tunnel " + warning);
    return warning;
}

} // namespace duckdb
