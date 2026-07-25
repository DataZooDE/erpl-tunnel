/*
 * mesh_shim.h — the identical C ABI exported by every mesh backend shim
 * (ts_shim over Tailscale tsnet, nb_shim over NetBird client/embed).
 *
 * The C++ MeshBackend binds ONE set of function pointers from whichever shim the
 * loader dlopen'd/LoadLibrary'd (HLD §5.2). Handles are opaque ints (indices into
 * a Go-side registry — Go pointers must never cross into C). Streams are handed
 * back as OS socket handles so the C++ port-forward engine pumps bytes itself and
 * never touches Go memory.
 *
 * Nothing but integers, caller-owned buffers and OS handles crosses this boundary:
 * no malloc/free across it, no FILE*, no C++ objects. That is what makes it safe
 * for a mingw-built shim to be loaded by an MSVC-built host on Windows.
 *
 * All functions are thread-safe and return 0 on success, non-zero on error; call
 * mesh_errmsg() for the last human-readable error on that node.
 */
#ifndef ERPL_TUNNEL_MESH_SHIM_H
#define ERPL_TUNNEL_MESH_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* mesh_kind identifies which mesh this shim implements. */
#define MESH_KIND_TAILSCALE 1
#define MESH_KIND_NETBIRD   2

typedef long mesh_node; /* opaque handle; >0 valid, <=0 invalid */

/* An OS stream handle: a file descriptor on Unix, a Winsock SOCKET on Windows.
 * uintptr_t because a Win64 SOCKET is UINT_PTR and does not fit an int. Opaque
 * except that it is a connected, blocking, bidirectional byte stream the caller
 * OWNS and releases (close() / closesocket()). A half-close by the peer shows up
 * as recv() returning 0. There are never residual bytes buffered on it at handoff.
 *
 * Windows note: use recv/send, never read/write — the latter only work on CRT file
 * descriptors, not on SOCKETs. */
typedef uintptr_t mesh_stream;
#define MESH_STREAM_INVALID ((mesh_stream)-1)

/* Which mesh is this shim? (1=tailscale, 2=netbird). Callable before mesh_new. */
int mesh_kind(void);

/* Allocate a new (unconfigured) node handle. Returns <=0 on failure. */
mesh_node mesh_new(void);

/* Set a string option before mesh_up. Recognised keys (unknown keys are ignored):
 *   auth_key, setup_key, hostname, tags, groups, control_url, mgmt_url, state_dir
 */
int mesh_set_str(mesh_node node, const char *key, const char *val);

/* Set a boolean option before mesh_up. Recognised keys: ephemeral. */
int mesh_set_bool(mesh_node node, const char *key, int val);

/* Enroll + connect. Blocks until the node is usable (or errors/timeouts).
 * Idempotent: calling again on an up node is a no-op success. */
int mesh_up(mesh_node node);

/* Dial host:port on the mesh. On success writes a connected stream handle to
 * *stream_out that the caller owns. It is one end of a local stream pair (an
 * AF_UNIX socketpair on Unix, a loopback TCP pair on Windows) whose other end a
 * goroutine bridges to the userspace mesh connection. */
int mesh_dial(mesh_node node, const char *host, int port, mesh_stream *stream_out);

/* Publish a local service onto the mesh: listen on mesh_port on THIS node's mesh
 * address and forward every accepted connection to local_host:local_port.
 *
 * Unlike mesh_dial, no stream handle crosses this boundary. The listener, the
 * accept loop and both ends of every proxied connection live entirely inside Go,
 * because the whole job is a port-forward and C++ has nothing to add in the middle.
 * That is deliberate: an accepted connection is born inside Go, so surfacing it to C
 * would need the socketpair + SCM_RIGHTS trick libtailscale uses — which has no
 * Windows equivalent. Keeping it in Go makes the three platforms identical.
 *
 * On success writes an opaque export handle to *export_out. Handles are scoped to
 * the node and are released by mesh_unexport, or wholesale by mesh_close. */
int mesh_export(mesh_node node, int mesh_port, const char *local_host, int local_port,
                long *export_out);

/* Stop a listener started by mesh_export and drop its accept loop. Idempotent:
 * unexporting an unknown or already-released handle succeeds. In-flight proxied
 * connections are closed. */
int mesh_unexport(mesh_node node, long export_handle);

/* Serialise the peer-local netmap/status as a JSON array of
 *   {backend, host_name, dns_name, mesh_ip, tags, online}
 * into buf (up to len bytes). If the buffer is too small, writes nothing and sets
 * *need to the required size; the caller retries with a larger buffer. */
int mesh_peers_json(mesh_node node, char *buf, size_t len, size_t *need);

/* Serialise this node's own identity as a single JSON object. Same buffer contract. */
int mesh_self_json(mesh_node node, char *buf, size_t len, size_t *need);

/* Copy the last error message on this node into buf (NUL-terminated, truncated to
 * len). Returns the number of bytes written (excluding the NUL). */
int mesh_errmsg(mesh_node node, char *buf, size_t len);

/* Tear down the node and release its handle. Idempotent. */
int mesh_close(mesh_node node);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ERPL_TUNNEL_MESH_SHIM_H */
