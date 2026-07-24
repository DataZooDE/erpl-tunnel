# erpl_tunnel Telemetry

`erpl_tunnel` collects **anonymous, privacy-preserving usage telemetry** so we can
see which backends and functions are used, on which platforms, and where they fail —
and prioritise accordingly. It is **on by default** and **trivial to turn off**.

Telemetry is emitted through the shared
[`DataZooDE/posthog-telemetry`](https://github.com/DataZooDE/posthog-telemetry)
library and follows the cross-product **`telemetry_schema: 2`** envelope. Ingestion
is the EU PostHog cloud.

## How to turn it off

Any one of these fully short-circuits telemetry — when disabled, **nothing leaves the
machine** (the opt-out is enforced at the transport, not just at the call sites):

```sql
SET erpl_telemetry_enabled = false;   -- DuckDB setting (per session)
```

```bash
export DATAZOO_DISABLE_TELEMETRY=1     # environment (1|true|yes)
```

## The guarantee: bounded, enumerated, non-PII

Every property we send is **either** a constant drawn from a small, code-controlled
enumeration **or** a pure number (durations, counts). The library additionally clamps
every outgoing string to 512 bytes as a backstop.

Because this is a *tunneling* extension, the privacy bar is especially high. We
**never** send: SSH host names, users, passwords, private-key paths or passphrases;
Tailscale auth keys or NetBird setup keys; control/management URLs; mesh peer names,
DNS names, tags, groups, or mesh IPs; `remote_host`/`remote_port`/`local_port`
values; state-dir paths; SQL text; row/result data; or any error message text. The
only free-form identifier we emit is the fixed, code-defined DuckDB **function name**
(e.g. `tunnel_create`) — never any argument passed to it — plus the **backend kind**
as a three-value enum (`ssh` | `tailscale` | `netbird`).

Instrumentation lives at each function's **bind**/call site (not on any per-row or
per-connection path), so heavy use produces O(1) telemetry, not a firehose.

## What is collected

### Envelope (attached to every event)

`product` (`erpl_tunnel`), `product_version` (CalVer, e.g. `2026.07.24`),
`product_edition` (`oss`), `telemetry_schema` (`2`), `duckdb_version`, `os`, `arch`,
`platform`, `is_ci`, `is_container`, a per-process `$session_id`, and — once
associated — the `deployment` group. `distinct_id` is the SHA-256 of a machine id: a
**stable, pseudonymous** identifier, not tied to any personal data.

### Events

| Event | When | Properties (beyond the envelope) |
|---|---|---|
| `extension_loaded` | the `erpl_tunnel` extension loads | — |
| `function_executed` | a tunnel function is called — **aggregated** per function per session | `function_name` (one of `tunnel_create`, `tunnel_close`, `tunnel_close_all`, `tunnels`, `tunnel_peers`, `tunnel_self`, `tunnel_mesh_activate`), `call_count`, `duration_ms_p50` |
| `feature_used` | `tunnel_create` is called | `feature` = `tunnel_create`, `backend` ∈ {`ssh`, `tailscale`, `netbird`} |

We emit **no** `$exception` events and attach **no** error text to any event.

## Why

The single most useful thing this tells us is **backend mix** (how many users reach
for SSH vs Tailscale vs NetBird) and **which functions are exercised** — so we invest
in the right backend and surface. None of it can identify a user, a host, a tailnet,
or a secret.
