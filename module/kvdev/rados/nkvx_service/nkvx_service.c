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
 * The `nkvx_exec` RPC handler (C2 STUB).
 *
 * Decodes the request through the C3 hg_proc serializer (proving the wire
 * contract round-trips against a real front), logs it, and replies
 * NOT_SUPPORTED. No object cold-fill, no wasmtime — those are Slice C5.
 *
 * Mercury ownership rules: HG_Get_input allocates the decoded request (strings,
 * inline input); HG_Free_input releases it via the proc HG_FREE path. The
 * handle is reference-counted — HG_Destroy drops our reference once the response
 * is queued.
 */
static hg_return_t
nkvx_exec_handler(hg_handle_t handle)
{
	nkvx_exec_in_t in;
	nkvx_exec_out_t out;
	hg_return_t ret;

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	ret = HG_Get_input(handle, &in);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: HG_Get_input failed: %s\n",
			HG_Error_to_string(ret));
		/* Cannot form a meaningful reply without the request; drop it. */
		HG_Destroy(handle);
		return ret;
	}

	if (g_executor != NULL) {
		/* Slice C5a: cold-fill from RADOS and run the module. The run is
		 * synchronous here — it briefly occupies the progress loop, acceptable
		 * on a dedicated executor (off-loop execution is a later optimization). */
		nkvx_executor_run(g_executor, &in, &out);
		fprintf(stderr,
			"nkvx_service: nkvx_exec op_id=%u key_len=%u runtime=%u "
			"module_key=%s module_ns=%s -> status=%d result_len=%u\n",
			in.op_id, in.key_len, in.runtime,
			in.module_key ? in.module_key : "(null)",
			in.module_ns ? in.module_ns : "(null)",
			out.status, out.result_len);
	} else {
		/*
		 * No cluster configured (C2 skeleton mode): decode + decline so the
		 * transport/contract still round-trips with no RADOS. NOT_SUPPORTED
		 * maps at the tenant to INVALID_OPCODE (design §3).
		 */
		fprintf(stderr,
			"nkvx_service: nkvx_exec op_id=%u key_len=%u runtime=%u "
			"module_key=%s module_ns=%s -> NOT_SUPPORTED (no cluster)\n",
			in.op_id, in.key_len, in.runtime,
			in.module_key ? in.module_key : "(null)",
			in.module_ns ? in.module_ns : "(null)");
		out.status = nkvx_status_to_wire(SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED);
		out.result_len = 0;
		out.result_inline_len = 0;
		out.result_inline = NULL;
	}

	ret = HG_Respond(handle, NULL, NULL, &out);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: HG_Respond failed: %s\n",
			HG_Error_to_string(ret));
	}

	/* HG_Respond encoded the response synchronously, so the result buffer can be
	 * released now. */
	nkvx_exec_out_free(&out);
	HG_Free_input(handle, &in);
	HG_Destroy(handle);

	/*
	 * We have already taken responsibility for the reply (HG_Respond) and
	 * dropped our handle reference (HG_Destroy). Return HG_SUCCESS regardless
	 * of the HG_Respond result: a non-SUCCESS return here makes Mercury's
	 * input-processing path auto-issue a SECOND error response on a handle we
	 * just responded to and destroyed (mercury_core.c hg_core_process_input).
	 * The respond failure is logged above; do not propagate it.
	 */
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
