---
status: accepted; supersedes ADR-0005
---

# KV Exec runs in a sandboxed executor that owns its namespace, not as a Ceph object-class in the OSD

ADR-0005 implemented KV Exec via `rados_read_op_exec` — the value's compute ran as
a Ceph object-class (`cls`) inside ceph-osd, under the PG lock. We are removing that
from the datapath. Exec now runs in **rados-nkvx**, a sandboxed WASM executor that
owns a content-addressed local-NVMe namespace and cold-fills from RADOS via plain
librados. The OSD↔executor interface is unprivileged librados reads on the cold path
only; the OSD is never modified and never in the hot path. The Exec opcode is a
frozen ABI between the rados-nkv front and the rados-nkvx executor, so execution can
be co-located (single-tier) or CRUSH-routed to the storage host (two-tier) with no
tenant-visible change.

## Considered options

- **In-OSD `cls` (ADR-0005, superseded).** Powerful but unshippable as a
  multi-tenant, tenant-extensible mechanism: it executes synchronously under the PG
  op-shard lock (a multi-ms sample head-of-line-blocks all ops on that shard), and a
  runtime bug crashes a *durability* process — a tenant-triggered DoS surface. The
  OSD is not restartable the way an executor is.
- **Masquerade as the OSD on the messenger (transparent MITM).** Requires holding
  the OSD's cephx identity and reimplementing the stateful msgr2 + OSD op dialect;
  it's credential impersonation, not an interface, and it optimizes only the cold
  path, which is already amortized to ~zero.
- **`SO_REUSEPORT` + shared cephx to "sniff" cls.** REUSEPORT load-balances whole
  connections by 4-tuple hash, not messages by type, and the steering is pre-auth —
  it cannot route by op-type, and "stub cls in the real OSD" makes correctness
  hash-dependent. The only caller of the cls is our own kvdev, so there is nothing
  to sniff once we stop calling it.
- **Reach into BlueStore's onode/buffer cache or extent map for zero-copy.** Pointer
  graphs with process-local refcounted lifetimes, sparse/compressed/checksummed
  bytes, and a device claimed exclusively by BlueStore's SPDK backend — more invasive
  OSD coupling than `cls`, for a copy we only pay on the cold path.

## Consequences

- Cold fill pays one loopback messenger hop + a copy (no shared-memory transport
  exists in mainline Ceph); this is acceptable because partitions are immutable and
  reused, so it is paid once per partition per host, not per Exec.
- Zero-copy NVMe→linear-memory DMA lives entirely in the executor's **own** namespace
  (a raw-bdev content-addressed store we control), downstream of the OSD boundary.
- For host-local data the partitions must live in a **replicated** pool; erasure
  coding forfeits host-local data (the primary host holds one shard).
- The earlier ADRs 0001–0008 currently live in the beads (Dolt) store; `bd` was not
  available when this file was written, so it is recorded on disk at the path the
  code/README already reference. Reconcile into beads when convenient.
