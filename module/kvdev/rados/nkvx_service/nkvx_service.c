/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation.
 *   All rights reserved.
 */

/*
 * rados-nkvx standalone executor service — Slice C2 skeleton.
 *
 * A non-SPDK Mercury TARGET that listens for the inter-tier `nkvx_exec` RPC
 * (the contract frozen in Slice C3, ../nkvx_exec_rpc.h) coming from the front
 * (rados-nkv). See docs/design/slice-c-exec-rpc-mercury.md §2/§4.
 *
 * SCOPE (C2): the service skeleton ONLY — Mercury init (listen), register the
 * RPC, a manual HG_Progress/HG_Trigger progress loop, and self-address publish
 * (design OQ-8: a file on the testbed). The RPC handler is a STUB: it decodes a
 * well-formed request and replies SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED. The real
 * fetch -> sha256-verify -> wasmtime-run -> TB4-cache pipeline (reusing the
 * existing off-reactor kvdev_rados_nkvx*.c) lands in Slice C5; this proves the
 * transport + contract round-trip end to end first.
 *
 * Build under -std=gnu11 (load-bearing for the C3 wire-status static-asserts;
 * see ../nkvx_exec_rpc.c). Built by the standalone Makefile in this directory,
 * NOT the SPDK build graph (the executor is non-SPDK by design, design §1).
 *
 * Dev rung: na+sm:// (or ofi+tcp://) loopback. Acceptance rung: ofi+verbs over
 * the E810 DAC (Slice C8) — same code, swap the --listen provider string.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#include <mercury.h>
#include <mercury_bulk.h>

#include "nkvx_exec_rpc.h"
#include "nkvx_executor.h"

/* Set from a signal handler to break the progress loop for an orderly exit. */
static volatile sig_atomic_t g_stop;

/*
 * The librados-backed executor backend (Slice C5a). NULL until a --rados-pool is
 * configured; when NULL the handler keeps the C2 skeleton behaviour (decode +
 * NOT_SUPPORTED) so the transport/contract still round-trips with no cluster.
 * Single executor per process, so a file-scope handle is sufficient.
 */
static struct nkvx_executor *g_executor;

static void
on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

/*
 * The `nkvx_exec` RPC handler (Slice C5a compute + Slice C7 bulk RMA).
 *
 * The flow is an async state machine because the large-payload paths interpose
 * Mercury bulk transfers that complete on later progress ticks (design §1.3):
 *
 *   Get_input
 *     -> [large input] HG_Bulk_transfer(PULL) ──cb──> on_input_pulled
 *     -> run executor (cold-fill + module)
 *     -> [large result + result_sink] HG_Bulk_transfer(PUSH) ──cb──> on_result_pushed
 *     -> HG_Respond
 *
 * NORMATIVE (design §1.3): for a large result the executor MUST await the PUSH
 * completion BEFORE HG_Respond — the response is what unblocks the front's CQE,
 * so responding early lets the tenant read a partially-written DPTR (silent
 * corruption). Here HG_Respond is chained strictly from on_result_pushed.
 *
 * Bulk RMA is a TRANSPORT concern and lives here (the handler owns the Mercury
 * handle/context/origin addr), not in the executor backend, which is delivery-
 * agnostic and just hands back a result buffer (struct nkvx_exec_result).
 *
 * Mercury ownership: HG_Get_input allocates the decoded request (strings, inline
 * input, the optional input_bulk/result_sink handles); HG_Free_input releases
 * ALL of it via the proc HG_FREE path — so the handler frees only the LOCAL bulk
 * handles it created, never in.input_bulk / in.result_sink. The handle is
 * reference-counted; HG_Destroy drops our reference once the response is queued.
 */
struct nkvx_req {
	hg_handle_t		handle;
	hg_context_t		*ctx;		/* this target's context (for bulk) */
	hg_addr_t		origin;		/* the front's address (bulk peer) */
	nkvx_exec_in_t		in;
	struct nkvx_exec_result	res;		/* delivered bytes from the backend */
	nkvx_exec_out_t		out;		/* response envelope (borrows res.buf) */
	void			*input_buf;	/* pulled large input, if any */
	hg_bulk_t		local_input;	/* local WRITE handle for the PULL */
	hg_bulk_t		local_result;	/* local READ handle for the PUSH */
};

/* Respond with req->out (already populated), then tear the request down. */
static void
nkvx_req_finish(struct nkvx_req *req)
{
	hg_return_t ret = HG_Respond(req->handle, NULL, NULL, &req->out);

	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: HG_Respond failed: %s\n",
			HG_Error_to_string(ret));
	}

	/* HG_Respond encoded the response synchronously, so everything it borrowed
	 * (req->out.result_inline points into req->res.buf) can be released now. */
	if (req->local_result != HG_BULK_NULL) {
		HG_Bulk_free(req->local_result);
	}
	if (req->local_input != HG_BULK_NULL) {
		HG_Bulk_free(req->local_input);
	}
	nkvx_exec_result_free(&req->res);
	if (req->input_buf != NULL) {
		/* The large-input path repointed in.input_inline at our pulled buffer;
		 * detach it so HG_Free_input frees only Mercury-owned memory, then we
		 * free input_buf ourselves below. On the inline path input_buf is NULL
		 * and in.input_inline is the proc-decoded buffer HG_Free_input must free. */
		req->in.input_inline = NULL;
	}
	free(req->input_buf);	/* free(NULL) is fine on the inline path */
	HG_Free_input(req->handle, &req->in);
	HG_Destroy(req->handle);
	free(req);
}

/* Populate the response envelope with a terminal status (no result body) and
 * respond + tear down. Collapses the handler's many identical failure tails. */
static void
nkvx_req_fail(struct nkvx_req *req, enum spdk_kvdev_io_status status)
{
	req->out.status = nkvx_status_to_wire(status);
	req->out.result_len = 0;
	req->out.result_inline = NULL;
	req->out.result_inline_len = 0;
	nkvx_req_finish(req);
}

/*
 * PUSH-completion callback (NORMATIVE chain point): the large result has fully
 * landed in the front's result_sink. Only NOW is it safe to respond.
 */
static hg_return_t
nkvx_result_pushed_cb(const struct hg_cb_info *info)
{
	struct nkvx_req *req = info->arg;

	if (info->ret != HG_SUCCESS) {
		/* The bytes did not (fully) land: do NOT report success — the front
		 * would fire a CQE over a torn DPTR. Synthesize FAILED. */
		fprintf(stderr, "nkvx_service: result PUSH failed: %s\n",
			HG_Error_to_string(info->ret));
		nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
		return HG_SUCCESS;
	}
	req->out.status = nkvx_status_to_wire(req->res.status);
	req->out.result_len = req->res.result_len;
	req->out.result_inline = NULL;		/* delivered via the sink, not inline */
	req->out.result_inline_len = 0;
	nkvx_req_finish(req);
	return HG_SUCCESS;
}

/*
 * Run the module, then deliver the result: inline in the response when it is
 * small (or no sink was offered), else PUSH it into the front's WRITE-registered
 * result_sink and respond only from the PUSH-completion callback.
 */
static void
nkvx_req_run_and_deliver(struct nkvx_req *req)
{
	hg_class_t *cls = HG_Get_info(req->handle)->hg_class;
	hg_return_t ret;

	nkvx_executor_run(g_executor, &req->in, &req->res);
	fprintf(stderr,
		"nkvx_service: nkvx_exec op_id=%u key_len=%u runtime=%u module_key=%s "
		"module_ns=%s -> status=%d result_len=%u deliver=%u%s\n",
		req->in.op_id, req->in.key_len, req->in.runtime,
		req->in.module_key ? req->in.module_key : "(null)",
		req->in.module_ns ? req->in.module_ns : "(null)",
		req->res.status, req->res.result_len, req->res.buf_len,
		(req->in.result_sink != HG_BULK_NULL) ? " sink" : "");

	/* Large result + a sink to push into: front-sink/executor-push (design §1.3). */
	if (req->in.result_sink != HG_BULK_NULL && req->res.buf_len > NKVX_INLINE_MAX) {
		void *p = req->res.buf;
		hg_size_t sz = req->res.buf_len;

		ret = HG_Bulk_create(cls, 1, &p, &sz, HG_BULK_READ_ONLY, &req->local_result);
		if (ret != HG_SUCCESS) {
			fprintf(stderr, "nkvx_service: HG_Bulk_create(result) failed: %s\n",
				HG_Error_to_string(ret));
			nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
			return;
		}
		ret = HG_Bulk_transfer(req->ctx, nkvx_result_pushed_cb, req,
				       HG_BULK_PUSH, req->origin, req->in.result_sink, 0,
				       req->local_result, 0, sz, HG_OP_ID_IGNORE);
		if (ret != HG_SUCCESS) {
			fprintf(stderr, "nkvx_service: HG_Bulk_transfer(PUSH) failed: %s\n",
				HG_Error_to_string(ret));
			nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
			return;
		}
		return;		/* respond from nkvx_result_pushed_cb (NORMATIVE) */
	}

	/* Inline path. A large result with NO sink offered cannot be delivered — the
	 * front did not register a DPTR to push into; decline cleanly (design §1.3).
	 *
	 * This branch is currently UNREACHABLE: the front registers a result_sink
	 * whenever osize > NKVX_INLINE_MAX, and the executor caps buf_len =
	 * min(result_len, osize) <= osize, so buf_len > NKVX_INLINE_MAX implies a
	 * sink was registered (and the PUSH path above was taken). It is retained as
	 * a defensive guard for when the C7.2 MR/handle cache makes sink registration
	 * conditional — the two thresholds (the front's osize-based sink offer and the
	 * executor's buf_len-based push) must stay in agreement. Do not delete. */
	if (req->res.buf_len > NKVX_INLINE_MAX) {
		fprintf(stderr, "nkvx_service: result %u > inline max %u and no result_sink "
			"— NOT_SUPPORTED\n", req->res.buf_len, NKVX_INLINE_MAX);
		req->out.status = nkvx_status_to_wire(SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED);
		req->out.result_len = 0;
		req->out.result_inline = NULL;
		req->out.result_inline_len = 0;
	} else {
		req->out.status = nkvx_status_to_wire(req->res.status);
		req->out.result_len = req->res.result_len;
		req->out.result_inline = req->res.buf;		/* borrowed; freed via res */
		req->out.result_inline_len = req->res.buf_len;
	}
	nkvx_req_finish(req);
}

/*
 * Large-input PULL completion: the request input has been read into input_buf.
 * Point the backend at it and proceed to run.
 */
static hg_return_t
nkvx_input_pulled_cb(const struct hg_cb_info *info)
{
	struct nkvx_req *req = info->arg;

	if (info->ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: input PULL failed: %s\n",
			HG_Error_to_string(info->ret));
		nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
		return HG_SUCCESS;
	}
	req->in.input_inline = req->input_buf;	/* backend reads input_inline */
	nkvx_req_run_and_deliver(req);
	return HG_SUCCESS;
}

static hg_return_t
nkvx_exec_handler(hg_handle_t handle)
{
	struct nkvx_req *req;
	const struct hg_info *info;
	hg_return_t ret;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		fprintf(stderr, "nkvx_service: out of memory for request\n");
		HG_Destroy(handle);
		return HG_NOMEM;
	}
	req->handle = handle;
	req->local_input = HG_BULK_NULL;
	req->local_result = HG_BULK_NULL;

	ret = HG_Get_input(handle, &req->in);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: HG_Get_input failed: %s\n",
			HG_Error_to_string(ret));
		/* Cannot form a meaningful reply without the request; drop it. */
		free(req);
		HG_Destroy(handle);
		return ret;
	}

	info = HG_Get_info(handle);
	req->ctx = info->context;
	req->origin = info->addr;

	if (g_executor == NULL) {
		/*
		 * No cluster configured (C2 skeleton mode): decode + decline so the
		 * transport/contract still round-trips with no RADOS. NOT_SUPPORTED
		 * maps at the tenant to INVALID_OPCODE (design §3).
		 */
		fprintf(stderr,
			"nkvx_service: nkvx_exec op_id=%u key_len=%u runtime=%u "
			"module_key=%s module_ns=%s -> NOT_SUPPORTED (no cluster)\n",
			req->in.op_id, req->in.key_len, req->in.runtime,
			req->in.module_key ? req->in.module_key : "(null)",
			req->in.module_ns ? req->in.module_ns : "(null)");
		nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED);
		return HG_SUCCESS;
	}

	/*
	 * Large input (rare, design §1.3): pull it from the front's READ-registered
	 * input_bulk into a local buffer BEFORE running. Small input rode inline.
	 */
	if (req->in.input_bulk != HG_BULK_NULL && req->in.input_len > NKVX_INLINE_MAX) {
		hg_class_t *cls = info->hg_class;
		hg_size_t sz = req->in.input_len;
		void *p;

		req->input_buf = malloc(req->in.input_len);
		if (req->input_buf == NULL) {
			nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_NOMEM);
			return HG_SUCCESS;
		}
		p = req->input_buf;
		ret = HG_Bulk_create(cls, 1, &p, &sz, HG_BULK_WRITE_ONLY, &req->local_input);
		if (ret != HG_SUCCESS) {
			fprintf(stderr, "nkvx_service: HG_Bulk_create(input) failed: %s\n",
				HG_Error_to_string(ret));
			nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
			return HG_SUCCESS;
		}
		ret = HG_Bulk_transfer(req->ctx, nkvx_input_pulled_cb, req,
				       HG_BULK_PULL, req->origin, req->in.input_bulk, 0,
				       req->local_input, 0, sz, HG_OP_ID_IGNORE);
		if (ret != HG_SUCCESS) {
			fprintf(stderr, "nkvx_service: HG_Bulk_transfer(PULL) failed: %s\n",
				HG_Error_to_string(ret));
			nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
			return HG_SUCCESS;
		}
		return HG_SUCCESS;	/* continue from nkvx_input_pulled_cb */
	}

	nkvx_req_run_and_deliver(req);
	return HG_SUCCESS;
}

/*
 * Publish the target's self-address string so the front can HG_Addr_lookup it
 * (design OQ-8 bootstrap = a file on the testbed). Written atomically (temp +
 * rename) so a reader never observes a half-written address. Also echoed to
 * stdout for interactive/`--addr-file -` use.
 */
static int
publish_self_addr(hg_class_t *hg, const char *addr_file)
{
	hg_addr_t self = HG_ADDR_NULL;
	char addr_str[512];
	hg_size_t addr_len = sizeof(addr_str);
	hg_return_t ret;

	ret = HG_Addr_self(hg, &self);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: HG_Addr_self failed: %s\n",
			HG_Error_to_string(ret));
		return -1;
	}
	ret = HG_Addr_to_string(hg, addr_str, &addr_len, self);
	HG_Addr_free(hg, self);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: HG_Addr_to_string failed: %s\n",
			HG_Error_to_string(ret));
		return -1;
	}

	printf("nkvx_service: listening, self addr = %s\n", addr_str);
	fflush(stdout);

	if (addr_file == NULL) {
		return 0;
	}

	char tmp[4096];
	int n = snprintf(tmp, sizeof(tmp), "%s.tmp.%d", addr_file, (int)getpid());
	if (n < 0 || (size_t)n >= sizeof(tmp)) {
		fprintf(stderr, "nkvx_service: addr-file path too long\n");
		return -1;
	}

	FILE *f = fopen(tmp, "w");
	if (f == NULL) {
		fprintf(stderr, "nkvx_service: fopen(%s) failed: %s\n", tmp, strerror(errno));
		return -1;
	}
	if (fprintf(f, "%s\n", addr_str) < 0 || fclose(f) != 0) {
		fprintf(stderr, "nkvx_service: writing %s failed: %s\n", tmp, strerror(errno));
		remove(tmp);
		return -1;
	}
	if (rename(tmp, addr_file) != 0) {
		fprintf(stderr, "nkvx_service: rename(%s -> %s) failed: %s\n",
			tmp, addr_file, strerror(errno));
		remove(tmp);
		return -1;
	}
	return 0;
}

/* Manual progress loop (design §4: the executor drives its own HG_Progress/
 * HG_Trigger). Trigger all ready completions, then block in Progress up to
 * timeout_ms so the loop wakes periodically to observe g_stop. */
static void
progress_loop(hg_context_t *ctx, unsigned int timeout_ms)
{
	while (!g_stop) {
		hg_return_t ret;
		unsigned int actual;

		do {
			actual = 0;
			ret = HG_Trigger(ctx, 0, 1, &actual);
		} while (ret == HG_SUCCESS && actual);

		if (g_stop) {
			break;
		}

		ret = HG_Progress(ctx, timeout_ms);
		if (ret != HG_SUCCESS && ret != HG_TIMEOUT) {
			fprintf(stderr, "nkvx_service: HG_Progress failed: %s\n",
				HG_Error_to_string(ret));
			break;
		}
	}
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--listen NA_INIT_STRING] [--addr-file PATH]\n"
		"          [--rados-pool POOL [--rados-namespace NS] [--rados-conf PATH] [--rados-user ID]]\n"
		"  --listen           Mercury/NA init string to listen on "
		"(default \"na+sm://\"; e.g. \"ofi+tcp://\", \"ofi+verbs://\")\n"
		"  --addr-file        publish the self-address to PATH (atomic write) for "
		"front bootstrap (design OQ-8)\n"
		"  --rados-pool       RADOS pool holding the KV objects; enables the Slice C5a\n"
		"                     cold-fill + module run (absent -> C2 skeleton, NOT_SUPPORTED)\n"
		"  --rados-namespace  RADOS namespace (== KV namespace; default namespace if omitted)\n"
		"  --rados-conf       ceph.conf path (default librados search if omitted)\n"
		"  --rados-user       ceph client id (default \"admin\")\n",
		prog);
}

int
main(int argc, char **argv)
{
	const char *na_init = "na+sm://";
	const char *addr_file = NULL;
	const char *rados_pool = NULL;
	const char *rados_ns = NULL;
	const char *rados_conf = NULL;
	const char *rados_user = NULL;
	int rc = 0;

	enum {
		OPT_RADOS_POOL = 256, OPT_RADOS_NS, OPT_RADOS_CONF, OPT_RADOS_USER,
	};
	static const struct option opts[] = {
		{ "listen",          required_argument, NULL, 'l' },
		{ "addr-file",       required_argument, NULL, 'a' },
		{ "rados-pool",      required_argument, NULL, OPT_RADOS_POOL },
		{ "rados-namespace", required_argument, NULL, OPT_RADOS_NS },
		{ "rados-conf",      required_argument, NULL, OPT_RADOS_CONF },
		{ "rados-user",      required_argument, NULL, OPT_RADOS_USER },
		{ "help",            no_argument,       NULL, 'h' },
		{ NULL,              0,                 NULL, 0 },
	};
	int c;
	while ((c = getopt_long(argc, argv, "l:a:h", opts, NULL)) != -1) {
		switch (c) {
		case 'l':
			na_init = optarg;
			break;
		case 'a':
			addr_file = optarg;
			break;
		case OPT_RADOS_POOL:
			rados_pool = optarg;
			break;
		case OPT_RADOS_NS:
			rados_ns = optarg;
			break;
		case OPT_RADOS_CONF:
			rados_conf = optarg;
			break;
		case OPT_RADOS_USER:
			rados_user = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 2;
		}
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	hg_class_t *hg = HG_Init(na_init, HG_TRUE /* listen */);
	if (hg == NULL) {
		fprintf(stderr, "nkvx_service: HG_Init(\"%s\", listen) failed\n", na_init);
		return 1;
	}

	hg_context_t *ctx = HG_Context_create(hg);
	if (ctx == NULL) {
		fprintf(stderr, "nkvx_service: HG_Context_create failed\n");
		HG_Finalize(hg);
		return 1;
	}

	hg_id_t rpc_id = HG_Register_name(hg, "nkvx_exec",
					  hg_proc_nkvx_exec_in_t,
					  hg_proc_nkvx_exec_out_t,
					  nkvx_exec_handler);
	if (rpc_id == 0) {
		fprintf(stderr, "nkvx_service: HG_Register_name(nkvx_exec) failed\n");
		rc = 1;
		goto out;
	}

	/* Slice C5a: connect librados up front (before publishing the address, so the
	 * executor can serve the first Exec the moment the front looks it up). Absent
	 * --rados-pool keeps the C2 skeleton (NOT_SUPPORTED) behaviour. */
	if (rados_pool != NULL) {
		int erc = nkvx_executor_open(rados_conf, rados_user, rados_pool, rados_ns,
					     &g_executor);
		if (erc != 0) {
			fprintf(stderr, "nkvx_service: nkvx_executor_open(pool=%s) failed: %s\n",
				rados_pool, strerror(-erc));
			rc = 1;
			goto out;
		}
		printf("nkvx_service: librados connected (pool=%s ns=%s)\n",
		       rados_pool, rados_ns ? rados_ns : "(default)");
	}

	if (publish_self_addr(hg, addr_file) != 0) {
		rc = 1;
		goto out;
	}

	progress_loop(ctx, 100 /* ms */);

out:
	if (addr_file != NULL) {
		/* Best-effort: do not leave a stale address behind on clean exit. */
		remove(addr_file);
	}
	nkvx_executor_close(g_executor);
	g_executor = NULL;
	HG_Context_destroy(ctx);
	HG_Finalize(hg);
	return rc;
}
