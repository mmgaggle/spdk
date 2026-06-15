/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation.
 *   All rights reserved.
 */

/*
 * Two-tier front bridge (Slice C4): adapts the SPDK kvdev_rados datapath to the
 * Mercury front client core. Compiled only under SPDK_CONFIG_MERCURY (the module
 * Makefile adds this source under CONFIG_MERCURY). See kvdev_rados_nkvx_front.h
 * and docs/design/slice-c-exec-rpc-mercury.md §2/§4.
 */

#include "kvdev_rados_nkvx_front.h"
#include "nkvx_front_client.h"		/* struct nkvx_front, nkvx_exec_in_t, NKVX_INLINE_MAX */

#include "spdk/log.h"
#include "spdk/util.h"			/* spdk_min */

/*
 * Per-forward completion context. Outlives kvdev_rados_nkvx_front_forward() and
 * is freed in the done trampoline (which runs from a progress tick on the
 * reactor thread, design §4.2).
 */
struct kvdev_rados_nkvx_fwd_ctx {
	spdk_kvdev_io_completion_cb	cb_fn;
	void				*cb_arg;
	void				*host_out;
	uint32_t			host_out_len;
};

/*
 * Mercury-side completion: copy the inline result into the tenant's output
 * buffer (truncated to its cap; the executor already truncated to osize, design
 * §1.2) and fire the kvdev completion with the TRUE result length.
 */
static void
kvdev_rados_nkvx_front_done(void *arg, enum spdk_kvdev_io_status status,
			    uint32_t result_len, const void *result_inline,
			    uint32_t result_inline_len)
{
	struct kvdev_rados_nkvx_fwd_ctx *ctx = arg;

	if (result_inline != NULL && result_inline_len > 0 &&
	    ctx->host_out != NULL && ctx->host_out_len > 0) {
		uint32_t n = spdk_min(result_inline_len, ctx->host_out_len);
		memcpy(ctx->host_out, result_inline, n);
	}

	/*
	 * status is passed straight through (correct: ABORTED maps to the tenant
	 * ABORTED_BY_REQUEST). NB a cancel-induced ABORTED (channel-destroy teardown)
	 * shares the tenant CQE status with a resource-cap ABORTED — telemetry only,
	 * no functional difference at the tenant.
	 */
	ctx->cb_fn(ctx->cb_arg, status, result_len);
	free(ctx);
}

int
kvdev_rados_nkvx_front_create(const char *target_addr, struct nkvx_front **out)
{
	const char *sep;
	char na_init[64];
	size_t n;

	if (target_addr == NULL || out == NULL) {
		return -EINVAL;
	}

	/* Derive the origin NA init/provider string from the address prefix:
	 * everything up to and including "://" (e.g. "ofi+tcp://h:p" -> "ofi+tcp://",
	 * "na+sm://7-0" -> "na+sm://"). */
	sep = strstr(target_addr, "://");
	if (sep == NULL) {
		SPDK_ERRLOG("nkvx front: malformed executor address '%s' (no '://')\n",
			    target_addr);
		return -EINVAL;
	}
	n = (size_t)(sep - target_addr) + 3;
	if (n + 1 > sizeof(na_init)) {
		SPDK_ERRLOG("nkvx front: executor provider prefix too long in '%s'\n",
			    target_addr);
		return -EINVAL;
	}
	memcpy(na_init, target_addr, n);
	na_init[n] = '\0';

	return nkvx_front_init(na_init, target_addr, out);
}

void
kvdev_rados_nkvx_front_destroy(struct nkvx_front *front)
{
	nkvx_front_fini(front);
}

int
kvdev_rados_nkvx_front_progress(struct nkvx_front *front)
{
	/* Non-blocking: timeout 0 so the reactor poller never stalls (design §4.2). */
	return nkvx_front_progress(front, 0);
}

int
kvdev_rados_nkvx_front_drain_progress(struct nkvx_front *front, unsigned int timeout_ms)
{
	/* Channel-destroy ONLY: the channel is being torn down (no more datapath on
	 * this reactor), so a small BLOCKING progress is fine and is what avoids the
	 * busy-spin of timeout-0 polling while we wait for cancelled forwards to reach
	 * a terminal completion. Never called on the hot datapath. */
	return nkvx_front_progress(front, timeout_ms);
}

int
kvdev_rados_nkvx_front_forward(struct nkvx_front *front,
			       const void *key, uint8_t key_len,
			       uint32_t op_id, bool read_only,
			       uint8_t runtime,
			       const char *module_key, const char *module_ns,
			       const uint8_t *sha256, bool sha256_valid,
			       uint64_t caps,
			       const void *input, uint32_t input_len,
			       void *output_buf, uint32_t output_buf_len,
			       spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_nkvx_fwd_ctx *ctx;
	nkvx_exec_in_t in;
	int rc;

	/* key_len is a uint8_t and SPDK_KVDEV_EXEC_KEY_MAX_LEN == 255, so it cannot
	 * exceed the cap; only the empty-key case is invalid here. */
	if (front == NULL || cb_fn == NULL || key_len == 0) {
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->host_out = output_buf;
	ctx->host_out_len = output_buf_len;

	memset(&in, 0, sizeof(in));
	in.op_id = op_id;
	in.read_only = read_only ? 1 : 0;
	in.runtime = runtime;
	in.caps = caps;
	in.key_len = key_len;
	memcpy(in.key, key, key_len);
	if (sha256 != NULL && sha256_valid) {
		memcpy(in.sha256, sha256, SPDK_KV_EXEC_SHA256_LEN);
	}
	in.sha256_valid = sha256_valid ? 1 : 0;
	in.module_key = (char *)module_key;	/* borrowed; encoded synchronously */
	in.module_ns = (char *)module_ns;
	in.osize = output_buf_len;
	in.input_len = input_len;
	in.input_inline = (void *)input;	/* borrowed; registered/encoded synchronously */
	in.input_bulk = HG_BULK_NULL;		/* originated by nkvx_front_forward */
	in.result_sink = HG_BULK_NULL;		/* originated by nkvx_front_forward */

	/*
	 * Pass the tenant's host output buffer as the result sink (Slice C7): when it
	 * can hold a large result (> NKVX_INLINE_MAX) the front client registers it
	 * WRITE-mode and the executor PUSHes straight into it. A small result still
	 * comes back inline and the done-cb copies it into host_out. Either way the
	 * bytes land in this same buffer — no double delivery (the executor inlines
	 * XOR pushes), and the done-cb's memcpy is a no-op on the push path (inline
	 * is empty). Large input is likewise registered from in.input_inline.
	 */
	rc = nkvx_front_forward(front, &in, output_buf, output_buf_len,
				kvdev_rados_nkvx_front_done, ctx);
	if (rc != 0) {
		free(ctx);
		return rc;
	}
	return 0;
}

void
kvdev_rados_nkvx_front_cancel_all(struct nkvx_front *front)
{
	nkvx_front_cancel_all(front);
}

unsigned
kvdev_rados_nkvx_front_outstanding(struct nkvx_front *front)
{
	return nkvx_front_outstanding(front);
}

void
kvdev_rados_nkvx_front_fail_all_pending(struct nkvx_front *front,
					enum spdk_kvdev_io_status status)
{
	nkvx_front_fail_all_pending(front, status);
}
