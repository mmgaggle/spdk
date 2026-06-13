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
	 * Per-op completion state.
	 *
	 * `expecting` gates io_complete(): it records a completion into the slot
	 * ONLY while an op is armed (expecting == true), and clears it as soon as
	 * the matching completion arrives. This is what makes a STALE completion
	 * race-safe on the PCIe/vfio-user transport: when an op times out we
	 * disconnect the qpair, but its hardware tracker is still outstanding and
	 * is only aborted LATER, on reconnect, inside the next
	 * spdk_nvme_qpair_process_completions(). That aborted callback fires with
	 * expecting == false (we drain it on reconnect, and the next op does not
	 * arm until after the drain), so it is discarded instead of being counted
	 * as the next op's result.
	 */
	volatile bool		expecting;
	volatile bool		done;
	volatile uint8_t	last_sct;
	volatile uint8_t	last_sc;
	volatile uint32_t	last_cdw0;
};

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
	struct kv_host_shim *sh = arg;

	/*
	 * Only record a completion that belongs to the currently armed op. A
	 * stale callback from a timed-out/aborted prior op (its hardware tracker
	 * is flushed on reconnect, see ensure_connected()) fires with
	 * expecting == false and is discarded here, so it cannot corrupt the
	 * status slot of the next op.
	 */
	if (!sh->expecting) {
		return;
	}
	sh->expecting = false;
	sh->last_sct = cpl->status.sct;
	sh->last_sc = cpl->status.sc;
	sh->last_cdw0 = cpl->cdw0;
	sh->done = true;
}

/*
 * Drain any completions sitting on the qpair WITHOUT an op armed (expecting ==
 * false), so io_complete() discards them. Used after a reconnect to consume the
 * stale aborted trackers from a prior timed-out/failed op before the next op
 * arms. Bounded: a fixed number of poll passes (the abort drain settles in the
 * first pass; the extra passes are cheap insurance) so this can never hang.
 *
 * A transport failure here (-ENXIO) means the reconnected qpair is already dead
 * again; we stop draining and let ensure_connected() re-flag the qpair so the
 * caller fails fast rather than submitting onto a dead qpair.
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
 * IMPORTANT (PCIe/vfio-user semantics): disconnecting the qpair does NOT abort
 * the outstanding HARDWARE tracker for a healthy in-flight op -- it only flushes
 * software-QUEUED requests (nvme_qpair_abort_all_queued_reqs). The outstanding
 * tracker is aborted only LATER, on RECONNECT, when the next
 * spdk_nvme_qpair_process_completions() drives the qpair CONNECTED->ENABLING and
 * fires the tracker callback with SC_ABORTED_SQ_DELETION. That stale callback is
 * made harmless two ways: (1) it is drained on reconnect while no op is armed
 * (drain_stale_completions()), and (2) io_complete() ignores any completion that
 * arrives with expecting == false. So a late completion from a timed-out op can
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
			 * outstanding tracker (if any) is flushed on reconnect and
			 * ignored by io_complete() (expecting cleared below). */
			sh->expecting = false;
			spdk_nvme_ctrlr_disconnect_io_qpair(sh->qpair);
			sh->qpair_failed = true;
			return n;
		}
		if (!sh->done && spdk_get_ticks() >= deadline) {
			/* Bounded backstop: the op never completed (target hung
			 * but not yet transport-failed). Disconnect and flag for
			 * reconnect. The op's hardware tracker is still outstanding;
			 * it is flushed on reconnect and discarded by io_complete()
			 * because we clear `expecting` here. */
			sh->expecting = false;
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
 * After a SUCCESSFUL reconnect, the prior op's outstanding hardware tracker is
 * aborted by the transport (it fires SC_ABORTED_SQ_DELETION on the next
 * process_completions). We pump those stale callbacks here, with no op armed
 * (expecting == false), so io_complete() discards them. This MUST happen before
 * the caller arms and submits the next op, otherwise the first
 * process_completions in poll_to_completion() would fire the stale callback into
 * the new op's slot and return the wrong status.
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
	/* Not expecting a completion: drain the aborted prior tracker(s). */
	sh->expecting = false;
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
	int rc;

	if (sh == NULL) {
		return -EINVAL;
	}
	rc = ensure_connected(sh);
	if (rc != 0) {
		return rc;
	}
	/* Arm the completion slot: done clears, expecting opens so the matching
	 * callback (and only it) is recorded by io_complete(). */
	sh->done = false;
	sh->expecting = true;
	rc = spdk_nvme_kv_store(sh->ns, sh->qpair, key, key_len, value, value_len,
				io_complete, sh, 0);
	if (rc != 0) {
		sh->expecting = false;	/* no tracker created; disarm the slot */
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
	int rc;

	if (sh == NULL) {
		return -EINVAL;
	}
	rc = ensure_connected(sh);
	if (rc != 0) {
		return rc;
	}
	/* Arm the completion slot: done clears, expecting opens so the matching
	 * callback (and only it) is recorded by io_complete(). */
	sh->done = false;
	sh->expecting = true;
	rc = spdk_nvme_kv_retrieve(sh->ns, sh->qpair, key, key_len, value, buf_len,
				   io_complete, sh, 0);
	if (rc != 0) {
		sh->expecting = false;	/* no tracker created; disarm the slot */
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
	int rc;

	if (sh == NULL) {
		return -EINVAL;
	}
	rc = ensure_connected(sh);
	if (rc != 0) {
		return rc;
	}
	/* Arm the completion slot: done clears, expecting opens so the matching
	 * callback (and only it) is recorded by io_complete(). */
	sh->done = false;
	sh->expecting = true;
	rc = spdk_nvme_kv_exist(sh->ns, sh->qpair, key, key_len, io_complete, sh);
	if (rc != 0) {
		sh->expecting = false;	/* no tracker created; disarm the slot */
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
	int rc;

	if (sh == NULL) {
		return -EINVAL;
	}
	rc = ensure_connected(sh);
	if (rc != 0) {
		return rc;
	}
	/* Arm the completion slot: done clears, expecting opens so the matching
	 * callback (and only it) is recorded by io_complete(). */
	sh->done = false;
	sh->expecting = true;
	rc = spdk_nvme_kv_delete(sh->ns, sh->qpair, key, key_len, io_complete, sh);
	if (rc != 0) {
		sh->expecting = false;	/* no tracker created; disarm the slot */
		return rc < 0 ? rc : -rc;
	}
	rc = poll_to_completion(sh);
	if (rc != 0) {
		return rc;
	}
	return status_to_rc(sh);
}
