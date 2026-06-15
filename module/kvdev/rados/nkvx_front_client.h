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
 * \param result_inline  inline result bytes (NULL when none / delivered via a
 *                       result_sink bulk in a later slice). Borrowed — valid
 *                       only for the duration of the callback; copy if needed.
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
 * Forward ONE nkvx_exec request (async). The request fields are consumed
 * synchronously (encoded into the SEND) before returning, so the caller may
 * free/reuse \p in immediately. The bulk handles in \p in (input_bulk,
 * result_sink) are contract-only here (transfer is Slice C7); pass HG_BULK_NULL.
 *
 * On success the RPC is in flight and \p cb will fire exactly once from a later
 * nkvx_front_progress() call. On a synchronous submission failure (handle
 * create / forward), returns negative and \p cb is NOT called.
 *
 * \return 0 if the RPC was submitted; negative errno-style otherwise.
 */
int nkvx_front_forward(struct nkvx_front *front, const nkvx_exec_in_t *in,
		       nkvx_front_done_cb cb, void *arg);

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
