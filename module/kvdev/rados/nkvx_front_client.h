/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * rados-nkv FRONT Mercury client core (Slice C4) — the origin side of the
 * inter-tier Exec RPC. See docs/design/slice-c-exec-rpc-mercury.md §4.
 *
 * This is deliberately SPDK-agnostic (its only SPDK include is <spdk/kvdev.h>
 * for the status enum, via nkvx_exec_rpc.h): it owns a Mercury ORIGIN class in
 * MANUAL-progress mode (design §4.1 "Do NOT use Margo on the front"), so the
 * SPDK front can drive HG_Progress/HG_Trigger from an SPDK poller on the reactor
 * thread and keep completion callbacks on that thread (design §4.2). The same
 * core is unit-tested standalone against the Slice C2 executor with no reactor.
 *
 * Threading: a struct nkvx_front is NOT thread-safe. Mercury context progress
 * must not run concurrently from two threads, so the SPDK integration owns one
 * nkvx_front PER reactor/channel (per-thread, no locking) — the SPDK-idiomatic
 * choice. All of init/forward/progress/fini for a given front run on its owning
 * thread.
 */

#ifndef NKVX_FRONT_CLIENT_H
#define NKVX_FRONT_CLIENT_H

#include "nkvx_exec_rpc.h"	/* nkvx_exec_in_t, status enum, contract procs */

#ifdef __cplusplus
extern "C" {
#endif

struct nkvx_front;

/**
 * Per-Exec completion, invoked from nkvx_front_progress() (i.e. on the caller's
 * thread — the SPDK reactor in the integrated case, design §4.2) once the
 * forwarded RPC completes.
 *
 * \param arg            caller context passed to nkvx_front_forward().
 * \param status         the executor's status already mapped back through
 *                       nkvx_status_from_wire() — a transport/RPC failure is
 *                       surfaced as SPDK_KVDEV_IO_STATUS_FAILED (design §3).
 * \param result_len     TRUE result length (may exceed osize; truncation
 *                       semantics, design §1.2). Valid only when status maps to
 *                       a delivered result; 0 otherwise.
 * \param result_inline  inline result bytes (NULL when none, or when the result
 *                       was delivered straight into the caller's sink buffer via
 *                       a result_sink bulk PUSH — in which case the bytes are
 *                       already in that buffer and nothing is passed here).
 *                       Borrowed — valid only for the callback; copy if needed.
 * \param result_inline_len  number of valid bytes at result_inline.
 */
typedef void (*nkvx_front_done_cb)(void *arg,
				   enum spdk_kvdev_io_status status,
				   uint32_t result_len,
				   const void *result_inline,
				   uint32_t result_inline_len);

/**
 * Bring up a front client: init a Mercury ORIGIN class on \p na_init (e.g.
 * "na+sm://", "ofi+tcp://", "ofi+verbs://..."), register the nkvx_exec contract,
 * and resolve the executor self-address \p target_addr (as published by
 * nkvx_service, design OQ-8). The address lookup is synchronous here (bootstrap,
 * before the reactor poller is hot).
 *
 * \return 0 and *out set on success; negative errno-style on failure (and *out
 * left NULL). The class is torn down with nkvx_front_fini().
 */
int nkvx_front_init(const char *na_init, const char *target_addr,
		    struct nkvx_front **out);

/** Tear down a front client (free address, destroy context, finalize class). */
void nkvx_front_fini(struct nkvx_front *front);

/**
 * MR/hg_bulk handle-cache counters (Slice C7.2). A `hit` reused a registered
 * handle for a recurring DPTR (no ibv_reg_mr); a `miss` registered a fresh one;
 * an `evict` freed an LRU unreferenced entry to make room. Steady-state DPTR
 * reuse drives hits up and keeps misses ~= the distinct-buffer count.
 */
struct nkvx_front_bulk_stats {
	uint64_t	hits;
	uint64_t	misses;
	uint64_t	evicts;
};

/** Read the handle-cache counters (for observability / acceptance tests). */
void nkvx_front_get_bulk_stats(const struct nkvx_front *front,
			       struct nkvx_front_bulk_stats *stats);

/**
 * Forward ONE nkvx_exec request (async), with the large-payload bulk RMA wired
 * (Slice C7, design §1.3). The scalar/inline fields of \p in are encoded
 * synchronously into the SEND before returning, so the \p in struct itself may
 * be freed/reused on return. The large-payload SOURCE BUFFERS, however, must
 * outlive the RPC: \p result_sink AND, for large input (input_len >
 * NKVX_INLINE_MAX), the buffer \p in->input_inline points at. Both are
 * registered as bulk handles the executor accesses ASYNCHRONOUSLY during the
 * call (it PULLs the input and PUSHes the result on later progress ticks) and
 * are released only when \p cb fires. The caller MUST leave \p in's bulk handles HG_BULK_NULL — this
 * function originates and owns them (the front is the registrable side):
 *
 *   - Large input: when in->input_len > NKVX_INLINE_MAX, \p in->input_inline is
 *     registered as a READ-mode bulk and the executor PULLs it. (Small input
 *     rides inline; in->input_inline carries it as before.)
 *   - Large result: when \p result_sink_len > NKVX_INLINE_MAX, \p result_sink is
 *     registered as a WRITE-mode bulk and shipped so the executor PUSHes the
 *     result straight into it. The executor PUSHes only when the result actually
 *     exceeds the inline cap; a small result still returns inline (the done-cb
 *     then carries result_inline). \p result_sink is the caller's host output
 *     buffer (the tenant DPTR) and \p result_sink_len its capacity (== osize);
 *     pass (NULL, 0) for a guaranteed-small result.
 *
 * The bulk handles stay registered until the RPC completes and are released in
 * the completion path, so \p result_sink and (for large input) the buffer at
 * \p in->input_inline must remain valid until \p cb fires.
 *
 * On success the RPC is in flight and \p cb will fire exactly once from a later
 * nkvx_front_progress() call. On a synchronous submission failure (bulk register
 * / handle create / forward), returns negative and \p cb is NOT called.
 *
 * \return 0 if the RPC was submitted; negative errno-style otherwise.
 */
int nkvx_front_forward(struct nkvx_front *front, const nkvx_exec_in_t *in,
		       void *result_sink, uint32_t result_sink_len,
		       nkvx_front_done_cb cb, void *arg);

/**
 * Cancel EVERY in-flight Exec on \p front (channel-destroy teardown drain).
 *
 * BEST-EFFORT, ORIGIN-SIDE ONLY (NOT a full UAF guarantee): HG_Cancel() cancels
 * the ORIGIN-side send/recv on each handle so the forward resolves to a terminal
 * completion (HG_CANCELED -> ABORTED) from a later progress tick, where the bulk
 * handles are released and the call ctx freed. It does NOT reach into the
 * executor: the executor has no do-not-PUSH awareness and may still PUSH into the
 * registered result_sink AFTER the origin cancels (tracked by bead spdk-5ia).
 * This is therefore safe ONLY at channel-destroy teardown, where the front AND
 * the executor are being torn down and the DPTR is not reused per-command. SAFE
 * per-command abort and on-the-verbs teardown require the executor-side cancel
 * protocol (bead spdk-5ia) and are NOT provided here.
 *
 * Idempotent. The caller progresses until nkvx_front_outstanding() reaches 0
 * (bounded — HG_Cancel forces each forward to a terminal NA completion), then, if
 * the bound is exceeded, uses nkvx_front_fail_all_pending() before fini.
 */
void nkvx_front_cancel_all(struct nkvx_front *front);

/**
 * Number of Exec forwards currently in flight on \p front (submitted, cb not yet
 * fired). For the channel-destroy drain: cancel all outstanding and progress
 * until this reaches 0 before nkvx_front_fini().
 */
unsigned nkvx_front_outstanding(const struct nkvx_front *front);

/**
 * TEARDOWN-ONLY forced completion of every still-in-flight Exec (channel-destroy
 * last resort). For each in-flight call: fire its done-cb ONCE with \p status
 * (the tenant io completes FAILED instead of hanging), release its bulk handles
 * to the cache, and remove it from the in-flight list so nkvx_front_outstanding()
 * reaches 0 before fini — no leaked call ctxs, no Mercury finalize over a live
 * in-flight list as far as our bookkeeping is concerned.
 *
 * MEMORY-SAFETY: this does NOT free the call ctx nor HG_Destroy the handle here.
 * A cancelled-but-not-yet-drained forward may still have a Mercury completion
 * pending (it carries the call as info->arg); freeing the ctx now would be a UAF
 * when that completion later triggers. Instead the call is detached (its tenant
 * cb is marked already-fired) and left for the normal forward completion to reap
 * (release-handles + HG_Destroy + free) without re-firing the tenant cb. The
 * handle teardown is thus left to HG_Finalize / the eventual trigger, which is
 * the documented-safe path (Mercury holds its own forward-side ref on the handle;
 * HG_Destroy on a non-terminal handle only drops OUR ref).
 *
 * Use ONLY after cancel_all + a bounded progress drain failed to reach 0 (a
 * wedged/dead executor). The cross-process late-PUSH residual is bead spdk-5ia.
 */
void nkvx_front_fail_all_pending(struct nkvx_front *front,
				 enum spdk_kvdev_io_status status);

/**
 * Drive Mercury progress once (design §4.2): advance the network for up to
 * \p timeout_ms, then trigger any ready completions (firing nkvx_front_done_cb
 * on this thread). The SPDK poller calls this with timeout_ms == 0 (non-blocking,
 * never stalls the reactor). A standalone caller may pass a small timeout to
 * block.
 *
 * \return the number of completions triggered this call (>= 0), or negative on
 * a fatal progress error.
 */
int nkvx_front_progress(struct nkvx_front *front, unsigned int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* NKVX_FRONT_CLIENT_H */
