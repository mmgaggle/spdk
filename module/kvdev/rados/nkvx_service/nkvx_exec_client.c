/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation.
 *   All rights reserved.
 */

/*
 * Minimal `nkvx_exec` ORIGIN (test client) — Slice C2.
 *
 * A standalone non-SPDK Mercury client that forwards ONE nkvx_exec request to a
 * running executor service (nkvx_service) and prints the reply. It stands in for
 * the real SPDK front (rados-nkv) so the C2 transport + contract round-trip can
 * be validated with no SPDK reactor (design §5 rung 1: "a minimal main can stand
 * in for the front to exercise the contract"). Slice C4 replaces this with the
 * SPDK-poller-driven front client.
 *
 * It drives Mercury progress synchronously (manual HG_Progress/HG_Trigger until
 * the forward callback fires), looks the target up from a string or an addr-file
 * published by the service, and exits 0 iff the reply status matches --expect
 * (default NOT_SUPPORTED, the C2 skeleton reply).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <getopt.h>
#include <errno.h>

#include <mercury.h>

#include "nkvx_exec_rpc.h"

struct forward_ctx {
	bool		done;
	hg_return_t	cb_ret;		/* HG_Forward callback ret */
	bool		have_out;
	int32_t		wire_status;
	uint32_t	result_len;
};

static hg_return_t
forward_cb(const struct hg_cb_info *info)
{
	struct forward_ctx *fc = info->arg;

	fc->cb_ret = info->ret;
	if (info->ret == HG_SUCCESS) {
		nkvx_exec_out_t out;
		memset(&out, 0, sizeof(out));
		hg_return_t ret = HG_Get_output(info->info.forward.handle, &out);
		if (ret == HG_SUCCESS) {
			fc->wire_status = out.status;
			fc->result_len = out.result_len;
			fc->have_out = true;
			HG_Free_output(info->info.forward.handle, &out);
		} else {
			fprintf(stderr, "client: HG_Get_output failed: %s\n",
				HG_Error_to_string(ret));
			fc->cb_ret = ret;
		}
	}
	fc->done = true;
	return HG_SUCCESS;
}

/* Read a one-line address from a file published by the service (--addr-file). */
static int
read_addr_file(const char *path, char *buf, size_t buf_sz)
{
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return -1;
	}
	char *p = fgets(buf, (int)buf_sz, f);
	fclose(f);
	if (p == NULL) {
		return -1;
	}
	buf[strcspn(buf, "\r\n")] = '\0';
	return (buf[0] != '\0') ? 0 : -1;
}

static const char *
status_name(enum spdk_kvdev_io_status s)
{
	switch (s) {
	case SPDK_KVDEV_IO_STATUS_SUCCESS:          return "SUCCESS";
	case SPDK_KVDEV_IO_STATUS_FAILED:           return "FAILED";
	case SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST:    return "KEY_NOT_EXIST";
	case SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
	case SPDK_KVDEV_IO_STATUS_INVALID:          return "INVALID";
	case SPDK_KVDEV_IO_STATUS_NOMEM:            return "NOMEM";
	case SPDK_KVDEV_IO_STATUS_KEY_EXIST:        return "KEY_EXIST";
	case SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED:    return "NOT_SUPPORTED";
	case SPDK_KVDEV_IO_STATUS_ABORTED:          return "ABORTED";
	case SPDK_KVDEV_IO_STATUS_READ_ONLY:        return "READ_ONLY";
	default:                                    return "(unknown)";
	}
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--listen NA] (--target ADDR | --addr-file PATH) [--expect STATUS]\n"
		"  --listen     local Mercury init string (default \"na+sm://\"; must match the service transport)\n"
		"  --target     executor self-address string to forward to\n"
		"  --addr-file  read the executor address from PATH (published by nkvx_service)\n"
		"  --expect     expected reply status int (default -7 = NOT_SUPPORTED); exit 1 on mismatch\n",
		prog);
}

int
main(int argc, char **argv)
{
	const char *na_init = "na+sm://";
	const char *target = NULL;
	const char *addr_file = NULL;
	int expect = SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
	int rc = 1;

	static const struct option opts[] = {
		{ "listen",    required_argument, NULL, 'l' },
		{ "target",    required_argument, NULL, 't' },
		{ "addr-file", required_argument, NULL, 'a' },
		{ "expect",    required_argument, NULL, 'e' },
		{ "help",      no_argument,       NULL, 'h' },
		{ NULL,        0,                 NULL, 0 },
	};
	int c;
	while ((c = getopt_long(argc, argv, "l:t:a:e:h", opts, NULL)) != -1) {
		switch (c) {
		case 'l': na_init = optarg; break;
		case 't': target = optarg; break;
		case 'a': addr_file = optarg; break;
		case 'e': expect = atoi(optarg); break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}

	char addr_buf[512];
	if (target == NULL && addr_file != NULL) {
		if (read_addr_file(addr_file, addr_buf, sizeof(addr_buf)) != 0) {
			fprintf(stderr, "client: cannot read addr-file %s: %s\n",
				addr_file, strerror(errno));
			return 1;
		}
		target = addr_buf;
	}
	if (target == NULL) {
		usage(argv[0]);
		return 2;
	}

	hg_class_t *hg = HG_Init(na_init, HG_FALSE /* origin */);
	if (hg == NULL) {
		fprintf(stderr, "client: HG_Init(\"%s\") failed\n", na_init);
		return 1;
	}
	hg_context_t *ctx = HG_Context_create(hg);
	if (ctx == NULL) {
		fprintf(stderr, "client: HG_Context_create failed\n");
		HG_Finalize(hg);
		return 1;
	}

	/* Origin registers the same name/procs to obtain the matching RPC id; no
	 * handler on the origin side (NULL). */
	hg_id_t rpc_id = HG_Register_name(hg, "nkvx_exec",
					  hg_proc_nkvx_exec_in_t,
					  hg_proc_nkvx_exec_out_t,
					  NULL);
	if (rpc_id == 0) {
		fprintf(stderr, "client: HG_Register_name failed\n");
		goto out_ctx;
	}

	hg_addr_t addr = HG_ADDR_NULL;
	hg_return_t ret = HG_Addr_lookup2(hg, target, &addr);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "client: HG_Addr_lookup2(%s) failed: %s\n",
			target, HG_Error_to_string(ret));
		goto out_ctx;
	}

	hg_handle_t handle = HG_HANDLE_NULL;
	ret = HG_Create(ctx, addr, rpc_id, &handle);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "client: HG_Create failed: %s\n", HG_Error_to_string(ret));
		goto out_addr;
	}

	/* A minimal but well-formed request (the C2 skeleton declines it anyway). */
	nkvx_exec_in_t in;
	memset(&in, 0, sizeof(in));
	in.op_id = 0x4242;
	in.read_only = 1;
	in.runtime = 1;			/* WASM */
	in.caps = 0;
	in.key_len = 4;
	memcpy(in.key, "ping", 4);
	in.sha256_valid = 0;
	in.module_key = (char *)"nkvx:bytecount";
	in.module_ns = (char *)"kvpool";
	in.osize = 4096;
	in.input_len = 0;
	in.input_inline = NULL;
	in.input_bulk = HG_BULK_NULL;
	in.result_sink = HG_BULK_NULL;

	struct forward_ctx fc;
	memset(&fc, 0, sizeof(fc));

	ret = HG_Forward(handle, forward_cb, &fc, &in);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "client: HG_Forward failed: %s\n", HG_Error_to_string(ret));
		goto out_handle;
	}

	/* Drive progress until the forward callback fires. */
	while (!fc.done) {
		unsigned int actual;
		do {
			actual = 0;
			ret = HG_Trigger(ctx, 0, 1, &actual);
		} while (ret == HG_SUCCESS && actual);
		if (fc.done) {
			break;
		}
		ret = HG_Progress(ctx, 1000);
		if (ret != HG_SUCCESS && ret != HG_TIMEOUT) {
			fprintf(stderr, "client: HG_Progress failed: %s\n",
				HG_Error_to_string(ret));
			goto out_handle;
		}
	}

	if (fc.cb_ret != HG_SUCCESS) {
		fprintf(stderr, "client: RPC failed: %s\n", HG_Error_to_string(fc.cb_ret));
		goto out_handle;
	}
	if (!fc.have_out) {
		fprintf(stderr, "client: no output decoded\n");
		goto out_handle;
	}

	enum spdk_kvdev_io_status status = nkvx_status_from_wire(fc.wire_status);
	printf("client: reply status=%d (%s) result_len=%u\n",
	       fc.wire_status, status_name(status), fc.result_len);

	if (fc.wire_status == expect) {
		printf("client: PASS (status matches expected %d)\n", expect);
		rc = 0;
	} else {
		fprintf(stderr, "client: FAIL (status %d != expected %d)\n",
			fc.wire_status, expect);
		rc = 1;
	}

out_handle:
	HG_Destroy(handle);
out_addr:
	HG_Addr_free(hg, addr);
out_ctx:
	HG_Context_destroy(ctx);
	HG_Finalize(hg);
	return rc;
}
