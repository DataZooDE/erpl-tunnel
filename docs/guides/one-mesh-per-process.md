# One mesh per process

A single DuckDB process can run **one** mesh backend — Tailscale **or** NetBird —
at a time. This is a hard constraint: each mesh embeds a Go runtime, and two Go
runtimes cannot coexist in one process. SSH has no such limit and uses no Go.

This is not a conservative guardrail — loading two Go shared libraries into one
process is explicitly unsupported upstream and corrupts the garbage collector
([golang/go#36628](https://github.com/golang/go/issues/36628),
[#65050](https://github.com/golang/go/issues/65050)), with Windows the most
crash-prone. The latch is what keeps you on the right side of it. For the same
reason the extension never unloads a mesh shim once activated: its Go runtime has
already started threads, and unmapping it would leave them running in freed pages.

The extension enforces it cleanly. The **first** mesh you activate wins; activating
the other in the same process is refused with an actionable message:

```
Tunnel: Tailscale is active in this DuckDB process; NetBird cannot be loaded —
start a new session for a different mesh.
```

To use the other mesh, start a new DuckDB session.

You can **preselect** the mesh for this process without enrolling a tunnel yet:

```sql
PRAGMA tunnel_mesh_activate('tailscale');   -- or 'netbird'
```

This surfaces any load/auth problem immediately and pins the one mesh this process
will use. It's optional — the first `tunnel_import`/`tunnel_peers` on a mesh secret
does the same thing lazily.

## What this means in practice

- Mixing **SSH + one mesh** in the same session is fine.
- Two Tailscale secrets (or two NetBird secrets) in one session is fine — same mesh.
- A `both` build carries both meshes; the runtime just picks the first one you use.

## Footprint

SSH-only sessions load **no Go at all** — the mesh shim is embedded but only
`dlopen`'d the first time you use that backend. So a `both` build costs nothing
extra until you actually reach for Tailscale or NetBird.

| Build | Mesh code loaded when… |
|---|---|
| `MESH_BACKEND=ssh` | never (no Go in the artifact) |
| `both` (default), SSH-only session | never (lazy) |
| `both`, first Tailscale/NetBird use | that one shim is loaded |

See [FOOTPRINT.md](../design/FOOTPRINT.md) for sizes and the glibc floor.

## Where the shim is loaded from

The shim is embedded in the extension and written out only on first activation.

- **Linux and macOS** — extracted to a private `0700` temp file, `dlopen`'d, then
  unlinked immediately. The mapping stays valid for the life of the process and
  nothing is left on disk.
- **Windows** — a loaded DLL cannot be deleted, so the same trick is impossible.
  The shim is written instead to a content-addressed per-user cache and reused:

  ```
  %LOCALAPPDATA%\DataZoo\erpl-tunnel\shims\<hash>\ts_shim.dll
  ```

  The directory name is a hash of the shim bytes, so it self-invalidates when you
  upgrade the extension. It is safe to delete when DuckDB is not running; it will
  be recreated on the next mesh activation. If endpoint protection quarantines it,
  allowlist that path — see [troubleshooting](troubleshooting.md).
