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

struct nkvx_front {
	hg_class_t	*cls;
	hg_context_t	*ctx;
	hg_id_t		rpc_id;
	hg_addr_t	addr;	/* resolved executor target */
};

/* Per-forward context, carried through HG_Forward's callback and freed there. */
struct nkvx_call {
	nkvx_front_done_cb	cb;
	void			*arg;
};

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
	if (front == NULL) {
		return;
	}
	HG_Addr_free(front->cls, front->addr);
	HG_Context_destroy(front->ctx);
	HG_Finalize(front->cls);
	free(front);
}

int
nkvx_front_forward(struct nkvx_front *front, const nkvx_exec_in_t *in,
		   nkvx_front_done_cb cb, void *arg)
{
	struct nkvx_call *call;
	hg_handle_t handle = HG_HANDLE_NULL;
	hg_return_t ret;

	if (front == NULL || in == NULL || cb == NULL) {
		return -EINVAL;
	}

	call = calloc(1, sizeof(*call));
	if (call == NULL) {
		return -ENOMEM;
	}
	call->cb = cb;
	call->arg = arg;

	ret = HG_Create(front->ctx, front->addr, front->rpc_id, &handle);
	if (ret != HG_SUCCESS) {
		free(call);
		return -EIO;
	}

	/*
	 * HG_Forward encodes the request into the SEND synchronously (the proc
	 * runs now), so `in` may be freed/reused by the caller on return. The
	 * const cast is safe: the encode path only reads. The completion fires
	 * later from nkvx_front_progress() via nkvx_front_forward_cb().
	 */
	ret = HG_Forward(handle, nkvx_front_forward_cb, call, (void *)in);
	if (ret != HG_SUCCESS) {
		HG_Destroy(handle);
		free(call);
		return -EIO;
	}

	return 0;
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
