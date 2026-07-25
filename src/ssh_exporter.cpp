#include "ssh_exporter.hpp"

#include "duckdb/common/exception.hpp"

#include <chrono>
#include <cstring>
#include <utility>

namespace duckdb {

namespace {

// Connect a TCP socket to host:port, resolving names and IPv6 via getaddrinfo.
//
// The connect is NON-BLOCKING with an explicit deadline. A blocking connect()
// ignores our timeout entirely — the kernel's own retry schedule can hold a
// blackholed address for minutes — which would mean `tunnel_export(timeout => 5)`
// hanging far longer than asked, and a worker dialling a vanished local service
// stalling Close() while it waits.
socket_t DialTcp(const std::string &host, int port, std::string &err, int timeout_ms) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    const std::string port_str = std::to_string(port);
    const int gai = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (gai != 0 || res == nullptr) {
        err = "could not resolve '" + host + "'";
        return kInvalidSocket;
    }
    socket_t sock = kInvalidSocket;
    bool timed_out = false;
    for (addrinfo *ai = res; ai != nullptr && !SocketValid(sock); ai = ai->ai_next) {
        socket_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (!SocketValid(s)) {
            continue;
        }
        SetSocketNonBlocking(s);
        const int rc = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        if (rc == 0) {
            SetSocketBlocking(s);
            sock = s;
            break;
        }
        if (!SocketConnectInProgress(LastSocketError())) {
            CloseSocketHandle(s);
            continue;
        }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        const int sel = select(SelectNfds(s, s), nullptr, &wfds, nullptr, &tv);
        if (sel <= 0) {
            timed_out = timed_out || (sel == 0);
            CloseSocketHandle(s);
            continue;
        }
        // select() reporting writable is not the same as connected: the error must
        // be read back with SO_ERROR or a refused connection looks like success.
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soerr), &len) != 0 ||
            soerr != 0) {
            CloseSocketHandle(s);
            continue;
        }
        SetSocketBlocking(s);
        sock = s;
    }
    freeaddrinfo(res);
    if (!SocketValid(sock)) {
        err = timed_out ? "timed out connecting to " + host + ":" + std::to_string(port)
                        : "could not connect to " + host + ":" + std::to_string(port);
    }
    return sock;
}

} // namespace

SshExporter::SshExporter(TunnelAuthParams auth, std::string local_host, int local_port,
                         std::string remote_bind_host, int remote_port)
    : auth_(std::move(auth)), remote_bind_host_(std::move(remote_bind_host)),
      requested_remote_port_(remote_port) {
    attrs_.backend = "ssh";
    attrs_.direction = "export";
    attrs_.ssh_host = auth_.ssh_host;
    attrs_.ssh_port = auth_.ssh_port;
    attrs_.ssh_user = auth_.ssh_user;
    attrs_.local_host = std::move(local_host);
    attrs_.local_port = local_port;
    attrs_.remote_host = remote_bind_host_;
    attrs_.remote_port = remote_port;
    attrs_.bind_addr = ""; // an export has no local listener
    attrs_.status = "Configuring";
}

SshExporter::~SshExporter() { Close(); }

void SshExporter::RecordError(const std::string &message) {
    std::lock_guard<std::mutex> lock(mu_);
    attrs_.error_message = message;
}

void SshExporter::Start(int timeout_seconds) {
    EnsureSocketSubsystem();
    const int connect_timeout_ms = timeout_seconds > 0 ? timeout_seconds * 1000 : 30000;

    // Probe the local service FIRST. Failing here says "there is nothing to
    // export" before we involve the bastion at all, which is a far clearer error
    // than a listener that never receives anything useful.
    {
        std::string derr;
        socket_t probe = DialTcp(attrs_.local_host, attrs_.local_port, derr, connect_timeout_ms);
        if (!SocketValid(probe)) {
            throw IOException(
                "tunnel_export: nothing is listening on " + attrs_.local_host + ":" +
                std::to_string(attrs_.local_port) +
                ", so there is nothing to export. Start the local service first, or point "
                "local_host/local_port at it.");
        }
        CloseSocketHandle(probe);
    }

    std::string derr;
    ssh_socket_ = DialTcp(auth_.ssh_host, auth_.ssh_port, derr, connect_timeout_ms);
    if (!SocketValid(ssh_socket_)) {
        throw IOException("tunnel_export: " + derr +
                          ". Check the SSH server is running and reachable.");
    }

    // select() below cannot represent a descriptor at or beyond FD_SETSIZE. Refuse
    // here with something actionable rather than corrupting memory in the accept
    // loop of a process that happens to hold many open files.
    if (!SocketFitsInFdSet(ssh_socket_)) {
        CloseSocketHandle(ssh_socket_);
        ssh_socket_ = kInvalidSocket;
        throw IOException(
            "tunnel_export: this process has too many open files for the SSH export to poll "
            "its connection safely. Close some tunnels or raise the file-descriptor limit.");
    }

    session_ = libssh2_session_init();
    if (session_ == nullptr) {
        CloseSocketHandle(ssh_socket_);
        ssh_socket_ = kInvalidSocket;
        throw IOException("tunnel_export: could not initialise an SSH session.");
    }
    // Blocking with a bound timeout through handshake/auth/listen, so a hung
    // bastion fails inside the pragma rather than in a background thread.
    libssh2_session_set_timeout(session_, timeout_seconds > 0 ? timeout_seconds * 1000 : 30000);

    if (libssh2_session_handshake(session_, ssh_socket_) != 0) {
        Close();
        throw IOException("tunnel_export: SSH handshake with " + auth_.ssh_host + " failed.");
    }

    bool authed = false;
    if (auth_.auth_method == kAuthMethodAgent) {
        Close();
        throw InvalidInputException(
            "Tunnel: SSH agent authentication is not supported. Use auth_method 'key' or "
            "'password' instead (set private_key_path/passphrase, or password, on the secret).");
    }
    if (auth_.auth_method == "key" && !auth_.private_key_path.empty()) {
        authed = libssh2_userauth_publickey_fromfile_ex(
                     session_, auth_.ssh_user.c_str(),
                     static_cast<unsigned int>(auth_.ssh_user.length()), nullptr,
                     auth_.private_key_path.c_str(),
                     auth_.passphrase.empty() ? nullptr : auth_.passphrase.c_str()) == 0;
    } else if (!auth_.password.empty()) {
        authed = libssh2_userauth_password_ex(
                     session_, auth_.ssh_user.c_str(),
                     static_cast<unsigned int>(auth_.ssh_user.length()),
                     auth_.password.c_str(),
                     static_cast<unsigned int>(auth_.password.length()), nullptr) == 0;
    }
    if (!authed) {
        Close();
        throw IOException("tunnel_export: SSH authentication failed for user '" +
                          auth_.ssh_user + "' on " + auth_.ssh_host + ".");
    }

    int bound = 0;
    const char *bind_host = remote_bind_host_.empty() ? nullptr : remote_bind_host_.c_str();
    listener_ = libssh2_channel_forward_listen_ex(session_, bind_host, requested_remote_port_,
                                                  &bound, 16);
    if (listener_ == nullptr) {
        char *msg = nullptr;
        int mlen = 0;
        libssh2_session_last_error(session_, &msg, &mlen, 0);
        const std::string detail = msg != nullptr ? msg : "unknown";
        Close();
        // The server does not tell us WHICH of these it was, so name them all.
        throw IOException(
            "tunnel_export: the SSH server refused to bind remote port " +
            std::to_string(requested_remote_port_) + " (" + detail + "). Common causes: " +
            "(1) sshd has 'AllowTcpForwarding no' or 'local' — it needs 'yes' or 'remote'; " +
            "(2) that port is already in use on " + auth_.ssh_host + "; " +
            "(3) ports below 1024 require root on the server. Choose a free port above 1024, "
            "or pass remote_port=0 to let the server pick one.");
    }
    if (bound == 0) {
        Close();
        // Inbound channels are matched on (host, port); a listener stuck at port 0
        // can never match, so this would hang forever with no diagnostic.
        throw IOException(
            "tunnel_export: the SSH server accepted the forward but did not report the "
            "allocated port, so no connection could ever be routed to it. Pass an explicit "
            "remote_port instead of 0.");
    }

    // An export is idle until someone connects, so without a keepalive a dead
    // session is only discovered at the worst possible moment.
    libssh2_keepalive_config(session_, 1, 30);
    // Non-blocking from here — see the header for why this is load-bearing.
    libssh2_session_set_blocking(session_, 0);
    libssh2_session_set_timeout(session_, 0);

    {
        std::lock_guard<std::mutex> lock(mu_);
        attrs_.remote_port = bound;
        attrs_.status = "Exporting " + attrs_.local_host + ":" +
                        std::to_string(attrs_.local_port) + " on " + auth_.ssh_host + ":" +
                        std::to_string(bound);
    }
    running_.store(true);
    accept_thread_ = std::thread([this]() { AcceptLoop(); });
}

int SshExporter::BoundRemotePort() const {
    std::lock_guard<std::mutex> lock(mu_);
    return attrs_.remote_port;
}

socket_t SshExporter::DialLocalService() {
    std::string err;
    std::string host;
    int port;
    {
        std::lock_guard<std::mutex> lock(mu_);
        host = attrs_.local_host;
        port = attrs_.local_port;
    }
    // Short and fixed: this is a service on this machine, so a slow dial means it
    // is gone, not far away — and a long wait here delays Close().
    socket_t s = DialTcp(host, port, err, 5000);
    if (!SocketValid(s)) {
        // One dead backend must not kill the export — the service may be restarting.
        RecordError("tunnel_export: an inbound connection arrived but " + host + ":" +
                    std::to_string(port) +
                    " refused it. The exported service stopped or moved; restart it — the "
                    "export is still listening.");
    }
    return s;
}

void SshExporter::AcceptLoop() {
    while (running_.load()) {
        // Wait for the SSH socket OUTSIDE the session lock. The 100ms timeout also
        // makes this self-healing: a pump thread may consume the readable event
        // while a channel-open sits queued, and this wakeup picks it up anyway.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ssh_socket_, &rfds);
        timeval tv{0, 100000};
        select(SelectNfds(ssh_socket_, ssh_socket_), &rfds, nullptr, nullptr, &tv);
        if (!running_.load()) {
            break;
        }

        LIBSSH2_CHANNEL *chan = nullptr;
        int err = 0;
        {
            std::lock_guard<std::mutex> lock(session_mu_);
            if (listener_ == nullptr) {
                break;
            }
            chan = libssh2_channel_forward_accept(listener_);
            if (chan == nullptr) {
                err = libssh2_session_last_errno(session_);
            }
            int seconds_to_next = 0;
            libssh2_keepalive_send(session_, &seconds_to_next);
        }

        if (chan != nullptr) {
            ReapFinishedWorkers();
            auto done = std::make_shared<std::atomic<bool>>(false);
            std::thread t([this, chan, done]() {
                Pump(chan);
                done->store(true);
            });
            std::lock_guard<std::mutex> lock(workers_mu_);
            workers_.push_back(Worker{std::move(t), std::move(done)});
            continue;
        }
        // An empty queue shows up as EAGAIN or CHANNEL_UNKNOWN depending on the path
        // taken inside libssh2; neither is fatal. Only a dead transport is.
        if (err == LIBSSH2_ERROR_SOCKET_SEND || err == LIBSSH2_ERROR_SOCKET_RECV ||
            err == LIBSSH2_ERROR_SOCKET_DISCONNECT) {
            RecordError("tunnel_export: the SSH session to " + auth_.ssh_host +
                        " dropped; the export is no longer active. Re-run PRAGMA "
                        "tunnel_export. If this recurs while idle, raise sshd's "
                        "ClientAliveInterval.");
            running_.store(false);
            break;
        }
    }
}

// Join and drop workers whose connection has ended. Called only from the accept
// loop, so a worker is never reaped by itself.
void SshExporter::ReapFinishedWorkers() {
    std::vector<std::thread> finished;
    {
        std::lock_guard<std::mutex> lock(workers_mu_);
        for (auto it = workers_.begin(); it != workers_.end();) {
            if (it->done->load()) {
                finished.push_back(std::move(it->thread));
                it = workers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Joined outside workers_mu_ — these threads are done, but keeping the lock
    // across a join is the habit that produces the deadlock below.
    for (auto &t : finished) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void SshExporter::Pump(LIBSSH2_CHANNEL *channel) {
    socket_t local = DialLocalService();
    if (!SocketValid(local)) {
        std::lock_guard<std::mutex> lock(session_mu_);
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        return;
    }
    if (!SocketFitsInFdSet(local)) {
        RecordError("tunnel_export: too many open files to serve another exported "
                    "connection safely; it was refused. Raise the file-descriptor limit.");
        CloseSocketHandle(local);
        std::lock_guard<std::mutex> lock(session_mu_);
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        return;
    }
    SetSocketNonBlocking(local);

    char buf[16384];
    while (running_.load()) {
        bool idle = true;

        // local -> channel. select() and recv() happen without the session lock.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(local, &rfds);
        timeval tv{0, 20000};
        if (select(SelectNfds(local, local), &rfds, nullptr, nullptr, &tv) > 0) {
            const int n = SocketRecv(local, buf, sizeof(buf));
            if (n == 0) {
                std::lock_guard<std::mutex> lock(session_mu_);
                libssh2_channel_send_eof(channel);
                break;
            }
            if (n < 0 && !SocketWouldBlock(LastSocketError())) {
                break;
            }
            if (n > 0) {
                idle = false;
                int off = 0;
                bool failed = false;
                while (off < n && running_.load()) {
                    ssize_t w;
                    {
                        std::lock_guard<std::mutex> lock(session_mu_);
                        w = libssh2_channel_write(channel, buf + off, static_cast<size_t>(n - off));
                    }
                    if (w == LIBSSH2_ERROR_EAGAIN) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        continue;
                    }
                    if (w < 0) {
                        failed = true;
                        break;
                    }
                    off += static_cast<int>(w);
                }
                if (failed) {
                    break;
                }
            }
        }

        // channel -> local
        ssize_t m;
        int cerr = 0;
        bool eof = false;
        {
            std::lock_guard<std::mutex> lock(session_mu_);
            m = libssh2_channel_read(channel, buf, sizeof(buf));
            if (m < 0) {
                cerr = libssh2_session_last_errno(session_);
            } else if (m == 0) {
                eof = libssh2_channel_eof(channel) != 0;
            }
        }
        if (m > 0) {
            idle = false;
            if (!SendAll(local, buf, static_cast<size_t>(m), running_)) {
                break;
            }
        } else if (eof) {
            ShutdownWrite(local);
            break;
        } else if (m < 0 && cerr != LIBSSH2_ERROR_EAGAIN) {
            break;
        }

        // Without this the pump spins on EAGAIN and starves the accept loop of the
        // session mutex, which silently overflows libssh2's listener queue.
        if (idle) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    CloseSocketHandle(local);
    std::lock_guard<std::mutex> lock(session_mu_);
    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
}

void SshExporter::Close() {
    const bool was_running = running_.exchange(false);
    (void)was_running;

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    // Move the workers out under the lock, then join with NO lock held. Joining
    // while holding a mutex a worker needs is a deadlock: Pump takes mu_ (via
    // DialLocalService/RecordError) and workers_mu_ is taken by the accept loop.
    std::vector<Worker> pending;
    {
        std::lock_guard<std::mutex> lock(workers_mu_);
        pending.swap(workers_);
    }
    for (auto &w : pending) {
        if (w.thread.joinable()) {
            w.thread.join();
        }
    }

    // Every thread is joined, so nothing can touch the session any more. Order
    // matters: forward_cancel dereferences listener->session, so it must run before
    // the session is freed.
    {
        std::lock_guard<std::mutex> lock(session_mu_);
        if (listener_ != nullptr) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (libssh2_channel_forward_cancel(listener_) == LIBSSH2_ERROR_EAGAIN &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            listener_ = nullptr;
        }
        if (session_ != nullptr) {
            libssh2_session_set_blocking(session_, 1);
            libssh2_session_set_timeout(session_, 5000);
            libssh2_session_disconnect(session_, "Shutdown");
            libssh2_session_free(session_);
            session_ = nullptr;
        }
        if (SocketValid(ssh_socket_)) {
            CloseSocketHandle(ssh_socket_);
            ssh_socket_ = kInvalidSocket;
        }
    }

    std::lock_guard<std::mutex> lock(mu_);
    attrs_.status = "Closed";
}

TunnelConnectionAttributes SshExporter::GetAttributes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return attrs_;
}

} // namespace duckdb
