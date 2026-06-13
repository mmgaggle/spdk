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
	/* Per-op completion state. */
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

	sh->last_sct = cpl->status.sct;
	sh->last_sc = cpl->status.sc;
	sh->last_cdw0 = cpl->cdw0;
	sh->done = true;
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
 * On any error path the outstanding request is aborted by disconnecting the
 * qpair (which completes/cancels every in-flight command on it). This prevents
 * a late completion from later firing into io_complete() and corrupting the
 * status of the NEXT op, since the shim reuses a single in-flight slot. The
 * shim is flagged for reconnect so the next op transparently reconnects the
 * qpair (see ensure_connected()).
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
			 * target is gone; abort the op and report the error. */
			spdk_nvme_ctrlr_disconnect_io_qpair(sh->qpair);
			sh->qpair_failed = true;
			return n;
		}
		if (!sh->done && spdk_get_ticks() >= deadline) {
			/* Bounded backstop: the op never completed (target hung
			 * but not yet transport-failed). Abort the outstanding
			 * request by disconnecting the qpair so a late completion
			 * cannot corrupt the next op. */
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
	sh->done = false;
	rc = spdk_nvme_kv_store(sh->ns, sh->qpair, key, key_len, value, value_len,
				io_complete, sh, 0);
	if (rc != 0) {
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
	sh->done = false;
	rc = spdk_nvme_kv_retrieve(sh->ns, sh->qpair, key, key_len, value, buf_len,
				   io_complete, sh, 0);
	if (rc != 0) {
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
	sh->done = false;
	rc = spdk_nvme_kv_exist(sh->ns, sh->qpair, key, key_len, io_complete, sh);
	if (rc != 0) {
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
	sh->done = false;
	rc = spdk_nvme_kv_delete(sh->ns, sh->qpair, key, key_len, io_complete, sh);
	if (rc != 0) {
		return rc < 0 ? rc : -rc;
	}
	rc = poll_to_completion(sh);
	if (rc != 0) {
		return rc;
	}
	return status_to_rc(sh);
}
