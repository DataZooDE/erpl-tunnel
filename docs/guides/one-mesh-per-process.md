# One mesh per process

A single DuckDB process can run **one** mesh backend — Tailscale **or** NetBird —
at a time. This is a hard constraint: each mesh embeds a Go runtime, and two Go
runtimes cannot coexist in one process. SSH has no such limit and uses no Go.

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
will use. It's optional — the first `tunnel_create`/`tunnel_peers` on a mesh secret
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
