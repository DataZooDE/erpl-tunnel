#pragma once

#include "libssh2.h"
#include "tunnel_connection.hpp"

#include <string>

namespace duckdb {

// SSH server host-key verification (issue #16).
//
// A tunnel secret may carry `host_key_fingerprint`, the server's SHA256 host-key
// fingerprint exactly as `ssh-keygen -lf` prints it ("SHA256:" + unpadded base64).
// It is checked right after the SSH handshake and BEFORE any authentication, so a
// mismatch never sends a password or offers a key to whoever answered. Without it
// the tunnel still connects, but reports a warning that carries the server's real
// fingerprint and how to pin it. There is deliberately no trust-on-first-use and
// nothing is written to known_hosts.

// Name of the secret field.
static constexpr const char *kHostKeyFingerprintKey = "host_key_fingerprint";

// Validate a user-supplied fingerprint and return it in canonical form
// ("SHA256:<43 base64 chars>"). Accepts the bare base64 part, a case-insensitive
// "SHA256:" prefix and a trailing '=' pad. Throws InvalidInputException with an
// actionable message on anything else (MD5 fingerprints, wrong length, ...).
std::string NormalizeHostKeyFingerprint(const std::string &value);

// The SHA256 fingerprint of the host key the server presented in the completed
// handshake on `session`, in canonical form. Empty if libssh2 cannot provide it.
std::string ComputeHostKeyFingerprint(LIBSSH2_SESSION *session);

// Short key type label as `ssh-keygen -lf` prints it: "ED25519", "ECDSA", "RSA", ...
std::string HostKeyTypeLabel(LIBSSH2_SESSION *session);

// Check the presented host key against `expected` (canonical form, or empty for
// "not pinned"). Returns the actual fingerprint. Throws IOException naming both
// fingerprints when `expected` is set and does not match, or when the server's
// fingerprint cannot be computed at all while one is expected. The caller must
// tear the session down on throw; it must not authenticate.
std::string VerifyHostKey(LIBSSH2_SESSION *session, const std::string &expected, const std::string &host,
                          int port, const std::string &context_label);

// The warning reported when a tunnel connected without a pinned host key.
std::string HostKeyNotVerifiedWarning(const std::string &host, int port, const std::string &actual_fingerprint,
                                      const std::string &key_type);

// Report a freshly created SSH tunnel's host-key status once: if the key was not
// verified, write the warning to DuckDB's log (level WARNING) and return it, so
// the pragma puts it in its result row. Returns "" when the
// key was verified or the tunnel is not SSH.
std::string ReportHostKeyStatus(ClientContext &context, const TunnelConnectionAttributes &attrs);

} // namespace duckdb
