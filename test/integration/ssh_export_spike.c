/*
 * ssh_export_spike.c — M0 de-risking spike for `tunnel_export` over SSH.
 *
 * The question this exists to answer:
 *
 *   libssh2 matches an inbound `forwarded-tcpip` channel-open against a listener by
 *   comparing the *bind address string* byte-for-byte (packet.c: listn->host vs the
 *   address sshd echoes) AND the port. OpenSSH canonicalises that address — notably
 *   it rewrites the bind to 127.0.0.1 when GatewayPorts is `no`, which is the default.
 *
 *   If the strings disagree, `forward_listen_ex` SUCCEEDS, every inbound connection is
 *   silently refused, and nothing surfaces an error anywhere. That is a hang with no
 *   diagnostic, which is precisely the kind of default we must not guess at.
 *
 * So: actually push a payload through, for each (GatewayPorts x bind-host) combination,
 * and let the results choose the default.
 *
 * Build:  cc -o ssh_export_spike ssh_export_spike.c -lssh2   (or the vcpkg static lib)
 * Usage:  ssh_export_spike <ssh_host> <ssh_port> <user> <pass> \
 *                          <bind_host|-> <remote_port> <local_host> <local_port>
 *
 * Exits 0 and prints SPIKE_OK once one full payload has been proxied; 1 otherwise.
 */
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <libssh2.h>

static int dial(const char *host, int port) {
    struct addrinfo hints, *res = NULL;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) {
        return -1;
    }
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s >= 0 && connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    return s;
}

int main(int argc, char **argv) {
    if (argc < 9) {
        fprintf(stderr, "usage: %s <ssh_host> <ssh_port> <user> <pass> "
                        "<bind_host|-> <remote_port> <local_host> <local_port>\n", argv[0]);
        return 2;
    }
    const char *ssh_host = argv[1];
    const int   ssh_port = atoi(argv[2]);
    const char *user = argv[3], *pass = argv[4];
    const char *bind_host = strcmp(argv[5], "-") == 0 ? NULL : argv[5];
    const int   remote_port = atoi(argv[6]);
    const char *local_host = argv[7];
    const int   local_port = atoi(argv[8]);

    if (libssh2_init(0) != 0) { fprintf(stderr, "SPIKE_FAIL libssh2_init\n"); return 1; }

    int sock = dial(ssh_host, ssh_port);
    if (sock < 0) { fprintf(stderr, "SPIKE_FAIL connect %s:%d\n", ssh_host, ssh_port); return 1; }

    LIBSSH2_SESSION *session = libssh2_session_init();
    if (!session) { fprintf(stderr, "SPIKE_FAIL session_init\n"); return 1; }
    libssh2_session_set_timeout(session, 15000);          /* blocking, but bounded */
    if (libssh2_session_handshake(session, sock) != 0) {
        fprintf(stderr, "SPIKE_FAIL handshake\n"); return 1;
    }
    if (libssh2_userauth_password(session, user, pass) != 0) {
        fprintf(stderr, "SPIKE_FAIL auth\n"); return 1;
    }

    /* The call under test. */
    int bound = -1;
    LIBSSH2_LISTENER *listener =
        libssh2_channel_forward_listen_ex(session, bind_host, remote_port, &bound, 16);
    if (!listener) {
        char *msg = NULL; int mlen = 0;
        int err = libssh2_session_last_error(session, &msg, &mlen, 0);
        printf("SPIKE_LISTEN_FAIL errno=%d msg=%s\n", err, msg ? msg : "?");
        return 1;
    }
    printf("SPIKE_LISTENING requested_bind=%s requested_port=%d bound_port=%d\n",
           bind_host ? bind_host : "(NULL->0.0.0.0)", remote_port, bound);
    fflush(stdout);
    if (bound == 0) {
        /* A listener stuck at port 0 can never match an inbound open — fail loudly
         * rather than hanging, which is the whole point of this spike. */
        printf("SPIKE_FAIL bound_port==0 (inbound could never be routed)\n");
        return 1;
    }

    /* Non-blocking from here: a blocking forward_accept parks forever. */
    libssh2_session_set_blocking(session, 0);

    const time_t deadline = time(NULL) + 25;
    LIBSSH2_CHANNEL *chan = NULL;
    int last_errno = 0;
    while (time(NULL) < deadline) {
        chan = libssh2_channel_forward_accept(listener);
        if (chan) break;
        /* An empty queue surfaces as EAGAIN *or* CHANNEL_UNKNOWN ("Channel not
         * found") depending on the path taken inside libssh2 — neither is fatal,
         * they just mean "nothing has arrived yet". Only a dead transport is fatal,
         * and that shows up as SOCKET_* / a closed session, which the loop below
         * detects by simply running out of deadline. Keep polling. */
        last_errno = libssh2_session_last_errno(session);
        if (last_errno == LIBSSH2_ERROR_SOCKET_SEND ||
            last_errno == LIBSSH2_ERROR_SOCKET_RECV ||
            last_errno == LIBSSH2_ERROR_SOCKET_DISCONNECT) {
            char *msg = NULL; int mlen = 0;
            libssh2_session_last_error(session, &msg, &mlen, 0);
            printf("SPIKE_FAIL transport died during accept: %s\n", msg ? msg : "?");
            return 1;
        }
        usleep(50 * 1000);
    }
    if (!chan) {
        /* THE failure mode we are hunting: listen succeeded, nothing was ever routed. */
        printf("SPIKE_NO_CONNECTION (listen succeeded but no inbound channel arrived — "
               "host-string mismatch or sshd bound elsewhere; last accept errno=%d)\n",
               last_errno);
        return 1;
    }
    printf("SPIKE_ACCEPTED\n");
    fflush(stdout);

    /* Prove bytes actually traverse: proxy this one connection to the local service. */
    int local = dial(local_host, local_port);
    if (local < 0) { printf("SPIKE_FAIL local dial %s:%d\n", local_host, local_port); return 1; }

    char buf[8192];
    ssize_t total_up = 0, total_down = 0;
    const time_t pump_deadline = time(NULL) + 15;
    int saw_response = 0;
    while (time(NULL) < pump_deadline && !saw_response) {
        ssize_t n = libssh2_channel_read(chan, buf, sizeof(buf));
        if (n > 0) { total_up += n; (void)!write(local, buf, (size_t)n); }
        else if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) { break; }

        struct timeval tv = {0, 50 * 1000};
        fd_set r; FD_ZERO(&r); FD_SET(local, &r);
        if (select(local + 1, &r, NULL, NULL, &tv) > 0) {
            ssize_t m = read(local, buf, sizeof(buf));
            if (m > 0) {
                total_down += m;
                ssize_t off = 0;
                while (off < m) {
                    ssize_t w = libssh2_channel_write(chan, buf + off, (size_t)(m - off));
                    if (w == LIBSSH2_ERROR_EAGAIN) { usleep(10000); continue; }
                    if (w < 0) break;
                    off += w;
                }
                saw_response = 1;   /* the local service answered — the path works */
            } else if (m == 0) {
                break;
            }
        }
    }

    printf("SPIKE_BYTES up=%zd down=%zd\n", total_up, total_down);
    close(local);
    libssh2_channel_close(chan);
    libssh2_channel_free(chan);
    libssh2_channel_forward_cancel(listener);
    libssh2_session_disconnect(session, "spike done");
    libssh2_session_free(session);
    close(sock);
    libssh2_exit();

    if (total_up > 0 && total_down > 0) { printf("SPIKE_OK\n"); return 0; }
    printf("SPIKE_FAIL no payload traversed\n");
    return 1;
}
