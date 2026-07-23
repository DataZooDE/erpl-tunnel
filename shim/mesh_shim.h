/*
 * mesh_shim.h — the identical C ABI exported by every mesh backend shim
 * (ts_shim over Tailscale tsnet, nb_shim over NetBird client/embed).
 *
 * The C++ MeshBackend binds ONE set of function pointers from whichever shim the
 * loader dlopen'd (HLD §5.2). Handles are opaque ints (indices into a Go-side
 * registry — Go pointers must never cross into C). Streams are handed back as OS
 * file descriptors so the C++ port-forward engine pumps bytes with read()/write()
 * and never touches Go memory.
 *
 * All functions are thread-safe and return 0 on success, non-zero on error; call
 * mesh_errmsg() for the last human-readable error on that node.
 */
#ifndef ERPL_TUNNEL_MESH_SHIM_H
#define ERPL_TUNNEL_MESH_SHIM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* mesh_kind identifies which mesh this shim implements. */
#define MESH_KIND_TAILSCALE 1
#define MESH_KIND_NETBIRD   2

typedef long mesh_node; /* opaque handle; >0 valid, <=0 invalid */

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

/* Dial host:port on the mesh. On success writes a connected OS fd to *fd_out that
 * the caller owns (close() to release). The fd is one end of a socketpair whose
 * other end a goroutine bridges to the userspace mesh connection. */
int mesh_dial(mesh_node node, const char *host, int port, int *fd_out);

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
