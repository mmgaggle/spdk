---
status: accepted
---

# The Exec wire + control-plane ABI is frozen; data-object keys ride in the DPTR payload

This freezes the tenant-facing Exec contract against the TB1 prototype so the execution
site stays relocatable (ADR-0009) without a host-visible re-spin. Two surfaces are
ratified together — the **SQE/CQE wire layout** (rados-nkv ↔ device) and the
**control-plane binding/allowlist schema** (sidecar ↔ rados-nkvx) — because a tenant
sees the seam between them only as "I named an op-ID and got bytes back." The single
open question, how to carry keys longer than the NVMe-KV 16-byte inline cap, is decided
here: **Option 1 — the key is a length-prefixed field inside the Exec DPTR request
payload.**

## Decisions

- **SQE field assignment — frozen as-coded.** `op-ID` in CDW13, input length in CDW10
  (validated against the DPTR transfer length), output-buffer cap (`osize`) in CDW12.
  The DPTR is a **single bidirectional buffer**: it carries the request on the way in
  and is overwritten with the response on the way out (one DMA, request↔response
  shared). Exec is a **vendor opcode** (`0x83`), so it is free to repurpose command
  dwords the KV command set assigns to standard ops.

- **The data-object key moves into the DPTR request payload (Option 1).** NVMe-KV
  rev1.2 is a hard 16-byte inline-key cap (KVKML ≤ 16; `KL > 16` → abort *Invalid
  Field*; the key lives in CDW2/3/14/15, the DPTR carries the *value*). Our keys exceed
  it: 32-byte content hashes and RADOS object names up to 255 bytes. So for Exec the
  key **leaves the canonical CDW2/3/14/15 inline slots** (which become reserved/unused
  for this opcode) and is carried as a **`u16` length-prefixed field at the head of the
  Exec DPTR request payload** (`key_len` 1..255, then `key_len` key bytes, then the
  binding ref + args). This spans both key kinds, adds **no second pointer and no DPTR
  contention** (the key is part of the one payload), and keeps a single DMA. This is a
  delta from the TB1 prototype, which decoded the key from the inline CDW slots into a
  16-byte buffer — see Consequences.

- **Result-size negotiation — Retrieve-style truncation.** The device writes at most
  CDW12 (`osize`, further clamped to the host buffer) bytes back, and always reports the
  **true output length in completion DW0**. A result that exceeds the caller's buffer is
  **truncated, not failed**: the host reads DW0, resizes, and re-issues
  (`BUFFER_TOO_SMALL` semantics). No partial-success ambiguity.

- **Binding encoding — structured `(runtime, module-locator, sha256, caps)`.** The
  control-plane entry is a typed `kv_exec_binding`, not the prototype's opaque
  `"class:method"` string: `runtime` selects the backend (wasmtime in v1, a Velox plan a
  future kind — ADR-0012); the **sha256 of the artifact is the sole authorization +
  integrity anchor** (ADR-0010); `(module-namespace, module-key)` is only the
  cold-fetch locator, consulted on a content-cache miss; plus per-invocation `caps`.
  The sidecar gRPC message carries the **same typed fields**, validated on **both
  sides** — the schema migration from `cls.method` to the structured form is part of
  this freeze.

- **Exec is read-only (ADR-0009/0011 trust split).** Exec never mutates the stored
  value, so it is **permitted on read-only namespaces**, still gated by the
  per-namespace allowlist. This flips the prototype's "Exec is a write" rejection at
  `lib/nvmf/ctrlr_kvdev.c:322-348` — see Consequences.

## Considered options (key length)

- **Option 1 — key length-prefixed in the DPTR payload (chosen).** Zero DPTR
  contention, one DMA, spans 32B hashes + 255B names. Cost: the key leaves its
  canonical SQE slot — acceptable for a vendor opcode that already defines a structured
  payload.
- **Option 2 — Samsung-style key-PRP union** (inline `key[16]` OR `key_prp/key_prp2`
  pointers to a separate host key buffer in the command-tail dwords). Rejected for
  Exec: a second pointer + second DMA, and it couples Exec to a base-layer
  Store/Retrieve decision. If `Store`/`Retrieve` ever need >16B keys, Option 2 is the
  **separate base-layer choice** — explicitly **out of this Exec freeze**.
- **Option 3 — hijack the standard key-length field for 17..255** — rejected: fights
  the spec's `KL > 16` → abort contract and confuses standard-conformant tooling.

## Consequences

- **Prototype delta — key decode.** TB1 reads the key from the inline CDW slots
  (`nvmf_kvdev_decode_key`, 16-byte cap). The frozen ABI requires the datapath to read
  the `u16`-prefixed key from the head of the Exec DPTR payload instead, leaving
  CDW2/3/14/15 unused for Exec. Tracked as a follow-up implementation slice; it lands
  with TB3 (`spdk-fbm`, allowlist-driven module loading) or just ahead of it.
- **Prototype delta — read-only gate + binding type.** The read-only allow-list at
  `ctrlr_kvdev.c:322-348` must add `SPDK_NVME_OPC_KV_EXEC` to the permitted set, and
  `nvmf_ns_kv_exec_op_allowed` must yield the structured `kv_exec_binding` rather than
  an opaque string. Both land with the sidecar schema migration.
- **Partition Arrow schema is deferred out of this freeze.** It couples to the cut-edge
  sampling policy (TB6, `spdk-swd`) and is not tenant-visible through the Exec ABI, so
  freezing it now would be premature. The Exec wire/control-plane contract stands
  without it.
- **What this buys.** No remaining open ABI question forces a later tenant-visible
  change. TB3 and downstream slices build against a fixed contract; the executor can
  move hosts (two-tier, GPU-initiated) with no host re-spin.

## Refinement — the read-only invariant is ENFORCED, not assumed (spdk-5hk)

The original freeze permits `KV_EXEC` on a read-only namespace on the premise that
"Exec never mutates." Adversarial review found that premise was an *unchecked
assumption*: the read-only opcode gate (`ctrlr_kvdev.c`) only decides *whether* Exec
is dispatched, and the per-namespace op allow-list checks *membership* only — neither
verifies an op is non-mutating. A shipped built-in contradicted it (the in-memory
`APPEND`, op_id 2, writes the stored value), so a mutating op could be allow-listed and
run on a read-only namespace.

The invariant is now **enforced at the mutation point** rather than assumed at the gate.
The namespace read-only state is threaded through `spdk_kvdev_exec()` into every backend
`exec` op (`bool read_only`). A backend MUST reject any path that could write —
returning `SPDK_KVDEV_IO_STATUS_READ_ONLY` (mapped to Command-Specific *Attempted Write
to Read Only Range*) — when `read_only` is set: the in-memory `APPEND` is rejected, the
legacy Ceph object-class (`cls`) path is rejected (a cls method can write arbitrarily and
non-mutation cannot be proven from the binding), while non-mutating paths (echo, the
read-only nkvx wasm executor) still run. This generalizes to any future mutating built-in
or write-capable module, which must guard at its own mutation point. The opcode gate is
unchanged (it still permits `KV_EXEC`); the enforcement is additive and below it.
