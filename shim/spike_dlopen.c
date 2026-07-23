/*
 * spike_dlopen.c — the highest-risk de-risking spike (BRD R1, HLD §12).
 *
 * Proves the embed→dlopen→activate path for a Go c-shared shim works: dlopen the
 * shim, resolve every mesh_* symbol, then actually EXECUTE Go code through the C
 * ABI (mesh_new + mesh_up error path) — exercising Go runtime init inside a
 * dlopen'd .so. If the two-Go-runtime / dlopen+cgo hazards were going to bite,
 * they bite here, before any C++ is built on top.
 *
 * Build:  cc -o spike_dlopen spike_dlopen.c -ldl
 * Run:    ./spike_dlopen ./ts/ts_shim.so
 * Exit 0 = the Go runtime loaded and ran under dlopen; the C ABI is callable.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef long mesh_node;

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "./ts/ts_shim.so";

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "FAIL dlopen: %s\n", dlerror());
        return 1;
    }
    printf("PASS dlopen: %s\n", path);

    // Resolve every symbol of the C ABI.
    const char *syms[] = {"mesh_kind", "mesh_new", "mesh_set_str", "mesh_set_bool",
                          "mesh_up", "mesh_dial", "mesh_peers_json", "mesh_self_json",
                          "mesh_errmsg", "mesh_close"};
    for (unsigned i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
        if (!dlsym(h, syms[i])) {
            fprintf(stderr, "FAIL dlsym: %s missing\n", syms[i]);
            return 1;
        }
    }
    printf("PASS dlsym: all 10 mesh_* symbols resolved\n");

    int (*mesh_kind)(void) = (int (*)(void))dlsym(h, "mesh_kind");
    mesh_node (*mesh_new)(void) = (mesh_node(*)(void))dlsym(h, "mesh_new");
    int (*mesh_set_str)(mesh_node, const char *, const char *) =
        (int (*)(mesh_node, const char *, const char *))dlsym(h, "mesh_set_str");
    int (*mesh_up)(mesh_node) = (int (*)(mesh_node))dlsym(h, "mesh_up");
    int (*mesh_errmsg)(mesh_node, char *, size_t) =
        (int (*)(mesh_node, char *, size_t))dlsym(h, "mesh_errmsg");
    int (*mesh_close)(mesh_node) = (int (*)(mesh_node))dlsym(h, "mesh_close");

    // Execute Go: mesh_kind must report Tailscale (1).
    if (mesh_kind() != 1) {
        fprintf(stderr, "FAIL mesh_kind: expected 1, got %d\n", mesh_kind());
        return 1;
    }
    printf("PASS mesh_kind == 1 (tailscale) — Go code executed under dlopen\n");

    // mesh_new allocates a handle in the Go-side registry.
    mesh_node n = mesh_new();
    if (n <= 0) {
        fprintf(stderr, "FAIL mesh_new: got %ld\n", n);
        return 1;
    }
    printf("PASS mesh_new -> handle %ld\n", n);

    // mesh_up with no auth key must fail cleanly (Go error path runs; no crash),
    // and mesh_errmsg must return an actionable message mentioning the auth key.
    mesh_set_str(n, "hostname", "spike-node");
    int rc = mesh_up(n);
    if (rc == 0) {
        fprintf(stderr, "FAIL mesh_up: expected error (no auth key), got success\n");
        return 1;
    }
    char err[512] = {0};
    mesh_errmsg(n, err, sizeof(err));
    if (!strstr(err, "auth key")) {
        fprintf(stderr, "FAIL mesh_errmsg: expected 'auth key', got '%s'\n", err);
        return 1;
    }
    printf("PASS mesh_up error path: '%s'\n", err);

    mesh_close(n);
    printf("PASS mesh_close\n");

    printf("\nSPIKE OK — Go runtime loaded & executed via dlopen; C ABI callable.\n");
    return 0;
}
