/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation.
 *   All rights reserved.
 */

/*
 * Standalone round-trip test for the Slice C4 front Mercury client core
 * (nkvx_front_client.{h,c}). No SPDK reactor: it drives nkvx_front_progress()
 * in a loop exactly as the integrated SPDK poller will, and asserts the async
 * done-callback fires with the expected status decoded back from the executor.
 *
 * Run against a live Slice C2 executor (nkvx_service) — see nkvx_front_test.sh,
 * which boots the executor, passes its published address here, and checks the
 * skeleton reply (NOT_SUPPORTED).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <getopt.h>
#include <errno.h>

#include "nkvx_front_client.h"

struct done_state {
	bool				called;
	enum spdk_kvdev_io_status	status;
	uint32_t			result_len;
	uint32_t			result_inline_len;
};

static void
on_done(void *arg, enum spdk_kvdev_io_status status, uint32_t result_len,
	const void *result_inline, uint32_t result_inline_len)
{
	struct done_state *st = arg;

	(void)result_inline;
	st->status = status;
	st->result_len = result_len;
	st->result_inline_len = result_inline_len;
	st->called = true;
}

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

int
main(int argc, char **argv)
{
	const char *na_init = "na+sm://";
	const char *target = NULL;
	const char *addr_file = NULL;
	int expect = SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;

	static const struct option opts[] = {
		{ "listen",    required_argument, NULL, 'l' },
		{ "target",    required_argument, NULL, 't' },
		{ "addr-file", required_argument, NULL, 'a' },
		{ "expect",    required_argument, NULL, 'e' },
		{ NULL,        0,                 NULL, 0 },
	};
	int c;
	while ((c = getopt_long(argc, argv, "l:t:a:e:", opts, NULL)) != -1) {
		switch (c) {
		case 'l': na_init = optarg; break;
		case 't': target = optarg; break;
		case 'a': addr_file = optarg; break;
		case 'e': expect = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s --listen NA (--target ADDR | --addr-file PATH) [--expect N]\n",
				argv[0]);
			return 2;
		}
	}

	char addr_buf[512];
	if (target == NULL && addr_file != NULL) {
		if (read_addr_file(addr_file, addr_buf, sizeof(addr_buf)) != 0) {
			fprintf(stderr, "test: cannot read addr-file %s: %s\n",
				addr_file, strerror(errno));
			return 1;
		}
		target = addr_buf;
	}
	if (target == NULL) {
		fprintf(stderr, "test: need --target or --addr-file\n");
		return 2;
	}

	struct nkvx_front *front = NULL;
	int rc = nkvx_front_init(na_init, target, &front);
	if (rc != 0) {
		fprintf(stderr, "test: nkvx_front_init(%s, %s) failed: %d\n",
			na_init, target, rc);
		return 1;
	}

	/* A minimal but well-formed request; the C2 skeleton declines it. */
	nkvx_exec_in_t in;
	memset(&in, 0, sizeof(in));
	in.op_id = 0x4242;
	in.read_only = 1;
	in.runtime = 1;			/* WASM */
	in.key_len = 4;
	memcpy(in.key, "ping", 4);
	in.module_key = (char *)"nkvx:bytecount";
	in.module_ns = (char *)"kvpool";
	in.osize = 4096;
	in.input_bulk = HG_BULK_NULL;
	in.result_sink = HG_BULK_NULL;

	struct done_state st;
	memset(&st, 0, sizeof(st));

	rc = nkvx_front_forward(front, &in, on_done, &st);
	if (rc != 0) {
		fprintf(stderr, "test: nkvx_front_forward failed: %d\n", rc);
		nkvx_front_fini(front);
		return 1;
	}

	/* Drive progress exactly as the SPDK poller will, until the cb fires. A
	 * bounded number of iterations guards against a hung executor. */
	int prog;
	for (int i = 0; i < 10000 && !st.called; i++) {
		prog = nkvx_front_progress(front, 100);
		if (prog < 0) {
			fprintf(stderr, "test: nkvx_front_progress failed: %d\n", prog);
			nkvx_front_fini(front);
			return 1;
		}
	}

	int ret = 1;
	if (!st.called) {
		fprintf(stderr, "test: FAIL (completion never fired)\n");
	} else if ((int)st.status != expect) {
		fprintf(stderr, "test: FAIL (status %d != expected %d)\n",
			(int)st.status, expect);
	} else {
		printf("test: PASS (status=%d result_len=%u inline_len=%u)\n",
		       (int)st.status, st.result_len, st.result_inline_len);
		ret = 0;
	}

	nkvx_front_fini(front);
	return ret;
}
