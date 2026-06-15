---
status: draft (design spec)
slice: spdk-xmu Slice C
implements: ADR-0015 (+ 2026-06-15 Amendment)
mirrors-rigor-of: ADR-0014
date: 2026-06-15
---

# Slice C — inter-tier Exec RPC over Mercury/libfabric

This is the design spec for the `rados-nkv` (front) → `rados-nkvx` (executor) Exec
hop, the "relocation" ADR-0009/0012 deferred and ADR-0015 (amended) bound to
**libfabric transport + Mercury (Mochi) RPC/bulk**. Scope = the Exec inter-tier
control hop ONLY. The data plane stays NVMe-oF/RDMA on RoCE (NVMe/TCP on EFA) and is
out of scope (ADR-0015 Amendment §Scope).

It freezes the Exec RPC contract at ADR-0014-level precision so the relocation stays
invisible to the tenant (ADR-0012). The tenant edge is unchanged: bidirectional DPTR
NVMe-KV over vfio-user, key length-prefixed in the payload (ADR-0014). The front
*terminates* that and re-emits `(op-id, key, request) → (result, true_len, status)`
as a Mercury RPC.

Terminology (consistent with ADR-0009/0014, do not drift):
- **front** = `rados-nkv`, an SPDK reactor process. Today it is `kvdev_rados.c` +
  `kvdev_rados_nkvx.c` running the executor *in-process*. Under Slice C the in-process
  executor is replaced (on the two-tier path) by a Mercury client stub.
- **executor** = `rados-nkvx`, a standalone (non-SPDK) Mercury service in the storage
  VM, owning wasmtime + the TB4 M.2 content-addressed cache and librados cold-fill.
- **object** = the stored KV value bytes the module runs over (cold-filled from RADOS).
- **module** = the `(runtime, sha256, locator)` artifact (`.wasm` in v1).

---

## 1. The Exec RPC contract (FROZEN)

One Mercury RPC, `nkvx_exec`, request→compute→response. Bidirectional is native to
RPC (the ADR-0015 reason for leaving NVMe-oF). Wire encoding is Mercury's portable
`hg_proc` serialization (XDR-style, endianness-safe), NOT a C struct memcpy — the
executor is non-SPDK and may differ in ABI.

### 1.1 Request envelope (`nkvx_exec_in_t`)

Inline (small, carried in the Mercury RPC SEND, NOT bulk):

| field | type | source | notes |
|---|---|---|---|
| `op_id` | `uint32` | tenant CDW13 | opaque to executor; for tracing/telemetry only |
| `key_len` | `uint8` | front (parsed from DPTR head) | 1..255 (`SPDK_KVDEV_EXEC_KEY_MAX_LEN`, `kvdev.h:43`) |
| `key` | `uint8[key_len]` | tenant DPTR head | the RADOS oid identity — `hex(key)` in current code (`kvdev_rados_key_to_oid`) |
| `runtime` | `uint8` | binding | mirrors `enum spdk_kv_exec_runtime` (`kvdev.h:70`): WASM=1, CLS=2 |
| `module_key` | `string` | binding | cold-fetch locator key (module object name) |
| `module_ns` | `string` | binding | cold-fetch locator pool/ns (`pool` or `pool/namespace`) |
| `sha256` | `uint8[32]` | binding | the SOLE auth/integrity anchor (ADR-0010); `sha256_valid` carried as a bool flag |
| `sha256_valid` | `bool` | binding | false on legacy cls path |
| `caps` | `uint64` | binding | per-invocation capability TIER `[0,3]` (`SPDK_KV_EXEC_CAPS_TIER_MAX`, `kvdev.h:64`) |
| `osize` | `uint32` | tenant CDW12 (clamped) | host output-buffer cap; drives truncation, mirrors `output_buf_len` |
| `input_len` | `uint32` | front | length of inline input |
| `input_inline` | `opaque[input_len]` | tenant DPTR tail | per-request input bytes; inline when `input_len ≤ NKVX_INLINE_MAX` |
| `input_bulk` | `hg_bulk_t` (optional) | front | present iff `input_len > NKVX_INLINE_MAX`; an RMA handle the executor READs |

`NKVX_INLINE_MAX` (proposed **4 KiB**): inputs are typically tiny (an offset, a filter
predicate); the object is the large thing and it does NOT travel inline (see §2). The
threshold exists only so a pathologically large `input` does not bloat the SEND.

The request **does NOT carry the object bytes**. This is the load-bearing decision —
see §2. The front passes only the *key* (+ binding); the executor resolves the object.

### 1.2 Response envelope (`nkvx_exec_out_t`)

| field | type | notes |
|---|---|---|
| `status` | `int32` | carries an `enum spdk_kvdev_io_status` value (`kvdev.h:119`) as a fixed `int32` (via `hg_proc`); value-stability is a BUILD invariant, not an ABI guarantee — see §3 |
| `result_len` | `uint32` | the **TRUE** result length (Retrieve-style truncation, ADR-0014); equals tenant CQE DW0 |
| `result_inline` | `opaque[min(result_len,osize)]` | present iff `min(result_len,osize) ≤ NKVX_INLINE_MAX` |
| `result_bulk` | (no field) | large results use front-origin bulk WRITE — see §1.3 |

`result_len` is always the true length even when truncated, so the front sets CQE DW0
to it and maps `BUFFER_TOO_SMALL` exactly as the tenant edge does
(`nvmf_kvdev_complete`, `ctrlr_kvdev.c:129-134`: `BUFFER_TOO_SMALL` → `sc=SUCCESS`,
`cdw0=true_len`).

### 1.3 Bulk-transfer model — who originates, who owns

Default to **inline both directions** (input ≤ 4 KiB, result ≤ 4 KiB). Bulk RMA only
for the large cases, and the **front always originates and owns the bulk buffers**
(it is the SPDK-MR-registered side; see §4):

- **Large input** (`input_len > NKVX_INLINE_MAX`, rare): the front registers the input
  buffer as an `hg_bulk_t` (READ-mode) and ships the handle in `input_bulk`. The
  **executor issues `HG_Bulk_transfer(PULL)`** to read it into a local buffer. Buffer
  ownership: front owns the source buffer and keeps it pinned until the RPC completes
  (the request-complete callback releases it).
- **Large result** (`result_len > NKVX_INLINE_MAX`, the common large-KV case — 64 MiB
  KV cache tensors are the whole point of this stack): the front pre-registers its
  *host output buffer* (the tenant DPTR, already a DMA buffer) as a WRITE-mode
  `hg_bulk_t` and ships **that handle in the REQUEST** (`result_sink` field, added to
  §1.1 when large results are enabled). The **executor issues `HG_Bulk_transfer(PUSH)`**
  to write its result directly into the front's host buffer, then returns `status` +
  `result_len` inline. Zero intermediate copy on the front; the result lands in the
  buffer the tenant DMA already targets.

  > **NORMATIVE — PUSH-before-respond ordering.** The executor MUST await *completion* of
  > the result `HG_Bulk_transfer(PUSH)` BEFORE it calls `HG_Respond`. The response is what
  > unblocks the front's completion callback, which sets the tenant CQE; if `HG_Respond`
  > races ahead of the PUSH completion, the front can fire the tenant CQE while the (up to
  > 64 MiB) result is still in flight, and the tenant reads a partially-written DPTR —
  > **silent data corruption**, not an error. Concretely: chain `HG_Respond` from the
  > PUSH-completion callback (or block the handler on the PUSH completion), never the
  > reverse. This ordering is part of the frozen contract for the large-result path.

  > **Decision:** large-result bulk is **front-sink, executor-push**. The front owns
  > the only DMA-registrable memory (SPDK hugepages); the executor's result buffer is
  > plain heap. Originating the bulk WRITE from the executor means the front registers
  > once (its tenant DPTR) and the executor streams into it — symmetric with how the
  > tenant edge overwrites the DPTR in place (ADR-0014). The alternative (executor
  > registers result, front PULLs) forces MR on the non-SPDK side and a front-side
  > copy; rejected.

  Add `result_sink` (optional `hg_bulk_t`) to the request envelope so the executor
  knows whether to push or inline. When absent, the executor inlines and the front
  copies (small-result path). When present, the executor pushes and `result_inline` is
  empty.

Object bytes are **never** a bulk handle on this RPC: the object lives on the executor
side (§2), so it never crosses the fabric in the Exec hop.

### 1.4 What's frozen vs open

FROZEN: field set, the inline-vs-bulk split, front-origin bulk ownership, `result_len`
= true length truncation semantics, and the *wire shape* of status (a fixed `int32`).

NOT frozen by ADR: the numeric VALUES of `enum spdk_kvdev_io_status` on the wire. ADR-0014
freezes the tenant SQE/CQE ABI; it does NOT freeze this internal enum. `enum
spdk_kvdev_io_status` is a plain C enum (int-width, negative values -1..-9) and is NOT
ABI-frozen — an enum-value bump in a future `kvdev.h` would silently re-map the tenant
CQE. See §3 for how this is made safe by a build invariant + a static-assert mapping
table, NOT by an ABI claim.

OPEN QUESTIONS:
- **OQ-1** `NKVX_INLINE_MAX` value (4 KiB proposed). Needs a measured input/result size
  histogram from the real KV-cache workloads.
- **OQ-2** Whether `op_id` should be dropped from the wire (executor ignores it) or
  kept for executor-side telemetry. Kept for now (cheap, aids tracing).
- **OQ-3** Idempotency/retry token. Mercury RPCs can be retried on transient fabric
  error; Exec is read-only (ADR-0014) and the executor result cache is content-keyed,
  so a retry is naturally idempotent — but we should carry a client request-id for
  dedupe/observability. Deferred to error-mapping slice (§6).

---

## 2. Front vs executor responsibility split — and the object-flow decision

### 2.1 The decision (object flow): **front sends only the KEY; the executor cold-fills + caches the object.**

This is the central design question and it is decided in favor of **key-only**:

- **ADR-0009 already mandates it.** The executor "owns a content-addressed local-NVMe
  namespace and cold-fills from RADOS via plain librados on the cold path only" — the
  OSD↔executor interface is "unprivileged librados reads on the cold path only." The
  TB4 content-addressed cache is *on the executor side* (the storage-VM M.2 over TB4,
  ADR-0015 Amendment §Consequences). Shipping object bytes from the front would
  relocate the cache to the wrong tier and re-introduce the copy ADR-0009 eliminated.
- **The cache is useless if the front cold-fills.** The whole TB4 value
  (`kvdev_rados_nkvx_wasm_run_cached`, the cold-fill-once-then-zero-copy property the
  TB4 acceptance test asserts — `kvdev_rados_nkvx_wasm.h:155-177`) is that the second
  Exec of an object does NO librados refetch and the cached buffer backs wasm linear
  memory zero-copy. If the front cold-fills and ships bytes, every Exec pays a full
  64 MiB cold-read + a 64 MiB fabric transfer, defeating both the cache AND the
  zero-copy DMA-into-linear-memory.
- **Data locality / CRUSH routing.** Two-tier exists so Exec runs on the host that owns
  the data (ADR-0009 "CRUSH-routed to the storage host"). The executor's librados sees
  the object host-locally; the front may be a different host. Key-only keeps the big
  bytes off the fabric entirely on the Exec hop.
- **64 MiB makes it decisive.** A 64 MiB object shipped per-Exec over the fabric would
  dominate latency and saturate the link; the data-plane (NVMe-oF/RDMA) is separately
  optimized for that and is explicitly NOT this hop.

**Consequence — the front's current cold-fill code moves to the executor.** Today the
*front* does the librados cold-fill (`kvdev_rados.c:1483` `rados_read_op_read`, the
`start_data_phase`/`dispatch_or_fail` path) because the executor is in-process. Under
Slice C the cold-fill (`kvdev_rados_nkvx_start_data_phase`, `kvdev_rados_nkvx_module_ioctx`,
the module fetch at `kvdev_rados.c:1556`) **relocates to the executor**. The front's
two-tier path becomes: parse DPTR → build `nkvx_exec_in_t` → Mercury RPC → on response,
set CQE. The front retains librados only for the *single-tier* (co-located) path, which
stays exactly as it is today (ADR-0009 "co-located OR CRUSH-routed, no tenant change").

### 2.2 Responsibility table

| concern | front (`rados-nkv`, SPDK) | executor (`rados-nkvx`, standalone) |
|---|---|---|
| tenant NVMe-KV Exec termination, DPTR parse (key+input) | ✅ (`ctrlr_kvdev.c:446-542`) | — |
| **allowlist / op_id → binding resolution** | ✅ **authoritative** (`nvmf_ns_kv_exec_op_allowed`, `ctrlr_kvdev.c:479`) | re-validates the binding it receives |
| caps TIER → resource budget enforcement | passes `caps` tier | ✅ enforces (fuel/epoch/memory; the existing TB2 caps machinery) |
| read-only invariant | passes `read_only`; gate at `ctrlr_kvdev.c` | ✅ enforces at mutation point (ADR-0014 refinement); rejects mutating paths |
| module fetch (librados) + **sha256 verify** + compile | — | ✅ (relocated; `kvdev_rados_nkvx_wasm_module_verify/insert`) |
| **object cold-fill (librados)** + content cache + zero-copy DMA | — (two-tier) / ✅ (single-tier only) | ✅ (TB4 cache, `wasm_run_cached`) |
| wasmtime run, off-reactor isolation | — | ✅ (it IS the reactor-free side now) |
| **result cache** (`spdk-k3z`) | — | ✅ moves to executor (it has object CONTENT in hand) |
| status → NVMe CQE mapping | ✅ (`nvmf_kvdev_complete`, §3) | returns `enum spdk_kvdev_io_status` |
| Mercury progress | ✅ from SPDK poller (§4) | own progress loop (§4) |

**Allowlist split — front authoritative, executor defensive.** The control-plane
allowlist (ADR-0014, `nvmf_ns_set_kv_exec_allowlist`) is *per (subsystem, nsid)* state
that lives on the front (it terminates the tenant). The front MUST gate (default-deny)
before any RPC — exactly as today (`ctrlr_kvdev.c:479-486`). The executor does NOT have
the per-namespace allowlist and must NOT be trusted to enforce tenancy. But the
executor independently enforces the *binding's own* invariants it can verify: the
sha256 gate (only bytes matching the bound hash are ever compiled/run — the
deny-by-default authorization anchor, ADR-0010) and the read-only mutation gate. So:
**allowlist membership = front only; binding integrity (hash) + read-only = both, with
the executor as the hard enforcement point for what it can prove.**

> Security note for the adversarial reviewer: the executor accepts a `(sha256, locator,
> caps)` from whichever front connects. A compromised/rogue front cannot make the
> executor run un-hashed bytes (hash gate) or mutate (read-only gate), but it CAN ask
> the executor to fetch+run any allow-listable module against any key it can name. The
> trust boundary is the **fabric authentication** (which fronts may connect) — see §4
> address/DAC and OQ-4. The allowlist is NOT a fabric-level ACL; it is tenant→op
> policy on the front. This is consistent with ADR-0009's trust model (the executor is
> the sandbox; the front is the policy point) but MUST be stated, not assumed.

OPEN QUESTIONS:
- **OQ-4** Fabric-level authn: which fronts may issue `nkvx_exec` to a given executor.
  Mercury/libfabric has no built-in authz; needs an allow-list of peer addresses or a
  shared token in the envelope. Two distinct exposures with no authn: (1) **confidentiality
  / integrity** — any peer that can reach the executor can run any allow-listable module
  against any key it can name (bounded by the hash + read-only gates, but not by tenancy);
  and (2) **availability / DoS** — a rogue or merely buggy peer can flood `nkvx_exec` to
  exhaust the executor's TB4 object cache, result cache, librados connections, and
  wasmtime resources, degrading every legitimate front. **Acceptable for the
  single-host/DAC testbed** (trusted peers, one front); **blocking for anything beyond
  the testbed** (multi-tenant, shared fabric, or untrusted hosts). Flagged as a real
  trust-boundary gap, not deferred-and-forgotten.
- **OQ-5** Does the executor need the per-namespace read-only bit, or is read-only a
  property of the *binding* (runtime/module)? Today `read_only` is threaded per-op
  (`spdk_kvdev_exec(..., bool read_only, ...)`). Carry it in the envelope as a bool.

---

## 3. Error / status mapping

The executor returns an `enum spdk_kvdev_io_status` (`kvdev.h:119`) value, carried on the
wire as a fixed `int32` via `hg_proc`; the front feeds it into the EXISTING
`nvmf_kvdev_complete` switch (`ctrlr_kvdev.c:109-164`) — **no new mapping code on the
front**, the RPC status IS the kvdev status. This is the point of keeping the wire status
= the internal enum.

**Making the wire status safe (this is NOT an ABI guarantee).** `enum spdk_kvdev_io_status`
is a plain C enum (int-width, negative values -1..-9). ADR-0014 freezes the tenant
SQE/CQE ABI — it does NOT freeze this internal enum, so we must not lean on ADR-0014 to
call the wire status "frozen." We make it safe by two explicit mechanisms instead:

1. **Build invariant.** The executor MUST be built against the *identical* `spdk/kvdev.h`
   as the front (same enum definition, same numeric values). Value-stability across the
   hop is therefore a build/packaging invariant of the two-tier deployment, NOT a
   property guaranteed by the wire format. A front and executor built from divergent
   `kvdev.h` are an unsupported configuration.

2. **Static-assert-backed front-side mapping table.** The front does NOT blindly cast the
   incoming `int32` to `enum spdk_kvdev_io_status`. It runs the wire value through an
   explicit table that maps each known wire code to its `enum spdk_kvdev_io_status`
   constant, with a `SPDK_STATIC_ASSERT` per row pinning the expected numeric value
   (mirroring the existing size-assert discipline at `kvdev.h:246`,
   `SPDK_STATIC_ASSERT(sizeof(struct spdk_kvdev_store_opts) == 16, ...)`). For example:

   ```c
   SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_SUCCESS          ==  0, "wire status drift");
   SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_FAILED           == -1, "wire status drift");
   SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST    == -2, "wire status drift");
   SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL == -3, "wire status drift");
   SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_INVALID          == -4, "wire status drift");
   SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_NOMEM            == -5, "wire status drift");
   SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_KEY_EXIST        == -6, "wire status drift");
   SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED    == -7, "wire status drift");
   SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_ABORTED          == -8, "wire status drift");
   SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_READ_ONLY        == -9, "wire status drift");
   ```

   An enum-value bump in a future `kvdev.h` then breaks the *build* (assert fires) instead
   of silently corrupting a tenant CQE. An unrecognized wire value (outside the known set)
   is treated as a transport-level fault and synthesized to `FAILED` → `INTERNAL_DEVICE_ERROR`
   (per the front-side synthesis rule below), never cast through.

| executor condition | kvdev status | NVMe CQE (sct/sc) at tenant | source |
|---|---|---|---|
| ok | `SUCCESS` (0) | GENERIC / `SUCCESS`, CQE DW0 = `result_len` | `ctrlr_kvdev.c:118` |
| result > host buffer (truncated) | `BUFFER_TOO_SMALL` (-3) | GENERIC / `SUCCESS`, DW0 = TRUE len | `:129` |
| op_id not allow-listed | (never reaches RPC) | GENERIC / `INVALID_OPCODE` | front gate `:479-486` |
| module not in allowlist binding / bad locator | `INVALID` (-4) | GENERIC / `INVALID_VALUE_SIZE` | `:135` |
| **HASH_MISMATCH** (fetched bytes ≠ bound sha256) | `INVALID` (-4) | GENERIC / `INVALID_VALUE_SIZE` | `:135`; see OQ-6 |
| **NOT_ALLOWED** (read-only namespace, mutating path) | `READ_ONLY` (-9) | CMD_SPECIFIC / `ATTEMPTED_WRITE_TO_RO_RANGE` | `:152-158` |
| caps exceeded (fuel/epoch/memory) | `ABORTED` (-8) | GENERIC / `ABORTED_BY_REQUEST` | `:146-150` |
| runtime unavailable (no wasmtime) | `NOT_SUPPORTED` (-7) | GENERIC / `INVALID_OPCODE` | `:141-144` |
| object-not-found (key absent in RADOS) | `KEY_NOT_EXIST` (-2) | GENERIC / `KV_KEY_DOES_NOT_EXIST` | `:123` |
| executor OOM / cache full | `NOMEM` (-5) | GENERIC / `CAPACITY_EXCEEDED` | `:138` |
| module trap / generic run failure | `FAILED` (-1) | GENERIC / `INTERNAL_DEVICE_ERROR` | `:160` |
| **fabric/RPC transport failure** (timeout, peer down, NAK) | (front-synthesized) | GENERIC / `INTERNAL_DEVICE_ERROR` | NEW, front-side |

**New front-side cases (not from the executor enum):**
- A Mercury RPC that fails to complete (HG error, addr lookup fail, timeout, bulk
  transfer error) has no kvdev status. The front synthesizes `FAILED` →
  `INTERNAL_DEVICE_ERROR`. **Decision:** transport errors map to `FAILED`/internal, NOT
  to `NOT_SUPPORTED` — the operation is valid, the fabric failed; the tenant should see
  a retryable device error, not "unsupported opcode."
- **OQ-6** `HASH_MISMATCH` currently collapses into `INVALID`
  (`kvdev_rados_nkvx_wasm_module_verify` returns `SPDK_KVDEV_IO_STATUS_INVALID` on
  mismatch). It is indistinguishable at the tenant from a malformed request. Acceptable
  (the tenant cannot fix either), but consider a distinct executor-side log/telemetry
  code so an operator can tell a hash mismatch (provisioning bug / tamper) from a bad
  request. No new tenant-visible status.

---

## 4. Mercury / libfabric integration

### 4.1 Layering

```
front (rados-nkv, SPDK reactor)          executor (rados-nkvx, standalone)
  Mercury HG class (origin)                Mercury HG class (target)
  Margo? NO — manual HG (see below)        Margo (argobots) progress loop OK here
  libfabric NA plugin (na_ofi)             libfabric NA plugin (na_ofi)
  provider: verbs|tcp|shm                  provider: verbs|tcp|shm
```

- **RPC + bulk = Mercury** (`mercury_core` / `HG_*` + `HG_Bulk_*`). It abstracts the
  RC-vs-SRD, max-message, and MR/RMA differences across providers (ADR-0015 Amendment
  §Why Mercury).
- **Transport = libfabric** via Mercury's `na_ofi` plugin. Provider selected by the
  Mercury init address string: `"ofi+verbs://"` (E810 testbed), `"ofi+tcp://"` /
  `"ofi+shm://"` (dev path, §5), `"ofi+efa://"` (later).
- **Do NOT use Margo on the front.** Margo couples Mercury progress to Argobots
  user-level threads with its own scheduler — incompatible with the SPDK reactor
  (which owns the thread and runs a poll loop). The front uses **raw Mercury in
  manual-progress mode** driven from an SPDK poller. The *executor* is standalone and
  MAY use Margo (or a plain `HG_Progress`/`HG_Trigger` loop) — its choice, decoupled.

### 4.2 Front progress model (the main integration tax — ADR-0015 §Consequences)

- Register an SPDK poller (`spdk_poller_register`) on the front reactor that, each tick,
  calls `HG_Progress(hg_context, 0 /*non-blocking, timeout=0*/)` then
  `HG_Trigger(hg_context, 0, max, &actual)` to fire completed RPC callbacks **on the
  reactor thread**. This is the manual-progress pattern ADR-0015 names; it keeps the
  Mercury completion callback on the SPDK thread, so the front can call
  `spdk_nvmf_request_complete` directly from the RPC response handler (no cross-thread
  handoff — unlike today's off-reactor worker, which the executor now owns).
- The poller is **non-blocking** (`timeout=0`) so it never stalls the reactor. Busy-poll
  cost is acceptable on a dedicated reactor; if it shows up, gate the progress call
  behind "any RPC in flight."
- **Completion semantics:** an `nkvx_exec` RPC forge `HG_Forward(handle, cb, ctx, &in)`;
  the `cb` runs from `HG_Trigger` on the reactor and: reads `out`, (if bulk-sink was
  used the result is already in the host buffer), copies `result_inline` into the host
  buffer if inline, sets CQE status+DW0, completes the NVMf request, frees the handle.

### 4.3 Memory registration (MR) — the realism question

- The bulk path (§1.3) registers the front's **tenant DPTR host buffer** as an
  `hg_bulk_t`. That buffer is SPDK DMA memory (hugepage-backed, from
  `spdk_dma_zmalloc` on the host, surfaced through the vfio-user DPTR). libfabric's
  verbs provider will `ibv_reg_mr` it; hugepage-backed, physically-pinned SPDK memory
  is exactly what verbs MR wants, so this aligns. **Gotcha:** the MR domain
  (`fid_domain`) Mercury/`na_ofi` opens must be on the same verbs device
  (`irdma`/E810) the SPDK NVMe-oF stack uses — but they are *separate* `ibv_context`s
  (two stacks coexist on the front per ADR-0015 §Consequences). Two MRs of the same
  pages on two devices is fine; registering on the wrong device is not. Pin the
  provider to the E810 by `fi_info` domain name in the Mercury init string.
- **MR registration latency — cache from day one.** `ibv_reg_mr` of a 64 MiB DPTR is not
  free: it pins and walks ~16K 4-KiB pages on *every* large Exec — a perf cliff this
  design already predicts, because the common case is a *reused* tenant DPTR (the same
  DMA buffer recurs across Execs → it should hit a cache, not re-register). An
  MR/`hg_bulk_t`-handle cache keyed by buffer address (Mercury `HG_Bulk` handle reuse
  and/or libfabric's MR cache) is therefore **part of C7**, not a deferred maybe.
  Register-per-Exec exists only as the functional-first rung (get bytes landing
  correctly, then turn the cache on). What remains open is the cache *sizing and eviction
  policy* (entry count, lifetime vs DPTR reuse pattern, invalidation on buffer free —
  tied to the C6 abort/teardown deregistration ordering) — **OQ-7**, measured.
- Inline path needs no MR (data rides in the SEND). This is why the 4 KiB threshold
  matters: most Execs avoid MR entirely.

### 4.4 Connection setup / addressing (via the DAC)

- Mercury addresses: the executor self-addresses (`HG_Addr_self` →
  `HG_Addr_to_string`) and publishes its address string (e.g. to a file / a small
  bootstrap RPC / the control plane). The front does `HG_Addr_lookup` on that string.
- On the E810 testbed the verbs provider addresses are tied to the RoCE GIDs on the
  **DAC** (direct-attach copper) link between front and storage VM. The address string
  encodes the provider + the executor's fabric address; lookup over verbs needs the
  RoCE path (GID/port) reachable — the same DAC the data-plane NVMe-oF/RDMA validated.
- **Gotcha:** verbs provider connection setup (RC QP creation) is heavier than tcp/shm;
  first-RPC latency includes a connection establishment. Mercury caches the connection
  per peer address, so amortized. The standalone executor must be **up and listening
  before** the front looks it up (bootstrap ordering — OQ-8).

OPEN QUESTIONS:
- **OQ-7** MR-per-Exec vs MR cache (measure).
- **OQ-8** Executor address discovery/bootstrap mechanism (file vs control-plane RPC vs
  static config). Start with a file/env on the testbed; productionize later.
- **OQ-9** Whether the front needs >1 outstanding `nkvx_exec` (concurrency). Mercury
  supports many in-flight handles on one context; the SPDK poller triggers them all.
  Need a per-front handle pool sized to the NVMf qdepth. Likely yes.

---

## 5. CPU / loopback dev path (no storage-VM hardware)

Two functional rungs before RDMA, selectable purely by the Mercury init provider
string (same Exec/RPC code — ADR-0015 Amendment "swap the provider"):

1. **`ofi+shm://` (or `ofi+tcp://`) — two local processes, one box.** Front and
   executor as two processes on the dev workstation (no SPDK reactor required for a
   pure-RPC harness; a minimal main can stand in for the front to exercise the contract,
   then the real SPDK front once the poller integration lands). **Proves:** the RPC
   contract (envelope encode/decode), inline+bulk paths (shm supports `HG_Bulk`), the
   front/executor split, the error/status mapping, the executor's fetch→verify→run→cache
   pipeline, result-cache relocation — i.e. EVERYTHING except RDMA-specific MR/QP
   behavior. CI-able with no special hardware. `tcp` additionally proves the
   wire-serialization survives a real socket (endianness, fragmentation).

2. **`ofi+verbs://` over the validated E810 DAC — the RDMA path.** Same code, verbs
   provider, front (SPDK reactor) ↔ executor (storage VM). **Proves:** `ibv_reg_mr` of
   SPDK hugepage DPTR buffers, the front-sink/executor-push 64 MiB bulk WRITE, the
   poller-driven manual progress under real verbs completion semantics, RC connection
   setup over the DAC, two-stack coexistence (SPDK verbs data plane + libfabric verbs
   Exec hop) on the same E810. This is the rung that validates the ADR-0015 thesis on
   real hardware; `efa`/`uet` are later rungs with no Exec/RPC code change.

The shm/tcp rung is the build/test gate for every Slice C bead below; the verbs rung is
the acceptance gate for the slice.

---

## 6. Implementation decomposition (bead candidates)

Ordered; each names scope, dependency, validation. Sub-slices C1..C8.

**C1 — vendor + build libfabric and Mercury into the tree/build.**
Scope: add libfabric + Mercury (Mochi `mercury`, optionally `mochi-margo` for the
executor only) as build deps; a `--with-mercury` configure knob (mirror `--with-wasm`'s
dlopen-or-graceful-degrade discipline so a stock SPDK build is unaffected); wire into
`mk/`/`configure`. Decide vendored-submodule vs system-pkg (Mercury has a real CMake
build — likely system/pkg-config like rbd, NOT in-tree compile). Depends: none.
Validate: `configure --with-mercury` builds; `HG_Init("na+sm://", ...)` smoke test
links and runs; stock build (`--without-mercury`) byte-identical.

**C2 — executor standalone service skeleton.**
Scope: a new non-SPDK binary `rados-nkvx` (likely `app/nkvx/` or `module/kvdev/rados/
nkvx_service/`): Mercury target init, register the `nkvx_exec` RPC (handler stub
returns `NOT_SUPPORTED`), a progress loop (Margo or `HG_Progress`/`HG_Trigger`),
address publish (OQ-8). Reuses the EXISTING `kvdev_rados_nkvx_wasm.c` /
`kvdev_rados_nkvx.c` executor + cache code, compiled OUT of the SPDK reactor context
(it is already off-reactor by design). Depends: C1. Validate: starts, publishes addr, a
`tcp` ping RPC round-trips and returns `NOT_SUPPORTED`.

**C3 — RPC handlers + contract structs (the freeze, in code).**
Scope: the `nkvx_exec_in_t`/`nkvx_exec_out_t` `hg_proc` serializers (§1), inline paths
only first. Executor handler: decode → (stub) run a built-in → encode response. Front:
a thin `nkvx_rpc_client.c`. Depends: C2. Validate: over `tcp`/`shm`, a built-in
`bytecount`/`identity` Exec round-trips inline; status enum survives the wire;
truncation (`result_len` > `osize`) reports true length.

**C4 — front Mercury client + SPDK-poller progress integration.**
Scope: register the SPDK poller calling `HG_Progress`/`HG_Trigger` (§4.2); wire the
two-tier branch of `kvdev_rados_exec` to build `nkvx_exec_in_t` and `HG_Forward`
instead of in-process dispatch; completion cb sets CQE + completes the NVMf request on
the reactor. Single-tier path untouched. Depends: C3. Validate: `kv_host nkvx-exec`
against a front whose executor is a *separate local process* over `tcp` — the existing
e2e assertions (bytecount/identity correctness, reactor-not-blocked latency probe) pass
with the executor across an RPC instead of in-process.

**C5 — object/cache path on the executor (the object-flow decision in code).**
Scope: relocate cold-fill + module fetch to the executor: the executor opens its own
librados (cold path only, ADR-0009), runs `kvdev_rados_nkvx_wasm_run_cached` (TB4 cache
+ zero-copy), and the result cache moves here (it has object CONTENT). The front stops
cold-filling on the two-tier path.

**BLOCKER — cross-hop cache coherence (was OQ-10).** Store/Delete are *data-plane* ops:
they terminate on the FRONT (NVMe-oF/RDMA) and **never reach the remote executor**.
Relocating the TB4 object cache to the executor therefore leaves it with NO invalidation
path — after a tenant overwrites or deletes a key, the executor's cached object bytes go
**stale**, and the next Exec runs the module over the old value. Critically, the
content-keyed RESULT cache does NOT rescue this: `result_probe` keys on whatever bytes the
(possibly stale) object cache hands it, so a stale object yields a stale-but-self-consistent
result-cache hit — a *wrong answer that looks valid*, never an error. C5 is therefore
**gated** on an executor-side invalidation mechanism, one of:
- a `rados_watch` on each cached object so the executor sees Store/Delete notifications
  from RADOS directly and evicts (preferred — keeps the executor self-sufficient, no
  new front→executor coupling), OR
- an explicit invalidate RPC the front issues across the hop on every Store/Delete to a
  key the executor may have cached (simpler, but adds a front→executor control path and a
  consistency window).

**Decision (2026-06-15): start with `rados_watch`, with a documented scale caveat and a
named successor.** Rationale and bounds:
- The live watch count is bounded by **resident TB4 cache entries**, not total keys: the
  executor watches an object only when it admits it to the cache and drops the watch on
  eviction, so watches ≈ what is currently cached (already capped by the cache). For the
  64 MiB KV-cache tensors that motivate this stack that is a modest number; the scale risk
  is the *many-small-objects* regime.
- This is an **atypical `rados_watch` usage** — most consumers (e.g. RGW metadata) hold a
  handful of long-lived watches, not one-per-cached-blob — so watch scale (per-OSD watch
  load, notify/ping overhead, watch re-establishment on osdmap change) is a **known
  limitation that may force a change**.
- Likely successor if it doesn't scale: **lazy version-validation, not the invalidate RPC.**
  The cold-fill already `stat`s the object, so a cache entry can carry the RADOS object
  version/mtime and an Exec re-`stat`s (cheap) to detect staleness — pull-validation
  bounded by one stat per Exec, still executor-self-sufficient, no watch-count ceiling. The
  explicit front→owning-executor invalidate RPC is the third fallback; note it is **NOT a
  front-to-front collective** (each key has a single owning executor under CRUSH, so it is
  point-to-point from the mutating front to that executor) but it is *blind-per-Store* (the
  front cannot know whether the key is cached) and re-couples the data plane to the control
  plane, which is why it is not first choice.

**Cardinality across multiple executors (cache instances) — when a collective is needed.**
All of the above assume the **CRUSH single-owner invariant**: each key has exactly one
owning executor (co-located with its data via CRUSH-to-primary, one executor per host), so
a mutation reaches that one executor with NO executor-to-executor coordination. A cross-
executor *collective* invalidation is needed ONLY when single-owner is relaxed and the same
key can be cached on several instances: **read-from-replica** Exec routing (the realistic
one — spreading Exec load over a key's replicas), **sharded executors** (>1 per host), or
non-CRUSH/load-balanced routing where a mutation cannot name the owner.
- Broadcasting every Store/Delete to every executor is O(stores × executors) — the wrong
  cardinality.
- **Binned invalidation** is the coarsening lever for that regime: partition the keyspace
  into B bins (e.g. `hash(key) mod B`); each executor tracks which bins it holds cached
  content for; a mutation invalidates `bin(K)` via a collective/gossip carrying only bin
  IDs, and each executor evicts (or marks-for-revalidation) its entries in that bin. Cost:
  **false invalidations within a bin** (cache thrash); coarser bins → less coordination
  state/traffic but more thrash. It has a clean `rados_watch` analog that ALSO bounds the
  watch count: watch one **per-bin epoch object** instead of one-per-cached-object →
  O(bins) watches, RADOS still does the fan-out (executors don't talk to each other), at
  the price of a per-Store epoch bump (re-introduces some data/control coupling) and the
  same bin false-positives.
- **Re-stat validation sidesteps cardinality entirely** (no watches, collectives, or bins)
  and is the preferred escape when ownership is single but scale is the worry; binning earns
  its keep specifically when *push* invalidation is required under *multi-owner* routing.

Recorded hierarchy: **single-owner → per-object `rados_watch` (C5 now), re-stat if the
count bites; multi-owner → binned invalidation (gossip or per-bin-epoch watch).** C5b's
mechanism stays selectable; C5a (relocate the pipeline) is invalidation-agnostic.

Depends: C4. Validate: the TB4 cache acceptance proof (2nd Exec of same key does 0
librados refetch — `cold_fills` counter) holds *with the executor remote*; **AND** a
tenant Store/Delete to a cached key invalidates the executor object cache so the next
Exec returns the NEW value (assert the stale-read does NOT occur, including the
result-cache-masks-it case) — C5 does not pass without this.

**C6 — error mapping + transport-failure synthesis.**
Scope: full §3 table; front-side synthesis of `FAILED` on HG/bulk/timeout error;
request-id for retry/dedupe (OQ-3); executor-side distinct HASH_MISMATCH telemetry
(OQ-6). Depends: C4. Validate: fault-injection — kill the executor mid-RPC (front sees
`INTERNAL_DEVICE_ERROR`, target stays up); a deliberately wrong-hashed module →
tenant sees `INVALID_VALUE_SIZE`; caps-exceeded → `ABORTED_BY_REQUEST`.

**C6a — Exec cancellation on tenant/front abort (use-after-free safety).** Aborts are
live in this tree (recent `lib/nvmf: fix wrong thread on ABORT retry`), so an in-flight
`nkvx_exec` can be aborted under us. Define the teardown for a tenant/front abort of an
in-flight Exec: cancel the outstanding `HG_Forward` (`HG_Cancel` on the handle), and
release the pinned WRITE bulk handle for the result sink (`result_sink` DPTR) and any
large-input READ handle. **Deregistration ordering is load-bearing:** the front's DPTR
must NOT be freed/returned while the executor may still hold (and PUSH into) the bulk
handle — that is a use-after-free / DMA-into-freed-memory. Required ordering: the front
deregisters/destroys its local `hg_bulk_t` and confirms the RPC is fully torn down
(cancellation acknowledged, or executor confirmed it dropped the handle) BEFORE it
releases the DPTR back to the pool; the executor must treat a cancelled handle as
"do not PUSH" and drop its remote view of the bulk handle. Ties into the MR/handle-cache
eviction path (§4.3, OQ-7). Validate: abort an in-flight 64 MiB Exec mid-PUSH and assert
no PUSH lands after teardown, no use-after-free (ASan/valgrind clean), the front's DPTR
is safely reusable, and the executor stays up.

**C7 — large-object/result bulk RMA.**
Scope: the front-sink/executor-push large-result path (§1.3): front registers the
tenant DPTR as a WRITE `hg_bulk_t`, ships `result_sink`; executor PUSHes (awaiting PUSH
completion before `HG_Respond`, §1.3); large-input PULL path. MR domain pinning to the
E810 verbs device. **Build the MR/`hg_bulk_t` handle cache here** (keyed by reused DPTR
address; §4.3) — register-per-Exec is only the functional-first rung; cache sizing/eviction
policy is the remaining open item (OQ-7).
Depends: C5. Validate: a 64 MiB-result Exec over `tcp`/`shm` first (functional), bytes
land correctly in the host buffer; truncation when `osize` < `result_len`. **Assert the
PUSH-before-respond ordering specifically (§1.3):** under concurrent load, every
large-result Exec returns a bytewise-correct result with NO partial/torn DPTR — i.e.
validate the `HG_Respond`-races-ahead-of-PUSH race directly (e.g. content-hash the result
buffer at CQE time across many iterations), not merely "the bytes eventually land." Also
exercises the MR/hg_bulk handle cache (below).

**C8 — RDMA test over the DAC (verbs provider).**
Scope: bring up the `ofi+verbs://` provider on the E810/DAC (§5 rung 2); validate
`ibv_reg_mr` of SPDK hugepage DPTR, RC connection setup, two-stack coexistence
(SPDK-verbs data plane + libfabric-verbs Exec hop), poller progress under real verbs
completions, the 64 MiB bulk WRITE over RoCE. Depends: C7 + the validated DAC.
Validate: the full `kv_nkvx_exec`-style e2e but two-tier over verbs; latency + the
reactor-not-blocked invariant; bytewise-correct large result over RDMA. This is the
slice's acceptance gate and the on-hardware proof of the ADR-0015 thesis.

### Open questions rollup
OQ-1 inline threshold · OQ-2 op_id on wire · OQ-3 retry/request-id · OQ-4 fabric authn
(real trust gap — confidentiality AND DoS; testbed-OK / blocking beyond) · OQ-5 read-only
in envelope · OQ-6 distinct HASH_MISMATCH · OQ-7 MR/hg_bulk handle-cache *sizing &
eviction policy only* (the cache itself is in C7 scope, not optional) · OQ-8 executor
addr bootstrap · OQ-9 in-flight concurrency / handle pool · OQ-10 **PROMOTED to a C5
blocker** (cross-hop cache invalidation on Store/Delete — executor `rados_watch` or
invalidate RPC; no longer a deferred OQ).
