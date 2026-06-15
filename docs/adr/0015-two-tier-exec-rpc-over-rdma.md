---
status: accepted (amended 2026-06-15 — transport binding generalised to libfabric; see Amendment)
---

# Two-tier front→executor Exec dispatch is RPC-over-RDMA, not NVMe-oF

ADR-0014 froze the tenant-facing Exec as a **bidirectional DPTR-shared** command
(request in, response out, one command). That is correct for the **single-tier /
tenant edge** — vfio-user is PCIe-style direct DMA, so the controller reads the request
and writes the response into the same mapped buffer. But it does **not** survive a
fabric. This ADR is additive to ADR-0014: it picks the transport for the inter-tier
link that ADR-0009/0012 left as "the relocation."

## Empirical basis (E810, real hardware, 2026-06-12)

Tested NVMe-oF over **both** transports on an Intel E810 (100 GbE, RoCEv2):

| Transport | Store (unidirectional) | Exec (bidirectional, opcode `0x83`) |
|---|---|---|
| vfio-user (PCIe direct DMA) | ✅ | ✅ |
| NVMe-oF / **TCP** | ✅ | ❌ `sct=0 sc=1` (Invalid Field), off-reactor-proof=0 |
| NVMe-oF / **RDMA (RoCE)** | ✅ | ❌ `sct=0 sc=1` — identical to TCP |

The RoCE connection itself was fine (Store round-tripped over the wire), so the Exec
rejection is the **NVMe-oF data model**, not the transport: a fabric command transfers
data in exactly one direction (H2C or C2H, from the opcode/SGL); bidirectional
(`0x83` = bits `11`) has no fabric expression. Repro:
`test/nvmf/kv/kv_nkvx_exec_{tcp,rdma}.sh` + `kv_host` `KV_HOST_TRTYPE={tcp,rdma}`.

## Decision

- **Single-tier / tenant edge stays NVMe-KV over vfio-user**, exactly as ADR-0014 froze
  it (bidirectional DPTR, key-in-payload). Unchanged.
- **Two-tier `rados-nkv` (front) → `rados-nkvx` (executor)** Exec dispatch is
  **RPC-over-RDMA** (RoCE verbs). The front terminates the tenant's NVMe-KV Exec and
  re-dispatches `(op-ID, key, request) → (result)` as an RPC. Bidirectional
  request/response is native to RPC; RoCE keeps the latency. We do **not** try to force
  NVMe-oF between the tiers.

## Considered options / consequences

- **NVMe-oF (TCP or RDMA) front→executor** — rejected: empirically rejects bidirectional
  Exec on both fabrics. Switching fabric type does not help.
- **Unidirectional NVMe-oF "staging-key"** (stage request via a Store/write, fetch
  result via a Retrieve/read) — possible and keeps "NVMe all the way down", but adds a
  round-trip and a scratch-key lifecycle for what is fundamentally an RPC.
- **RPC-over-RDMA (chosen)** — Exec *is* request→compute→response; an RPC transport fits
  it, and RoCE verbs (SEND/RECV, READ/WRITE) carry it bidirectionally at low latency.
  The E810 `irdma` RoCEv2 is the hardware; testbed is `spdk-xmu`.
- ADR-0014 is **not superseded** — it is scoped to the tenant edge. This ADR defines
  the relocated inter-tier transport, which is invisible to the tenant (ADR-0012).

## Amendment (2026-06-15): RPC transport binding = libfabric (Mercury), not RoCE-verbs-specific

The original Decision named "RoCE verbs (SEND/RECV, READ/WRITE)" as the RPC-over-RDMA
transport, binding the inter-tier hop to verbs/RoCE. To keep the relocation
(ADR-0009/0012) portable across the fabrics we now target, the transport binding is
generalised to **libfabric**, with **Mercury (Mochi)** providing the RPC + bulk-RMA
layer on top. The RPC-over-RDMA decision and its empirical basis (above) are unchanged;
only the *transport binding* moves from "RoCE verbs" to "a libfabric provider."

### Why (fabrics now in scope)
- **EFA** exposes no standard RC verbs — it is SRD/datagram, reachable in full only via
  libfabric's `efa` provider. A verbs/RoCE-specific RPC stack (and NVMe-oF/RDMA itself)
  cannot run on EFA, so RoCE-verbs is disqualified as the *sole* binding.
- **Ultra Ethernet (UET)** — emerging transport whose consumer API is consolidating
  around libfabric. libfabric is the single abstraction that spans the E810/RoCE testbed
  (verbs provider) today, EFA next, and UET as it matures — same Exec/RPC code, swap the
  provider.

### Why Mercury, not raw libfabric
libfabric is a transport abstraction, not an RPC framework. EFA's SRD/datagram model vs
RoCE's RC differ in reliability, max message size, and RMA/MR semantics; Mercury already
abstracts those (RPC envelope + bulk via RMA across providers verbs/efa/tcp/shm) and is
proven in HPC object stores. Raw libfabric would re-solve those differences per provider.

### Scope — Exec hop only; data plane unchanged
This amendment covers ONLY the Exec inter-tier control hop. The `rados-nkv` ↔ `rados-nkvx`
**data plane stays NVMe-oF/RDMA on RoCE and falls back to NVMe/TCP on EFA** (NVMe-oF/RDMA
also cannot run on EFA). Committing the data plane to libfabric as well (DAOS-style,
single fabric stack everywhere) is explicitly **deferred** — revisit only if EFA
data-plane RDMA performance becomes load-bearing.

### Consequences
- **`rados-nkv` (front) is an SPDK reactor process** → drive libfabric/Mercury progress
  from an SPDK poller (manual-progress mode) and register the request/result/object
  buffers with libfabric MR aligned to SPDK's hugepage/DMA memory. This is the main
  integration tax; poll-mode is compatible with the reactor.
- **`rados-nkvx` (executor) runs as a standalone service** (Mercury server in the storage
  VM), *not* an SPDK reactor — keeps the libfabric/Mercury side clean and the wasmtime
  executor decoupled from SPDK. Decided 2026-06-15. The M.2 content-addressed cache
  (TB4) on the executor side is therefore accessed without an SPDK bdev stack (raw
  block / mmap / a non-SPDK NVMe path); if that cache later needs SPDK bdev or native
  RDMA, revisit this as its own decision.
- Two transport stacks coexist on the front node on the RoCE testbed (SPDK verbs for the
  NVMe-oF/RDMA data plane; libfabric/Mercury for the Exec hop). On EFA the data plane is
  NVMe/TCP, so the SPDK verbs stack is RoCE-testbed-only.
- Testbed `spdk-xmu`: the E810/RoCE path is exercised via the libfabric **verbs**
  provider; EFA and UET providers are added later without touching the Exec/RPC code.
