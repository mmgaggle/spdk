/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation.
 *   All rights reserved.
 */

/*
 * rados-nkv FRONT Mercury client core (Slice C4). See nkvx_front_client.h and
 * docs/design/slice-c-exec-rpc-mercury.md §4 (Mercury manual-progress model).
 */

#include "nkvx_front_client.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <mercury.h>
#include <mercury_bulk.h>

/*
 * MR / hg_bulk handle cache (Slice C7.2, design §4.3 / OQ-7).
 *
 * Registering a bulk handle over a 64 MiB DPTR is not free: on the verbs
 * provider HG_Bulk_create -> ibv_reg_mr pins and page-walks ~16K pages on EVERY
 * large Exec — a perf cliff. The common case is a RECURRING tenant DPTR (SPDK
 * pool-recycles the DMA buffer), so a handle keyed by (addr,len,flags) is reused
 * across Execs instead of re-registered. Nil benefit on sm/tcp; the win is verbs.
 *
 * Bounded LRU: a fixed per-front slot array, evicting the least-recently-used
 * UNREFERENCED entry. nkvx_front is per reactor/channel and single-threaded (see
 * nkvx_front_client.h), so no locking. `refs` guards an in-flight handle from
 * eviction; a handle stays registered after refs hits 0 so the next reuse HITs.
 * If every slot is referenced at once (>= NKVX_BULK_CACHE_SLOTS concurrent
 * distinct DPTRs), the overflow handle is created uncached and freed on release.
 *
 * INVALIDATION CAVEAT (OQ-7, ties to C6 teardown): the cache keys on address and
 * cannot observe a tenant DPTR being freed/remapped. It relies on SPDK DMA
 * buffers being stable (pool-recycled, same mapping), which holds at the tenant
 * edge. Explicit invalidation-on-free is deferred (OQ-7 / C6a).
 */
#define NKVX_BULK_CACHE_SLOTS 64

struct nkvx_bulk_entry {
	void		*addr;		/* key: buffer base (HG_BULK_NULL handle == empty) */
	hg_size_t	len;		/* key: length */
	uint8_t		flags;		/* key: HG_BULK_READ_ONLY / HG_BULK_WRITE_ONLY */
	hg_bulk_t	handle;		/* the registered handle; HG_BULK_NULL if slot empty */
	uint32_t	refs;		/* in-flight Execs holding this handle */
	uint64_t	used_seq;	/* LRU stamp (front->bulk_seq at last acquire) */
};

struct nkvx_front {
	hg_class_t	*cls;
	hg_context_t	*ctx;
	hg_id_t		rpc_id;
	hg_addr_t	addr;	/* resolved executor target */

	/* MR/hg_bulk handle cache (C7.2). */
	struct nkvx_bulk_entry	bulk_cache[NKVX_BULK_CACHE_SLOTS];
	uint64_t		bulk_seq;	/* monotonic LRU clock */
	uint64_t		bulk_hits;
	uint64_t		bulk_misses;
	uint64_t		bulk_evicts;
};

/* Per-forward context, carried through HG_Forward's callback and freed there. */
struct nkvx_call {
	struct nkvx_front	*front;	/* owning front (for cache release in the cb) */
	nkvx_front_done_cb	cb;
	void			*arg;
	/*
	 * Bulk handles the front originated for this Exec (Slice C7). They must stay
	 * registered for the whole RPC — the executor PULLs the input and PUSHes the
	 * result during the call, all before it responds — so they are released only
	 * here, in the forward completion. HG_BULK_NULL when the payload rode inline.
	 * Released via the C7.2 handle cache (kept registered for reuse, not freed).
	 */
	hg_bulk_t		input_bulk;
	hg_bulk_t		result_sink;
};

/*
 * Acquire a bulk handle for [addr, addr+len) with `flags`, from the cache when a
 * matching live entry exists (no re-registration), else register a fresh one and
 * insert it (evicting the LRU unreferenced entry if the cache is full). Returns 0
 * and sets *out; negative errno on a registration failure. A handle returned when
 * the cache is full-of-in-flight-entries is UNCACHED — nkvx_bulk_release frees it.
 */
static int
nkvx_bulk_acquire(struct nkvx_front *front, void *addr, hg_size_t len,
		  uint8_t flags, hg_bulk_t *out)
{
	struct nkvx_bulk_entry *empty = NULL, *victim = NULL, *slot;
	hg_bulk_t h;
	hg_return_t ret;
	int i;

	for (i = 0; i < NKVX_BULK_CACHE_SLOTS; i++) {
		struct nkvx_bulk_entry *e = &front->bulk_cache[i];

		if (e->handle != HG_BULK_NULL && e->addr == addr &&
		    e->len == len && e->flags == flags) {
			e->refs++;
			e->used_seq = ++front->bulk_seq;
			front->bulk_hits++;
			*out = e->handle;	/* HIT: reuse, no ibv_reg_mr */
			return 0;
		}
		if (e->handle == HG_BULK_NULL) {
			if (empty == NULL) {
				empty = e;
			}
		} else if (e->refs == 0 &&
			   (victim == NULL || e->used_seq < victim->used_seq)) {
			victim = e;	/* LRU unreferenced eviction candidate */
		}
	}

	/* MISS: register a fresh handle. */
	front->bulk_misses++;
	ret = HG_Bulk_create(front->cls, 1, &addr, &len, flags, &h);
	if (ret != HG_SUCCESS) {
		return -EIO;
	}

	slot = empty ? empty : victim;
	if (slot == NULL) {
		/* Every slot is in flight: hand back an uncached handle (release frees
		 * it — it will not be found in the cache). */
		*out = h;
		return 0;
	}
	if (slot->handle != HG_BULK_NULL) {	/* reclaiming a victim slot */
		HG_Bulk_free(slot->handle);
		front->bulk_evicts++;
	}
	slot->addr = addr;
	slot->len = len;
	slot->flags = flags;
	slot->handle = h;
	slot->refs = 1;
	slot->used_seq = ++front->bulk_seq;
	*out = h;
	return 0;
}

/*
 * Release a handle acquired above: drop the cache entry's refcount (the handle
 * stays REGISTERED for the next reuse). A handle not in the cache is an overflow
 * registration and is freed now. HG_BULK_NULL is a no-op.
 */
static void
nkvx_bulk_release(struct nkvx_front *front, hg_bulk_t h)
{
	int i;

	if (h == HG_BULK_NULL) {
		return;
	}
	for (i = 0; i < NKVX_BULK_CACHE_SLOTS; i++) {
		struct nkvx_bulk_entry *e = &front->bulk_cache[i];

		if (e->handle == h) {
			if (e->refs > 0) {
				e->refs--;
			}
			return;		/* keep registered for reuse */
		}
	}
	HG_Bulk_free(h);		/* uncached overflow handle */
}

/*
 * HG_Forward completion, fired from HG_Trigger inside nkvx_front_progress() — so
 * it runs on the progressing thread (the SPDK reactor in the integrated case).
 * Decodes the response, maps the wire status, hands the result to the caller's
 * done-cb, then releases the output, the handle, and this context.
 */
static hg_return_t
nkvx_front_forward_cb(const struct hg_cb_info *info)
{
	struct nkvx_call *call = info->arg;
	hg_handle_t handle = info->info.forward.handle;

	if (info->ret != HG_SUCCESS) {
		/*
		 * Transport/RPC failure (timeout, peer down, NAK): no executor
		 * status exists. Synthesize FAILED -> INTERNAL_DEVICE_ERROR at the
		 * tenant (design §3 "New front-side cases").
		 */
		call->cb(call->arg, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
		goto out;
	}

	nkvx_exec_out_t out;
	memset(&out, 0, sizeof(out));

	hg_return_t ret = HG_Get_output(handle, &out);
	if (ret != HG_SUCCESS) {
		/* The reply did not decode: treat as a transport-level fault. */
		call->cb(call->arg, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
		goto out;
	}

	call->cb(call->arg, nkvx_status_from_wire(out.status), out.result_len,
		 out.result_inline, out.result_inline_len);

	HG_Free_output(handle, &out);

out:
	/* The executor is done with the bulk buffers by the time the forward
	 * completes (it PULLs/PUSHes before responding), so release the handles back
	 * to the C7.2 cache (kept registered for reuse; overflow handles are freed). */
	nkvx_bulk_release(call->front, call->input_bulk);
	nkvx_bulk_release(call->front, call->result_sink);
	HG_Destroy(handle);
	free(call);
	return HG_SUCCESS;
}

int
nkvx_front_init(const char *na_init, const char *target_addr,
		struct nkvx_front **out)
{
	struct nkvx_front *front;
	hg_return_t ret;

	if (out == NULL || na_init == NULL || target_addr == NULL) {
		return -EINVAL;
	}
	*out = NULL;

	front = calloc(1, sizeof(*front));
	if (front == NULL) {
		return -ENOMEM;
	}
	front->addr = HG_ADDR_NULL;

	front->cls = HG_Init(na_init, HG_FALSE /* origin, no listen */);
	if (front->cls == NULL) {
		free(front);
		return -ENOTCONN;
	}

	front->ctx = HG_Context_create(front->cls);
	if (front->ctx == NULL) {
		HG_Finalize(front->cls);
		free(front);
		return -ENOMEM;
	}

	/* Origin registers the same name/procs to obtain the matching RPC id;
	 * no handler on the origin (NULL rpc_cb). */
	front->rpc_id = HG_Register_name(front->cls, "nkvx_exec",
					 hg_proc_nkvx_exec_in_t,
					 hg_proc_nkvx_exec_out_t,
					 NULL);
	if (front->rpc_id == 0) {
		goto err;
	}

	ret = HG_Addr_lookup2(front->cls, target_addr, &front->addr);
	if (ret != HG_SUCCESS) {
		goto err;
	}

	*out = front;
	return 0;

err:
	HG_Context_destroy(front->ctx);
	HG_Finalize(front->cls);
	free(front);
	return -ENOTCONN;
}

void
nkvx_front_fini(struct nkvx_front *front)
{
	int i;

	if (front == NULL) {
		return;
	}
	/* Free every cached bulk handle. Callers drain in-flight Execs before fini
	 * (per the threading contract), so no entry should still be referenced. */
	for (i = 0; i < NKVX_BULK_CACHE_SLOTS; i++) {
		if (front->bulk_cache[i].handle != HG_BULK_NULL) {
			HG_Bulk_free(front->bulk_cache[i].handle);
		}
	}
	HG_Addr_free(front->cls, front->addr);
	HG_Context_destroy(front->ctx);
	HG_Finalize(front->cls);
	free(front);
}

void
nkvx_front_get_bulk_stats(const struct nkvx_front *front,
			  struct nkvx_front_bulk_stats *stats)
{
	if (front == NULL || stats == NULL) {
		return;
	}
	stats->hits = front->bulk_hits;
	stats->misses = front->bulk_misses;
	stats->evicts = front->bulk_evicts;
}

int
nkvx_front_forward(struct nkvx_front *front, const nkvx_exec_in_t *in,
		   void *result_sink, uint32_t result_sink_len,
		   nkvx_front_done_cb cb, void *arg)
{
	struct nkvx_call *call;
	nkvx_exec_in_t local;		/* mutable copy: carries the bulk handles */
	hg_handle_t handle = HG_HANDLE_NULL;
	hg_return_t ret;
	int rc;

	if (front == NULL || in == NULL || cb == NULL) {
		return -EINVAL;
	}

	/* A large input must come with a source buffer to register as a READ bulk;
	 * a large input_len with no input_inline would ship a request advertising
	 * input it cannot deliver (neither inline nor via PULL). Reject it. */
	if (in->input_len > NKVX_INLINE_MAX && in->input_inline == NULL) {
		return -EINVAL;
	}

	call = calloc(1, sizeof(*call));
	if (call == NULL) {
		return -ENOMEM;
	}
	call->front = front;
	call->cb = cb;
	call->arg = arg;
	call->input_bulk = HG_BULK_NULL;
	call->result_sink = HG_BULK_NULL;

	/*
	 * Originate the front-side bulk handles (Slice C7, design §1.3): the front is
	 * the registrable side, so it registers its own input buffer (READ) and host
	 * output buffer (WRITE) and ships the handles. The encode below copies them
	 * onto the wire; the executor PULLs/PUSHes against them mid-RPC.
	 */
	local = *in;
	local.input_bulk = HG_BULK_NULL;
	local.result_sink = HG_BULK_NULL;

	if (in->input_len > NKVX_INLINE_MAX && in->input_inline != NULL) {
		/* C7.2: reuse a cached MR/hg_bulk handle for this input DPTR, or
		 * register one (design §4.3); released back to the cache in the cb. */
		rc = nkvx_bulk_acquire(front, in->input_inline, in->input_len,
				       HG_BULK_READ_ONLY, &call->input_bulk);
		if (rc != 0) {
			free(call);
			return rc;
		}
		local.input_bulk = call->input_bulk;
	}

	if (result_sink != NULL && result_sink_len > NKVX_INLINE_MAX) {
		/* C7.2: reuse a cached MR/hg_bulk handle for this result-sink DPTR
		 * (the recurring tenant output buffer), or register one. */
		rc = nkvx_bulk_acquire(front, result_sink, result_sink_len,
				       HG_BULK_WRITE_ONLY, &call->result_sink);
		if (rc != 0) {
			nkvx_bulk_release(front, call->input_bulk);
			free(call);
			return rc;
		}
		local.result_sink = call->result_sink;
	}

	ret = HG_Create(front->ctx, front->addr, front->rpc_id, &handle);
	if (ret != HG_SUCCESS) {
		goto err_bulk;
	}

	/*
	 * HG_Forward encodes the request into the SEND synchronously (the proc
	 * runs now), so `local` may go out of scope on return; the registered bulk
	 * handles, however, must outlive the RPC and are freed in the completion.
	 * The completion fires later from nkvx_front_progress() via the forward cb.
	 */
	ret = HG_Forward(handle, nkvx_front_forward_cb, call, &local);
	if (ret != HG_SUCCESS) {
		HG_Destroy(handle);
		goto err_bulk;
	}

	return 0;

err_bulk:
	nkvx_bulk_release(front, call->input_bulk);
	nkvx_bulk_release(front, call->result_sink);
	free(call);
	return -EIO;
}

int
nkvx_front_progress(struct nkvx_front *front, unsigned int timeout_ms)
{
	hg_return_t ret;
	unsigned int triggered = 0;

	if (front == NULL) {
		return -EINVAL;
	}

	/* Advance the network first, then fire whatever became ready. */
	ret = HG_Progress(front->ctx, timeout_ms);
	if (ret != HG_SUCCESS && ret != HG_TIMEOUT) {
		return -EIO;
	}

	do {
		unsigned int actual = 0;
		ret = HG_Trigger(front->ctx, 0, 1, &actual);
		triggered += actual;
	} while (ret == HG_SUCCESS);
	/* HG_Trigger returns HG_TIMEOUT when the completion queue is drained. */
	if (ret != HG_TIMEOUT) {
		return -EIO;
	}

	return (int)triggered;
}
