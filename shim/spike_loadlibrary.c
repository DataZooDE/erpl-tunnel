/*
 * spike_loadlibrary.c — the Windows sibling of spike_dlopen.c (BRD R1 / HLD §12).
 *
 * Proves the load-bearing assumption of the Windows mesh port: that a Go
 * c-shared DLL built with mingw-w64 can be LoadLibrary'd by an MSVC-built host
 * and actually run.
 *
 * Build (deliberately with MSVC, not mingw — the MSVC<->mingw boundary is the
 * whole point):
 *     cl /nologo spike_loadlibrary.c
 *     spike_loadlibrary.exe ts_shim.dll 1
 *
 * WHY THIS IS NOT JUST A LOAD TEST: golang/go#75949 means that on Go 1.25.x a
 * c-shared DLL loads fine and its symbols resolve fine, but LoadLibrary never
 * triggers the Go runtime's init — so the runtime is dead and the first real
 * call misbehaves. "The DLL loaded" and "Go is running" are different
 * observations, and the bug lives exactly in the gap. So this spike asserts
 * behaviour that only initialised Go can produce:
 *   - mesh_new() returns a handle from a package-level Go map (that map is
 *     populated by package init),
 *   - mesh_up() with no auth key produces a Go-formatted error string.
 * The fix shipped in Go 1.26, which is why the toolchain floor is 1.26.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef long mesh_node;

typedef int (*fn_kind)(void);
typedef mesh_node (*fn_new)(void);
typedef int (*fn_up)(mesh_node);
typedef int (*fn_errmsg)(mesh_node, char *, size_t);
typedef int (*fn_close)(mesh_node);

static void die(const char *what) {
    DWORD e = GetLastError();
    char *msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, e, 0, (LPSTR)&msg, 0, NULL);
    fprintf(stderr, "FAIL: %s (error %lu: %s)\n", what, (unsigned long)e, msg ? msg : "?");
    if (msg) {
        LocalFree(msg);
    }
    exit(1);
}

static void *must_resolve(HMODULE h, const char *name) {
    void *p = (void *)GetProcAddress(h, name);
    if (p == NULL) {
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "GetProcAddress(%s)", name);
        die(buf);
    }
    return p;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <shim.dll> <expected-kind 1|2>\n", argv[0]);
        return 2;
    }
    const int want_kind = atoi(argv[2]);

    HMODULE h = LoadLibraryExA(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (h == NULL) {
        die("LoadLibraryExA — if this is ERROR_MOD_NOT_FOUND the shim has a "
            "dependent DLL it should not (rebuild with -static-libgcc)");
    }
    printf("ok: LoadLibraryEx(%s)\n", argv[1]);

    /* All ten ABI symbols must resolve, exactly as the Unix spike checks. */
    static const char *const names[] = {"mesh_kind",       "mesh_new",      "mesh_set_str",
                                        "mesh_set_bool",   "mesh_up",       "mesh_dial",
                                        "mesh_peers_json", "mesh_self_json", "mesh_errmsg",
                                        "mesh_close"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        (void)must_resolve(h, names[i]);
    }
    printf("ok: all 10 mesh_* symbols resolved\n");

    fn_kind   k     = (fn_kind)must_resolve(h, "mesh_kind");
    fn_new    nw    = (fn_new)must_resolve(h, "mesh_new");
    fn_up     up    = (fn_up)must_resolve(h, "mesh_up");
    fn_errmsg emsg  = (fn_errmsg)must_resolve(h, "mesh_errmsg");
    fn_close  clos  = (fn_close)must_resolve(h, "mesh_close");

    /* First call into Go. On a broken runtime this is where it dies. */
    int got_kind = k();
    if (got_kind != want_kind) {
        fprintf(stderr, "FAIL: mesh_kind() = %d, want %d\n", got_kind, want_kind);
        return 1;
    }
    printf("ok: mesh_kind() = %d\n", got_kind);

    /* Package-level Go state: a handle can only come from an initialised map. */
    mesh_node n = nw();
    if (n <= 0) {
        fprintf(stderr, "FAIL: mesh_new() = %ld — Go package init did not run "
                        "(golang/go#75949; needs Go >= 1.26)\n", n);
        return 1;
    }
    printf("ok: mesh_new() = %ld (Go package init ran)\n", n);

    /* Go error formatting: must fail without a key, with an actionable message. */
    if (up(n) == 0) {
        fprintf(stderr, "FAIL: mesh_up() succeeded with no auth/setup key\n");
        return 1;
    }
    char buf[512] = {0};
    int wrote = emsg(n, buf, sizeof(buf));
    if (wrote <= 0 || strlen(buf) == 0) {
        fprintf(stderr, "FAIL: mesh_errmsg() returned nothing — Go string marshalling broken\n");
        return 1;
    }
    if (strstr(buf, "key") == NULL) {
        fprintf(stderr, "FAIL: mesh_errmsg() = \"%s\", expected it to mention a key\n", buf);
        return 1;
    }
    printf("ok: mesh_up() refused without a key: \"%s\"\n", buf);

    clos(n);
    printf("ok: mesh_close()\n");

    /* Deliberately no FreeLibrary: a Go runtime cannot be unloaded — its threads
     * would be running in unmapped pages. Same rule as the Unix loader. */
    printf("SPIKE-LOADLIBRARY OK — Go runs inside an MSVC host on Windows.\n");
    return 0;
}
