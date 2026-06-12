---
status: accepted
---

# Modules are authorized by content hash, located by namespace:key, with unrestricted provenance

Modules may live in any RADOS namespace, operator- or tenant-curated. The control
plane dictates, per *executing* namespace, which modules may run, via an allowlist
binding `(subsystem, exec-namespace, op-ID) → (module-namespace, module-key, sha256,
per-invocation caps)`. The **sha256 is the sole authorization + integrity anchor**:
the executor fetches the bytes at `(module-namespace, module-key)` only on a
content-cache miss, and runs them only if they hash to the bound sha256. Where the
bytes live never confers the right to run them — only a control-plane binding does.

## Considered options

- **Bind by key/name alone.** Rejected: a tenant with Store access to the keyed
  object could swap the code behind an allowlisted op-ID — arbitrary execution in the
  executor, total bypass of the allowlist (ADR-0008 trust split). Hash-pinning closes
  it; the tenant cannot produce different bytes that hash to the blessed value, nor
  change which value is blessed.
- **Bind by hash alone (no locator).** Content addressing says *what*, not *where*;
  in a KV namespace the bytes still need a key to fetch. `namespace:key` is that
  cold-fetch source, consulted only when the hash isn't already cached.
- **Restrict modules to a reserved namespace.** Rejected as the *only* option: we want
  both operator-curated (locked-down reserved namespace) and tenant-supplied
  (operator-blessed) provenance. Hash-pinning makes arbitrary provenance safe.

## Consequences

- **Exec without read.** A tenant names only an op-ID and never reads the module, so
  operator modules can be run by tenants that have no read access to them.
- **Dedup by hash.** A module blessed for many executing namespaces is fetched once
  and shared in the executor's content-addressed cache.
- **Availability vs integrity.** Integrity is always preserved by the hash check, but
  a module blessed for namespaces *other than its host* can be denied (not corrupted)
  by whoever can overwrite the host object → hash mismatch → fetch fail. Guideline:
  shared/cross-tenant modules live in an operator-controlled namespace; tenant-namespace
  modules are blessed only for that tenant.
- **Schema change (Phase 0).** The sidecar binding field becomes structured
  `module-namespace:key:sha256hex` (keys are 1–16 binary bytes, hex-encoded;
  delimiting must be unambiguous) — structured validation, replacing the opaque
  `cls.method` string.
