---
status: accepted; extends ADR-0003
---

# TTL selects the storage tier: TTL writes are ephemeral and never durably persisted

The NVMe-KV vendor TTL (ADR-0003) becomes the storage-tier selector on Store. A Store
**with** a TTL is *ephemeral*: it lands only in the owning-host executor's resident
store, ages out at the TTL, and is **never written to RADOS** — no replication, no
durability, no PG migration. A Store **without** a TTL is *durable*: write-through to
RADOS with full guarantees, originated host-local on the primary's host. The client
declares durability intent per write. This resolves the write-pushdown latency
problem: durable writes are quorum-bound regardless of pushdown, but ephemeral writes
(e.g. LLM KV-cache: recomputable, hot, TTL'd) complete at local-NVMe latency and never
burden RADOS with data that does not need durability.

Reads stay tier-agnostic: the front routes by `CRUSH(K)`; the owning-host executor
serves a resident ephemeral entry if present (shadowing RADOS for its lifetime), else
cold-fills from RADOS.

## Consequences

- **No availability / durability / migration for ephemeral data.** Single copy, one
  host; lost on host failure, and orphaned on CRUSH rebalance (it never went to RADOS,
  so it does not migrate when `primary(K)` moves). Effective lifetime is
  `min(TTL, time-to-remap)`. This is a hard contract: ephemeral = best-effort, may
  vanish before its TTL. Acceptable only for recomputable/ephemeral data.
- **Eviction priority under capacity pressure (open policy).** Ephemeral entries are
  unrecoverable (no backing store); durable-cache entries are re-fetchable from RADOS.
  The executor must choose what to drop, trading "honor the TTL" against "keep the
  unrecoverable data." A v1 knob.
- **No mixed tiers on one mutable key.** A durable + ephemeral write to the same key
  needs a precedence rule (local-shadows-RADOS-until-expiry). Free for the flagships
  (content-addressed/versioned keys never collide); a sharp edge for general mutable
  use.
- **`CRUSH(K)` doubles as the cache-placement hash**, so writer and reader agree on the
  ephemeral entry's host with zero coordination, reusing the OSD map the front already
  has — at the cost of coupling ephemeral distribution to OSD topology (revisitable).
- Extends ADR-0003: TTL gains enforced tier-selecting semantics. Reconcile with any
  current rados-path TTL handling in Phase 0.
