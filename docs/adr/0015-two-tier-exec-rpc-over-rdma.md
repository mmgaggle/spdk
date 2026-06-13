---
status: accepted
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
