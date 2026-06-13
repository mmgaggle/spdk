/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Implementation of the in-process NVMe KV host (initiator) client shim.
 *
 * Mirrors the idioms of test/nvmf/kv/kv_host.c: attach over VFIOUSER, find the
 * CSI==KV namespace, alloc an io qpair, use spdk_dma_zmalloc DMA buffers, submit
 * each op then poll the qpair to completion capturing sct/sc/cdw0.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvme_kv.h"

#include "kv_host_shim.h"

/*
 * Size of the per-op identity-token context ring (see struct kv_host_shim).
 *
 * The shim is strictly synchronous: at most one op is armed at a time, and an
 * orphaned tracker from a timed-out/failed op is at most ONE generation old (it
 * fires during the immediately following op's poll). A ring of this many
 * slots therefore guarantees an orphan's slot is not reused (its stored op_id
 * not overwritten) for far longer than any orphan can survive, so a late orphan
 * callback always reads back its OWN (stale) op_id and is rejected. Must be a
 * power of two so the index masks cheaply.
 */
#define KV_HOST_SHIM_OP_RING 64u

struct kv_host_shim;

/*
 * Per-op identity-token context. One of these is handed to the transport as the
 * completion cb_arg for each submitted op. It carries a back-pointer to the
 * shim and the op's unique monotonically-increasing token (op_id).
 *
 * LIFETIME: these contexts live in a fixed ring embedded in struct kv_host_shim
 * (op_ctx[]). They are NEVER freed individually -- only when the whole shim is
 * freed in kv_host_shim_close(). A late orphan callback can therefore fire
 * arbitrarily later (even after the op that submitted it has returned) and STILL
 * safely dereference its context: the memory outlives the orphan because it is
 * owned by the shim, and the slot's op_id is not overwritten until the ring
 * wraps KV_HOST_SHIM_OP_RING ops later (which an at-most-one-generation-old
 * orphan never reaches). So io_complete() never touches freed memory and never
 * reads a slot whose op_id has been recycled out from under the orphan.
 */
struct kv_op_ctx {
	struct kv_host_shim	*sh;
	uint64_t		op_id;
};

struct kv_host_shim {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct spdk_nvme_qpair	*qpair;
	/* Cached KV namespace capabilities. */
	uint32_t		kvvml;	/* max value length */
	uint32_t		kvkml;	/* max key length */
	/* Whether this shim owns the SPDK env (called spdk_env_init). */
	bool			owns_env;
	/*
	 * Set when poll_to_completion() disconnected the qpair after a timeout
	 * or transport failure. The next op reconnects it via ensure_connected()
	 * before submitting, or fails fast if the target is still down.
	 */
	bool			qpair_failed;
	/*
	 * Per-op completion state with a PER-OP IDENTITY TOKEN.
	 *
	 * The transport (vfio-user, which reuses the PCIe qpair ops) does NOT
	 * abort an outstanding HARDWARE tracker on disconnect or on reconnect for
	 * this transport: the CONNECTED->ENABLING tracker abort in nvme_qpair.c is
	 * gated on trtype == PCIE and is skipped for vfio-user. So when an op times
	 * out or transport-fails, its tracker stays outstanding and its callback
	 * (an orphan) can fire at an UNBOUNDED later poll -- including DURING the
	 * NEXT op's poll_to_completion(). A bounded drain on reconnect cannot
	 * guarantee consuming it.
	 *
	 * To make that orphan harmless we give every op a unique token:
	 *   - next_op_id is a monotonically increasing counter; each op takes the
	 *     next value as its token before submitting.
	 *   - armed_op_id holds the token of the op currently being polled.
	 *   - the op's completion cb_arg is a struct kv_op_ctx (from op_ctx[])
	 *     carrying that token.
	 * io_complete() records a completion ONLY when ctx->op_id == armed_op_id;
	 * a stale orphan carries an OLD token != armed_op_id and is DISCARDED. So
	 * an orphan from op A can never be recorded as op B's result, even if it
	 * fires in the middle of B's armed poll window.
	 */
	uint64_t		next_op_id;
	volatile uint64_t	armed_op_id;
	struct kv_op_ctx	op_ctx[KV_HOST_SHIM_OP_RING];
	volatile bool		done;
	volatile uint8_t	last_sct;
	volatile uint8_t	last_sc;
	volatile uint32_t	last_cdw0;
};

/*
 * Arm the next op: allocate a fresh identity token, stamp it into the ring slot
 * that will be used as this op's cb_arg, and publish it as the armed token.
 * Returns the per-op context to hand to the transport as cb_arg.
 *
 * The slot index round-robins over the ring. Because the shim is synchronous,
 * the slot being (re)stamped here cannot still hold a live orphan (that would
 * require an orphan to survive KV_HOST_SHIM_OP_RING ops, which cannot happen).
 */
static struct kv_op_ctx *
arm_op(struct kv_host_shim *sh)
{
	uint64_t id = ++sh->next_op_id;
	struct kv_op_ctx *ctx = &sh->op_ctx[id & (KV_HOST_SHIM_OP_RING - 1)];

	ctx->sh = sh;
	ctx->op_id = id;
	sh->done = false;
	sh->armed_op_id = id;
	return ctx;
}

/*
 * Disarm the armed op: no completion will be accepted into the status slot until
 * the next arm_op(). Setting armed_op_id to 0 (never a valid token, since
 * next_op_id is pre-incremented so the first token is 1) means io_complete()
 * rejects EVERY callback -- including a stale orphan that fires after a timeout
 * or transport failure. Idempotent and cheap.
 */
static void
disarm_op(struct kv_host_shim *sh)
{
	sh->armed_op_id = 0;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct kv_host_shim *sh = cb_ctx;

	sh->ctrlr = ctrlr;
}

static void
io_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct kv_op_ctx *ctx = arg;
	struct kv_host_shim *sh = ctx->sh;

	/*
	 * Identity-token gate. Record a completion ONLY when it belongs to the
	 * currently armed op (ctx->op_id == armed_op_id). A stale orphan from a
	 * timed-out/transport-failed prior op carries that op's OLD token, which
	 * no longer matches armed_op_id (the timeout/failure path disarmed it, and
	 * a later op armed a new token), so it is DISCARDED here. This is what
	 * closes the vfio-user abort-on-reconnect window: the orphan's tracker is
	 * NOT aborted by disconnect/reconnect for this transport, so its callback
	 * may fire at an arbitrary later poll -- even in the middle of the next
	 * op's armed poll window -- yet it can never be mistaken for that op's
	 * result because its token differs.
	 *
	 * ctx points into the shim's op_ctx[] ring, which outlives every orphan
	 * (freed only at kv_host_shim_close()), so this dereference is always safe.
	 */
	if (ctx->op_id != sh->armed_op_id) {
		return;
	}
	sh->armed_op_id = 0;	/* consume: the matching completion arrived */
	sh->last_sct = cpl->status.sct;
	sh->last_sc = cpl->status.sc;
	sh->last_cdw0 = cpl->cdw0;
	sh->done = true;
}

/*
 * Best-effort backstop drain. Pump a bounded number of poll passes with NO op
 * armed (armed_op_id == 0), so io_complete() discards anything that fires.
 *
 * This is NOT the primary protection and CANNOT be: on vfio-user the orphaned
 * tracker is not aborted by reconnect, so its callback may fire at an UNBOUNDED
 * later poll -- past any fixed number of passes here, and even during the next
 * op's armed poll. The identity-token gate in io_complete() is what actually
 * makes such an orphan harmless. This drain just opportunistically consumes any
 * orphan that happens to be ready right now, keeping the qpair tidy; the bound
 * means it can never hang.
 */
static void
drain_stale_completions(struct kv_host_shim *sh)
{
	unsigned int pass;

	for (pass = 0; pass < 8; pass++) {
		int32_t n = spdk_nvme_qpair_process_completions(sh->qpair, 0);

		if (n <= 0) {
			break;
		}
	}
}

/*
 * Per-op completion budget. Bounds how long an in-flight KV command may run
 * before poll_to_completion() gives up and reports -ETIMEDOUT.
 *
 * Rationale: poll_to_completion() previously busy-looped FOREVER. If the NVMe
 * controller/target died mid-op (e.g. the target was hard-killed during a
 * Store/Retrieve/Exist/Delete), the call hung forever, stalling the in-process
 * NIXL plugin data path. This mirrors the bounded-wait pattern in
 * test/nvmf/kv/kv_host.c (wait_for_completion_timeout). A healthy in-memory or
 * vfio-user KV op is sub-millisecond; 20s is generous headroom for a slow
 * round-trip (e.g. a RADOS-backed kvdev) while still bounding a dead/wedged
 * target to seconds, not forever. A transport-layer qpair failure (-ENXIO from
 * spdk_nvme_qpair_process_completions, the usual signal of a dead target) is
 * detected immediately and does NOT wait for the full deadline.
 */
#define KV_HOST_SHIM_OP_TIMEOUT_S 20u

/*
 * Poll the qpair until the in-flight command completes, the qpair fails at the
 * transport layer, or the timeout expires.
 *
 * Returns 0 on completion (sh->done set; the caller reads status_to_rc()).
 * Returns a negated errno otherwise:
 *   -ENXIO     the qpair failed at the transport layer (dead/removed target)
 *   -ETIMEDOUT the command did not complete within KV_HOST_SHIM_OP_TIMEOUT_S
 *
 * On any error path the qpair is disconnected and the shim is flagged for
 * reconnect (see ensure_connected()).
 *
 * IMPORTANT (vfio-user semantics): this transport does NOT abort an outstanding
 * HARDWARE tracker on disconnect OR on reconnect. disconnect only flushes
 * software-QUEUED requests; and the CONNECTED->ENABLING tracker abort is gated
 * on trtype == PCIE in nvme_qpair.c, so it is SKIPPED for vfio-user. The
 * outstanding tracker is aborted only on qpair DELETE/free or a full ctrlr
 * reset. So a timed-out/failed op's callback (an orphan) can fire at an
 * UNBOUNDED later poll -- including during the NEXT op's poll below. We make
 * that orphan harmless with the PER-OP IDENTITY TOKEN: on every error path here
 * we disarm the op (armed_op_id = 0) so the orphan no longer matches, and the
 * next op arms a fresh token. io_complete() then records only the token-matching
 * completion, dropping the orphan. So a late completion from a timed-out op can
 * never be mistaken for, or corrupt the status of, the NEXT op.
 */
static int
poll_to_completion(struct kv_host_shim *sh)
{
	uint64_t deadline;

	deadline = spdk_get_ticks() +
		   (uint64_t)KV_HOST_SHIM_OP_TIMEOUT_S * spdk_get_ticks_hz();

	while (!sh->done) {
		int32_t n = spdk_nvme_qpair_process_completions(sh->qpair, 0);

		if (n < 0) {
			/* Transport-level failure (e.g. -ENXIO: qpair failed). The
			 * target is gone; disconnect and flag for reconnect. The
			 * outstanding tracker (if any) is NOT aborted by this
			 * transport, so its orphan callback may fire later; disarm
			 * the op (token cleared) so io_complete() drops it. */
			disarm_op(sh);
			spdk_nvme_ctrlr_disconnect_io_qpair(sh->qpair);
			sh->qpair_failed = true;
			return n;
		}
		if (!sh->done && spdk_get_ticks() >= deadline) {
			/* Bounded backstop: the op never completed (target hung
			 * but not yet transport-failed). Disconnect and flag for
			 * reconnect. The op's hardware tracker is still outstanding
			 * and is NOT aborted by this transport; its orphan callback
			 * may fire at an arbitrary later poll. Disarm the op (token
			 * cleared) so io_complete() discards that orphan. */
			disarm_op(sh);
			spdk_nvme_ctrlr_disconnect_io_qpair(sh->qpair);
			sh->qpair_failed = true;
			return -ETIMEDOUT;
		}
	}
	return 0;
}

/*
 * Reconnect the qpair if a prior op timed out / saw a transport failure and
 * disconnected it. Called before submitting each op. Returns 0 if the qpair is
 * usable, or a negated errno if it could not be reconnected (the target is
 * still down) so the op fails fast rather than submitting onto a dead qpair.
 *
 * NOTE on the prior op's orphan: on vfio-user the prior op's outstanding tracker
 * is NOT aborted by reconnect (the CONNECTED->ENABLING abort is PCIe-only), so
 * its orphan callback may still fire later -- possibly during the next op's
 * poll. The best-effort drain below opportunistically consumes any orphan that
 * is ready right now (with no op armed, armed_op_id == 0, so io_complete()
 * discards it), but it is NOT relied upon: the per-op identity token is what
 * guarantees a late orphan can never be recorded as the next op's result.
 */
static int
ensure_connected(struct kv_host_shim *sh)
{
	int rc;

	if (!sh->qpair_failed) {
		return 0;
	}
	rc = spdk_nvme_ctrlr_reconnect_io_qpair(sh->qpair);
	if (rc != 0) {
		return rc < 0 ? rc : -rc;
	}
	/* No op armed: best-effort consume any orphan that is ready right now. */
	disarm_op(sh);
	drain_stale_completions(sh);
	sh->qpair_failed = false;
	return 0;
}

/*
 * Translate the captured completion into the public return convention:
 *   0          -> SUCCESS
 *   positive   -> device-reported NVMe status code (sct == GENERIC)
 *   negative   -> transport/other error (negated errno)
 */
static int
status_to_rc(struct kv_host_shim *sh)
{
	if (sh->last_sct != SPDK_NVME_SCT_GENERIC) {
		/* Non-generic status types are reported as a transport-ish error. */
		return -EIO;
	}
	return (int)sh->last_sc;
}

int
kv_host_shim_open(const struct kv_host_shim_opts *opts, struct kv_host_shim **out)
{
	struct kv_host_shim *sh;
	struct spdk_nvme_transport_id trid = {};
	const struct spdk_nvme_kv_ns_data *kv_ns_data;
	uint32_t nsid;
	int rc;

	if (opts == NULL || out == NULL) {
		return -EINVAL;
	}
	/*
	 * Define the out-param up front so every failure path below (env-init
	 * failure, probe/attach failure, no KV ns, qpair alloc failure) leaves
	 * the caller's handle defined rather than stale.
	 */
	*out = NULL;
	/* Size-version validation: require the field we read here. */
	if (opts->opts_size < sizeof(struct kv_host_shim_opts)) {
		return -EINVAL;
	}
	if (opts->vfu_addr == NULL) {
		return -EINVAL;
	}

	sh = calloc(1, sizeof(*sh));
	if (sh == NULL) {
		return -ENOMEM;
	}

	if (opts->init_env) {
		struct spdk_env_opts env_opts;

		env_opts.opts_size = sizeof(env_opts);
		spdk_env_opts_init(&env_opts);
		env_opts.name = opts->name ? opts->name : "kv_host_shim";
		/*
		 * As an in-process host harness we frequently run unprivileged and
		 * without reserved hugepages. Forcing no_huge selects IOVA=VA so DMA
		 * works without root/PA access; mem_size bounds the no-huge heap. If
		 * the caller has a root/hugepage environment this still works.
		 */
		env_opts.no_huge = true;
		env_opts.mem_size = 512;
		if (spdk_env_init(&env_opts) < 0) {
			free(sh);
			return -EFAULT;
		}
		sh->owns_env = true;
	}

	trid.trtype = SPDK_NVME_TRANSPORT_VFIOUSER;
	snprintf(trid.traddr, sizeof(trid.traddr), "%s", opts->vfu_addr);

	if (spdk_nvme_probe(&trid, sh, probe_cb, attach_cb, NULL) != 0 ||
	    sh->ctrlr == NULL) {
		rc = -ENODEV;
		goto err_env;
	}

	/* Bind the requested namespace, or the first CSI==KV ns when nsid==0. */
	if (opts->nsid != 0) {
		struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(sh->ctrlr, opts->nsid);

		if (ns != NULL && spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_KV) {
			sh->ns = ns;
		}
	} else {
		for (nsid = spdk_nvme_ctrlr_get_first_active_ns(sh->ctrlr); nsid != 0;
		     nsid = spdk_nvme_ctrlr_get_next_active_ns(sh->ctrlr, nsid)) {
			struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(sh->ctrlr, nsid);

			if (ns != NULL && spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_KV) {
				sh->ns = ns;
				break;
			}
		}
	}

	if (sh->ns == NULL) {
		rc = -ENOENT;
		goto err_detach;
	}

	kv_ns_data = spdk_nvme_kv_ns_get_data(sh->ns);
	if (kv_ns_data == NULL) {
		rc = -EPROTO;
		goto err_detach;
	}
	sh->kvkml = kv_ns_data->kvf[0].kvkml;
	sh->kvvml = kv_ns_data->kvf[0].kvvml;

	sh->qpair = spdk_nvme_ctrlr_alloc_io_qpair(sh->ctrlr, NULL, 0);
	if (sh->qpair == NULL) {
		rc = -ENOMEM;
		goto err_detach;
	}

	*out = sh;
	return 0;

err_detach:
	spdk_nvme_detach(sh->ctrlr);
err_env:
	if (sh->owns_env) {
		spdk_env_fini();
	}
	free(sh);
	return rc;
}

void
kv_host_shim_close(struct kv_host_shim *sh)
{
	if (sh == NULL) {
		return;
	}
	if (sh->qpair != NULL) {
		spdk_nvme_ctrlr_free_io_qpair(sh->qpair);
	}
	if (sh->ctrlr != NULL) {
		spdk_nvme_detach(sh->ctrlr);
	}
	if (sh->owns_env) {
		spdk_env_fini();
	}
	free(sh);
}

void *
kv_host_shim_dma_alloc(size_t len)
{
	return spdk_dma_zmalloc(len, 0, NULL);
}

void
kv_host_shim_dma_free(void *buf)
{
	spdk_dma_free(buf);
}

uint32_t
kv_host_shim_max_value_len(const struct kv_host_shim *sh)
{
	return sh ? sh->kvvml : 0;
}

uint32_t
kv_host_shim_max_key_len(const struct kv_host_shim *sh)
{
	return sh ? sh->kvkml : 0;
}

int
kv_host_shim_store(struct kv_host_shim *sh, const void *key, uint8_t key_len,
		   const void *value, uint32_t value_len)
{
	struct kv_op_ctx *ctx;
	int rc;

	if (sh == NULL) {
		return -EINVAL;
	}
	rc = ensure_connected(sh);
	if (rc != 0) {
		return rc;
	}
	/* Arm a fresh identity token; io_complete() records only the completion
	 * carrying this token (ctx), dropping any stale orphan from a prior op. */
	ctx = arm_op(sh);
	rc = spdk_nvme_kv_store(sh->ns, sh->qpair, key, key_len, value, value_len,
				io_complete, ctx, 0);
	if (rc != 0) {
		disarm_op(sh);	/* no tracker created; drop the armed token */
		return rc < 0 ? rc : -rc;
	}
	rc = poll_to_completion(sh);
	if (rc != 0) {
		return rc;
	}
	return status_to_rc(sh);
}

int
kv_host_shim_retrieve(struct kv_host_shim *sh, const void *key, uint8_t key_len,
		      void *value, uint32_t buf_len, uint32_t *value_len_out)
{
	struct kv_op_ctx *ctx;
	int rc;

	if (sh == NULL) {
		return -EINVAL;
	}
	rc = ensure_connected(sh);
	if (rc != 0) {
		return rc;
	}
	/* Arm a fresh identity token; io_complete() records only the completion
	 * carrying this token (ctx), dropping any stale orphan from a prior op. */
	ctx = arm_op(sh);
	rc = spdk_nvme_kv_retrieve(sh->ns, sh->qpair, key, key_len, value, buf_len,
				   io_complete, ctx, 0);
	if (rc != 0) {
		disarm_op(sh);	/* no tracker created; drop the armed token */
		return rc < 0 ? rc : -rc;
	}
	rc = poll_to_completion(sh);
	if (rc != 0) {
		return rc;
	}
	rc = status_to_rc(sh);
	/* On SUCCESS, cdw0 carries the device's TRUE value length. */
	if (rc == 0 && value_len_out != NULL) {
		*value_len_out = sh->last_cdw0;
	}
	return rc;
}

int
kv_host_shim_exist(struct kv_host_shim *sh, const void *key, uint8_t key_len)
{
	struct kv_op_ctx *ctx;
	int rc;

	if (sh == NULL) {
		return -EINVAL;
	}
	rc = ensure_connected(sh);
	if (rc != 0) {
		return rc;
	}
	/* Arm a fresh identity token; io_complete() records only the completion
	 * carrying this token (ctx), dropping any stale orphan from a prior op. */
	ctx = arm_op(sh);
	rc = spdk_nvme_kv_exist(sh->ns, sh->qpair, key, key_len, io_complete, ctx);
	if (rc != 0) {
		disarm_op(sh);	/* no tracker created; drop the armed token */
		return rc < 0 ? rc : -rc;
	}
	rc = poll_to_completion(sh);
	if (rc != 0) {
		return rc;
	}
	return status_to_rc(sh);
}

int
kv_host_shim_delete(struct kv_host_shim *sh, const void *key, uint8_t key_len)
{
	struct kv_op_ctx *ctx;
	int rc;

	if (sh == NULL) {
		return -EINVAL;
	}
	rc = ensure_connected(sh);
	if (rc != 0) {
		return rc;
	}
	/* Arm a fresh identity token; io_complete() records only the completion
	 * carrying this token (ctx), dropping any stale orphan from a prior op. */
	ctx = arm_op(sh);
	rc = spdk_nvme_kv_delete(sh->ns, sh->qpair, key, key_len, io_complete, ctx);
	if (rc != 0) {
		disarm_op(sh);	/* no tracker created; drop the armed token */
		return rc < 0 ? rc : -rc;
	}
	rc = poll_to_completion(sh);
	if (rc != 0) {
		return rc;
	}
	return status_to_rc(sh);
}
