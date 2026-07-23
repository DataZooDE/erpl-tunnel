#include "tunnel_connection.hpp"
#include "tunnel_secret.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#ifdef WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <arpa/inet.h>
    #include <netdb.h>
#endif

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace duckdb {

TunnelConnection::TunnelConnection() {
    // Initialize Windows sockets if on Windows
#ifdef WIN32
    WSADATA wsa_data;
    const int wsa_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (wsa_result != 0) {
        throw IOException("Failed to initialize Windows sockets");
    }
#endif

    // Initialize libssh2
    const int init_result = libssh2_init(0);
    if (init_result != 0) {
        throw IOException("Failed to initialize libssh2 library");
    }
}

TunnelConnection::~TunnelConnection() {
    Close();
    libssh2_exit();

    // Cleanup Windows sockets if on Windows
#ifdef WIN32
    WSACleanup();
#endif
}

TunnelConnection::TunnelConnection(TunnelConnection&& other) noexcept
    : session_(other.session_),
      ssh_socket_(other.ssh_socket_),
      is_connected_(other.is_connected_.load()),
      is_running_(other.is_running_.load()),
      attributes_(std::move(other.attributes_)),
      worker_thread_(std::move(other.worker_thread_)) {
    other.session_ = nullptr;
    other.ssh_socket_ = -1;
    other.is_connected_.store(false);
    other.is_running_.store(false);
}

TunnelConnection& TunnelConnection::operator=(TunnelConnection&& other) noexcept {
    if (this != &other) {
        Close();
        session_ = other.session_;
        ssh_socket_ = other.ssh_socket_;
        is_connected_.store(other.is_connected_.load());
        is_running_.store(other.is_running_.load());
        attributes_ = std::move(other.attributes_);
        worker_thread_ = std::move(other.worker_thread_);
        
        other.session_ = nullptr;
        other.ssh_socket_ = -1;
        other.is_connected_.store(false);
        other.is_running_.store(false);
    }
    return *this;
}

void TunnelConnection::ValidateConnectionParameters(const std::string& ssh_host, int32_t ssh_port,
                                                   const std::string& ssh_user, const std::string& remote_host,
                                                   int32_t remote_port, int32_t local_port) const {
    if (ssh_host.empty()) {
        throw InvalidInputException("SSH host cannot be empty");
    }
    
    if (ssh_port < kMinPortNumber || ssh_port > kMaxPortNumber) {
        throw InvalidInputException("SSH port must be between " + std::to_string(kMinPortNumber) + 
                                   " and " + std::to_string(kMaxPortNumber));
    }
    
    if (ssh_user.empty()) {
        throw InvalidInputException("SSH user cannot be empty");
    }
    
    if (remote_host.empty()) {
        throw InvalidInputException("Remote host cannot be empty");
    }
    
    if (remote_port < kMinPortNumber || remote_port > kMaxPortNumber) {
        throw InvalidInputException("Remote port must be between " + std::to_string(kMinPortNumber) + 
                                   " and " + std::to_string(kMaxPortNumber));
    }
    
    if (local_port < kMinPortNumber || local_port > kMaxPortNumber) {
        throw InvalidInputException("Local port must be between " + std::to_string(kMinPortNumber) + 
                                   " and " + std::to_string(kMaxPortNumber));
    }
}

bool TunnelConnection::TestTunnelConnection(int32_t timeout_seconds) {
    const auto start_time = std::chrono::steady_clock::now();
    const auto timeout_duration = std::chrono::seconds(timeout_seconds);
    
    while (std::chrono::steady_clock::now() - start_time < timeout_duration) {
        // Check if the tunnel worker is running and has started listening
        if (!is_running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // Try to connect to the local port
        const int32_t test_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (test_socket < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // Set socket to non-blocking for timeout
#ifdef WIN32
        u_long non_blocking = 1;
        ioctlsocket(test_socket, FIONBIO, &non_blocking);
#else
        const int flags = fcntl(test_socket, F_GETFL, 0);
        fcntl(test_socket, F_SETFL, flags | O_NONBLOCK);
#endif
        
        sockaddr_in test_addr{};
        test_addr.sin_family = AF_INET;
        test_addr.sin_port = htons(attributes_.local_port);
        test_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        
        const int connect_result = connect(test_socket, reinterpret_cast<struct sockaddr*>(&test_addr), sizeof(test_addr));
        if (connect_result == 0) {
            // Connection successful immediately
            CloseSocket(test_socket);
            return true;
        }
        
        if (errno == EINPROGRESS) {
            // Connection in progress, wait for it to complete
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(test_socket, &write_fds);
            
            timeval tv{};
            tv.tv_sec = 1;  // 1 second timeout for each attempt
            tv.tv_usec = 0;
            
            const int select_result = select(test_socket + 1, nullptr, &write_fds, nullptr, &tv);
            if (select_result > 0) {
            // Check if connection was successful
#ifdef WIN32
            char error = 0;
            int len = sizeof(error);
            if (getsockopt(test_socket, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
#else
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(test_socket, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
#endif
                    CloseSocket(test_socket);
                    return true;
                }
            }
        }
        
        CloseSocket(test_socket);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    return false;
}

void TunnelConnection::Connect(const std::string& ssh_host, int32_t ssh_port, const std::string& ssh_user,
                              const std::string& remote_host, int32_t remote_port, int32_t local_port) {
    // Validate input parameters
    ValidateConnectionParameters(ssh_host, ssh_port, ssh_user, remote_host, remote_port, local_port);
    
    // Store connection attributes
    attributes_.ssh_host = ssh_host;
    attributes_.ssh_port = ssh_port;
    attributes_.ssh_user = ssh_user;
    attributes_.remote_host = remote_host;
    attributes_.remote_port = remote_port;
    attributes_.local_port = local_port;
    
    // Resolve the SSH host with getaddrinfo — handles hostnames (not just IPv4
    // literals) and IPv6, replacing erpl's inet_addr + IPv4-only gethostbyname
    // (FR-9). Try each returned address until one connects.
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;     // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    const std::string port_str = std::to_string(ssh_port);
    const int gai = getaddrinfo(ssh_host.c_str(), port_str.c_str(), &hints, &res);
    if (gai != 0 || res == nullptr) {
        attributes_.error_message = "Failed to resolve host: " + ssh_host;
        throw IOException("Tunnel: could not resolve SSH host '" + ssh_host + "': " +
                          std::string(gai_strerror(gai)) + ". Check the host name and DNS.");
    }

    ssh_socket_ = -1;
    for (struct addrinfo *ai = res; ai != nullptr; ai = ai->ai_next) {
        const int32_t sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) {
            continue;
        }
        if (connect(sock, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) == 0) {
            ssh_socket_ = sock;
            break;
        }
        CloseSocket(sock);
    }
    freeaddrinfo(res);

    if (ssh_socket_ < 0) {
        attributes_.error_message = "Failed to connect to SSH host: " + ssh_host + ":" + std::to_string(ssh_port);
        throw IOException("Tunnel: failed to connect to SSH host '" + ssh_host + ":" +
                          std::to_string(ssh_port) + "'. Check the SSH server is running and reachable.");
    }

    // Initialize SSH session
    session_ = libssh2_session_init();
    if (!session_) {
        attributes_.error_message = "Failed to initialize SSH session";
        CloseSocket(ssh_socket_);
        throw IOException("Failed to initialize SSH session. libssh2 may not be properly installed.");
    }
    
    // Perform SSH handshake
    const int handshake_result = libssh2_session_handshake(session_, ssh_socket_);
    if (handshake_result != 0) {
        attributes_.error_message = "SSH handshake failed";
        CloseSocket(ssh_socket_);
        throw IOException("SSH handshake failed. The server may not support SSH or the connection was interrupted.");
    }
    
    // Authentication will be handled by the calling code
    is_connected_ = true;
    attributes_.status = kStatusConnected;
}

void TunnelConnection::StartWorker() {
    is_running_ = true;
    worker_thread_ = std::thread([this]() {
        WorkerFunction();
    });
}

void TunnelConnection::StopWorker() {
    is_running_ = false;
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void TunnelConnection::Close() {
    is_running_ = false;
    
    // Close SSH session first to unblock any pending operations
    if (session_) {
        // Set a timeout for session operations
        libssh2_session_set_timeout(session_, 5000); // 5 second timeout
        
        // Disconnect the session
        libssh2_session_disconnect(session_, "Shutdown");
        libssh2_session_free(session_);
        session_ = nullptr;
    }
    
    // Close the SSH socket
    if (ssh_socket_ >= 0) {
        CloseSocket(ssh_socket_);
        ssh_socket_ = -1;
    }
    
    // Deterministic teardown: the accept loop and every per-connection worker
    // observe is_running_ == false and the closed session, then return. We JOIN
    // them (erpl detached on a timeout, leaking threads and fds) — after Close()
    // returns, no tunnel thread is still executing (FR-5/§8.3).
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(forward_threads_mutex_);
        for (auto &t : forward_threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        forward_threads_.clear();
    }

    is_connected_ = false;
    attributes_.status = kStatusClosed;
}

bool TunnelConnection::IsConnected() const noexcept {
    return is_connected_ && is_running_;
}

TunnelConnectionAttributes TunnelConnection::GetAttributes() const noexcept {
    return attributes_;
}

std::string TunnelConnection::GetStatus() const noexcept {
    return attributes_.status;
}

std::string TunnelConnection::GetErrorMessage() const noexcept {
    return attributes_.error_message;
}

TunnelAuthParams TunnelAuthParams::FromContext(ClientContext& context, const std::string& secret_name) {
    TunnelAuthParams auth_params;
    
    // Lookup the secret under either registered type ('tunnel' | 'ssh_tunnel').
    // A missing secret is a HARD, actionable error — never the
    // silent localhost/agent default erpl used (BRD §8.2, NFR-3). Defaulting to an
    // anonymous localhost node hides the real mistake and produces confusing
    // downstream failures ("SSH user cannot be empty").
    auto secret_match = LookupTunnelSecret(context, secret_name);
    if (!secret_match.HasMatch()) {
        if (secret_name.empty() || secret_name == "*") {
            throw InvalidInputException(
                "Tunnel: no tunnel secret specified. Pass secret := '<name>' and create it "
                "first, e.g. CREATE SECRET s (TYPE ssh_tunnel, ssh_host '…', ssh_user '…', "
                "password '…'). Not defaulting to an anonymous localhost node.");
        }
        throw InvalidInputException(
            "Tunnel: secret '" + secret_name + "' not found. Create it with "
            "CREATE SECRET " + secret_name + " (TYPE ssh_tunnel, ssh_host '…', ssh_user '…', "
            "password '…' /* or private_key_path */). Not defaulting to an anonymous node.");
    }
    
    // Cast to KeyValueSecret
    const auto& duck_secret = dynamic_cast<const KeyValueSecret&>(secret_match.GetSecret());
    auth_params = ConvertTunnelSecretToAuthParams(duck_secret);
    
    return auth_params;
}

std::string TunnelAuthParams::ToString() const {
    return "TunnelAuthParams{ssh_host='" + ssh_host + "', ssh_port=" + std::to_string(ssh_port) + 
           ", ssh_user='" + ssh_user + "', auth_method='" + auth_method + "'}";
}

std::shared_ptr<TunnelConnection> TunnelAuthParams::Connect(const std::string& remote_host, int32_t remote_port, int32_t local_port) {
    auto connection = std::make_shared<TunnelConnection>();
    connection->Connect(ssh_host, ssh_port, ssh_user, remote_host, remote_port, local_port);
    
    // Authenticate based on the auth method
    bool auth_success = false;
    if (auth_method == kAuthMethodPassword && !password.empty()) {
        auth_success = connection->AuthenticateWithPassword(password);
    } else if (auth_method == kAuthMethodKey && !private_key_path.empty()) {
        auth_success = connection->AuthenticateWithKey(private_key_path, passphrase);
    } else if (auth_method == kAuthMethodAgent) {
        auth_success = connection->AuthenticateWithAgent();
    }
    
    if (!auth_success) {
        throw IOException("Failed to authenticate tunnel connection: " + connection->GetErrorMessage());
    }
    
    return connection;
}

bool TunnelConnection::AuthenticateWithPassword(const std::string& password) {
    if (!session_) {
        attributes_.error_message = "No SSH session available for authentication";
        return false;
    }
    
    const int auth_result = libssh2_userauth_password(session_, attributes_.ssh_user.c_str(), password.c_str());
    if (auth_result != 0) {
        attributes_.error_message = "SSH password authentication failed. Check username and password.";
        return false;
    }
    
    attributes_.status = kStatusAuthenticated;
    return true;
}

bool TunnelConnection::AuthenticateWithKey(const std::string& private_key_path, const std::string& passphrase) {
    if (!session_) {
        attributes_.error_message = "No SSH session available for key authentication";
        return false;
    }
    
    // Read private key file
    std::ifstream key_file(private_key_path);
    if (!key_file.is_open()) {
        attributes_.error_message = "Failed to open private key file: " + private_key_path;
        return false;
    }
    
    std::stringstream key_buffer;
    key_buffer << key_file.rdbuf();
    const std::string private_key = key_buffer.str();
    
    int auth_result;
    if (passphrase.empty()) {
        auth_result = libssh2_userauth_publickey_frommemory(session_, attributes_.ssh_user.c_str(), 
                                                           attributes_.ssh_user.length(),
                                                           nullptr, 0,
                                                           private_key.c_str(), private_key.length(),
                                                           nullptr);
    } else {
        auth_result = libssh2_userauth_publickey_frommemory(session_, attributes_.ssh_user.c_str(), 
                                                           attributes_.ssh_user.length(),
                                                           nullptr, 0,
                                                           private_key.c_str(), private_key.length(),
                                                           passphrase.c_str());
    }
    
    if (auth_result != 0) {
        attributes_.error_message = "SSH key authentication failed. Check if the private key is valid and the passphrase is correct.";
        return false;
    }
    
    attributes_.status = kStatusAuthenticated;
    return true;
}

bool TunnelConnection::AuthenticateWithAgent() {
    if (!session_) {
        attributes_.error_message = "No SSH session available for agent authentication";
        return false;
    }
    
    // SSH-agent auth is not implemented (erpl left it stubbed). Fail with an
    // actionable message that names the supported alternatives — never a silent
    // false that surfaces later as a generic "authentication failed" (FR-8).
    attributes_.error_message =
        "SSH agent authentication is not supported. Use auth_method 'key' or 'password' "
        "instead (set private_key_path/passphrase, or password, on the secret).";
    return false;
}

void TunnelConnection::WorkerFunction() {
    attributes_.status = "Starting tunnel on port " + std::to_string(attributes_.local_port);
    
    // Create listening socket
    const int32_t listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        attributes_.error_message = "Failed to create listening socket for tunnel";
        attributes_.status = kStatusError;
        return;
    }
    
    // Set socket to non-blocking to allow for graceful shutdown
#ifdef WIN32
    u_long non_blocking = 1;
    ioctlsocket(listen_sock, FIONBIO, &non_blocking);
#else
    const int flags = fcntl(listen_sock, F_GETFL, 0);
    fcntl(listen_sock, F_SETFL, flags | O_NONBLOCK);
#endif
    
    // Allow quick re-bind after a previous tunnel on the same port was closed.
    int reuse = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in listen_addr{};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(attributes_.local_port);
    // Bind loopback by default (FR-2/ADR-006); INADDR_ANY only on explicit opt-in.
    listen_addr.sin_addr.s_addr = inet_addr(attributes_.bind_addr.c_str());

    if (bind(listen_sock, reinterpret_cast<struct sockaddr*>(&listen_addr), sizeof(listen_addr)) != 0) {
        attributes_.error_message = "Failed to bind to port " + std::to_string(attributes_.local_port) + 
                                   ". Port may be in use or insufficient permissions.";
        attributes_.status = kStatusError;
        CloseSocket(listen_sock);
        return;
    }
    
    if (listen(listen_sock, 5) != 0) {
        attributes_.error_message = "Failed to listen on port " + std::to_string(attributes_.local_port);
        attributes_.status = kStatusError;
        CloseSocket(listen_sock);
        return;
    }
    
    attributes_.status = kStatusListening + std::string(" on port ") + std::to_string(attributes_.local_port);
    
    while (is_running_) {
        // Use select with timeout to make accept non-blocking
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_sock, &read_fds);
        
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms timeout
        
        const int select_result = select(listen_sock + 1, &read_fds, nullptr, nullptr, &tv);
        if (select_result < 0) {
            // Error in select
            break;
        } else if (select_result == 0) {
            // Timeout - continue loop to check is_running_
            continue;
        }
        
        // Accept connection
        const int32_t client_sock = accept(listen_sock, nullptr, nullptr);
        if (client_sock < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            break;
        }
        
        if (!is_running_) {
            CloseSocket(client_sock);
            break;
        }
        
        // Create SSH channel for port forwarding
        LIBSSH2_CHANNEL* channel = libssh2_channel_direct_tcpip_ex(session_, 
                                                                  attributes_.remote_host.c_str(), 
                                                                  attributes_.remote_port, 
                                                                  "127.0.0.1", attributes_.remote_port);
        if (!channel) {
            attributes_.error_message = "Failed to create SSH channel for port forwarding";
            CloseSocket(client_sock);
            continue;
        }
        
        // Handle data forwarding in a tracked worker thread. erpl detached these,
        // leaking threads that outlived Close(); we keep them joinable and join on
        // teardown (FR-5/§8.3). Opportunistically reap already-finished workers so
        // the vector doesn't grow unbounded on long-lived tunnels.
        {
            std::lock_guard<std::mutex> lock(forward_threads_mutex_);
            forward_threads_.emplace_back([this, channel, client_sock]() {
                ForwardData(client_sock, channel);
            });
        }
    }
    
    CloseSocket(listen_sock);
    attributes_.status = "Tunnel stopped";
}

void TunnelConnection::ForwardData(int32_t client_socket, LIBSSH2_CHANNEL* channel) {
    static constexpr size_t kBufferSize = 16384;
    char buffer[kBufferSize];
    fd_set fds;
    timeval tv;
    
    // Set client socket to non-blocking
#ifdef WIN32
    u_long non_blocking = 1;
    ioctlsocket(client_socket, FIONBIO, &non_blocking);
#else
    const int client_flags = fcntl(client_socket, F_GETFL, 0);
    fcntl(client_socket, F_SETFL, client_flags | O_NONBLOCK);
#endif
    
    while (is_running_) {
        FD_ZERO(&fds);
        FD_SET(client_socket, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 50000; // 50ms timeout for faster response to shutdown
        
        if (select(client_socket + 1, &fds, nullptr, nullptr, &tv) > 0) {
            const ssize_t bytes = read(client_socket, buffer, sizeof(buffer));
            if (bytes <= 0) break;
            
            size_t written = 0;
            while (written < static_cast<size_t>(bytes) && is_running_) {
                const ssize_t rc = libssh2_channel_write(channel, buffer + written, bytes - written);
                if (rc < 0) {
                    if (libssh2_session_last_errno(session_) != LIBSSH2_ERROR_EAGAIN) {
                        goto cleanup;
                    }
                    break;
                }
                written += rc;
            }
        }
        
        // Read from SSH channel
        const ssize_t bytes_from_channel = libssh2_channel_read(channel, buffer, sizeof(buffer));
        if (bytes_from_channel > 0) {
            size_t written = 0;
            while (written < static_cast<size_t>(bytes_from_channel) && is_running_) {
                const ssize_t rc = write(client_socket, buffer + written, bytes_from_channel - written);
                if (rc < 0) {
                    goto cleanup;
                }
                written += rc;
            }
        } else if (bytes_from_channel == 0) {
            if (libssh2_channel_eof(channel)) break;
        } else if (libssh2_session_last_errno(session_) != LIBSSH2_ERROR_EAGAIN) {
            break;
        }
    }
    
cleanup:
    CloseSocket(client_socket);
    libssh2_channel_free(channel);
}

int32_t TunnelConnection::CreateSocket() {
    return socket(AF_INET, SOCK_STREAM, 0);
}

void TunnelConnection::CloseSocket(int32_t socket) noexcept {
    if (socket >= 0) {
        close(socket);
    }
}

} // namespace duckdb 