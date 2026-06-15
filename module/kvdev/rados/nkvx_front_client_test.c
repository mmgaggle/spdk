/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation.
 *   All rights reserved.
 */

/*
 * Standalone round-trip test for the front Mercury client core
 * (nkvx_front_client.{h,c}). No SPDK reactor: it drives nkvx_front_progress()
 * in a loop exactly as the integrated SPDK poller will, and asserts the async
 * done-callback fires with the expected status decoded back from the executor.
 *
 * Slice C7 extends it to the large-result path: it allocates a host output
 * buffer (the stand-in for the tenant DPTR), hands it to nkvx_front_forward()
 * as the result sink, and — exactly as the real bridge does — copies any inline
 * result into that same buffer. After completion it can sha256 the delivered
 * bytes and compare against --expect-sha256, proving the executor PUSH landed
 * the correct bytes (or the inline copy did) end to end over a real transport.
 *
 * Run against a live executor (nkvx_service) — see nkvx_front_test.sh (C4
 * skeleton round-trip) and nkvx_c7_test.sh (large-result bulk RMA).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <getopt.h>
#include <errno.h>

#include <openssl/sha.h>

#include "nkvx_front_client.h"

struct done_state {
	bool				called;
	enum spdk_kvdev_io_status	status;
	uint32_t			result_len;
	uint32_t			result_inline_len;
	void				*sink;		/* host output buffer */
	uint32_t			sink_cap;
};

static void
on_done(void *arg, enum spdk_kvdev_io_status status, uint32_t result_len,
	const void *result_inline, uint32_t result_inline_len)
{
	struct done_state *st = arg;

	st->status = status;
	st->result_len = result_len;
	st->result_inline_len = result_inline_len;

	/*
	 * Mirror the real bridge (kvdev_rados_nkvx_front_done): an inline result is
	 * copied into the host output buffer; a bulk-pushed result is already there
	 * (result_inline NULL/0), so this is a no-op on the push path.
	 */
	if (result_inline != NULL && result_inline_len > 0 &&
	    st->sink != NULL && st->sink_cap > 0) {
		uint32_t n = result_inline_len < st->sink_cap ? result_inline_len : st->sink_cap;
		memcpy(st->sink, result_inline, n);
	}
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

static int
parse_hex(const char *hex, unsigned char *out, size_t out_len)
{
	if (strlen(hex) != out_len * 2) {
		return -1;
	}
	for (size_t i = 0; i < out_len; i++) {
		unsigned byte;

		if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
			return -1;
		}
		out[i] = (unsigned char)byte;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	const char *na_init = "na+sm://";
	const char *target = NULL;
	const char *addr_file = NULL;
	int expect = SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
	const char *key = "ping";
	int runtime = 1;			/* WASM */
	const char *module_key = "nkvx:bytecount";
	const char *module_ns = "kvpool";
	int osize = 4096;
	int expect_result = -1;			/* >=0: assert result_len == this */
	const char *sha256_hex = NULL;		/* module bound hash (64 hex) */
	const char *expect_sha_hex = NULL;	/* expected sha256 of delivered bytes */

	enum { OPT_KEY = 256, OPT_RUNTIME, OPT_MODULE, OPT_MODULE_NS, OPT_OSIZE,
	       OPT_EXPECT_RESULT, OPT_SHA256, OPT_EXPECT_SHA };
	static const struct option opts[] = {
		{ "listen",        required_argument, NULL, 'l' },
		{ "target",        required_argument, NULL, 't' },
		{ "addr-file",     required_argument, NULL, 'a' },
		{ "expect",        required_argument, NULL, 'e' },
		{ "key",           required_argument, NULL, OPT_KEY },
		{ "runtime",       required_argument, NULL, OPT_RUNTIME },
		{ "module",        required_argument, NULL, OPT_MODULE },
		{ "module-ns",     required_argument, NULL, OPT_MODULE_NS },
		{ "osize",         required_argument, NULL, OPT_OSIZE },
		{ "expect-result", required_argument, NULL, OPT_EXPECT_RESULT },
		{ "sha256",        required_argument, NULL, OPT_SHA256 },
		{ "expect-sha256", required_argument, NULL, OPT_EXPECT_SHA },
		{ NULL,            0,                 NULL, 0 },
	};
	int c;
	while ((c = getopt_long(argc, argv, "l:t:a:e:", opts, NULL)) != -1) {
		switch (c) {
		case 'l': na_init = optarg; break;
		case 't': target = optarg; break;
		case 'a': addr_file = optarg; break;
		case 'e': expect = atoi(optarg); break;
		case OPT_KEY: key = optarg; break;
		case OPT_RUNTIME: runtime = atoi(optarg); break;
		case OPT_MODULE: module_key = optarg; break;
		case OPT_MODULE_NS: module_ns = optarg; break;
		case OPT_OSIZE: osize = atoi(optarg); break;
		case OPT_EXPECT_RESULT: expect_result = atoi(optarg); break;
		case OPT_SHA256: sha256_hex = optarg; break;
		case OPT_EXPECT_SHA: expect_sha_hex = optarg; break;
		default:
			fprintf(stderr, "usage: %s --listen NA (--target ADDR | --addr-file PATH) "
				"[--key K] [--runtime N] [--module M] [--module-ns NS] [--osize N] "
				"[--sha256 HEX] [--expect N] [--expect-result N] [--expect-sha256 HEX]\n",
				argv[0]);
			return 2;
		}
	}

	if (osize <= 0) {
		fprintf(stderr, "test: --osize must be > 0\n");
		return 2;
	}
	size_t key_len = strlen(key);
	if (key_len == 0 || key_len > SPDK_KVDEV_EXEC_KEY_MAX_LEN) {
		fprintf(stderr, "test: --key must be 1..%d bytes\n", SPDK_KVDEV_EXEC_KEY_MAX_LEN);
		return 2;
	}
	unsigned char sha256[SPDK_KV_EXEC_SHA256_LEN];
	bool sha256_valid = false;
	if (sha256_hex != NULL) {
		if (parse_hex(sha256_hex, sha256, sizeof(sha256)) != 0) {
			fprintf(stderr, "test: --sha256 must be %d hex chars\n",
				SPDK_KV_EXEC_SHA256_LEN * 2);
			return 2;
		}
		sha256_valid = true;
	}
	unsigned char expect_sha[SHA256_DIGEST_LENGTH];
	bool have_expect_sha = false;
	if (expect_sha_hex != NULL) {
		if (parse_hex(expect_sha_hex, expect_sha, sizeof(expect_sha)) != 0) {
			fprintf(stderr, "test: --expect-sha256 must be %d hex chars\n",
				SHA256_DIGEST_LENGTH * 2);
			return 2;
		}
		have_expect_sha = true;
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

	/* The host output buffer == the tenant DPTR stand-in / result sink. A poison
	 * fill makes a short or absent push detectable. */
	unsigned char *sink = malloc((size_t)osize);
	if (sink == NULL) {
		fprintf(stderr, "test: out of memory for %d-byte sink\n", osize);
		return 1;
	}
	memset(sink, 0xA5, (size_t)osize);

	struct nkvx_front *front = NULL;
	int rc = nkvx_front_init(na_init, target, &front);
	if (rc != 0) {
		fprintf(stderr, "test: nkvx_front_init(%s, %s) failed: %d\n",
			na_init, target, rc);
		free(sink);
		return 1;
	}

	nkvx_exec_in_t in;
	memset(&in, 0, sizeof(in));
	in.op_id = 0x4242;
	in.read_only = 1;
	in.runtime = (uint8_t)runtime;
	in.key_len = (uint8_t)key_len;
	memcpy(in.key, key, key_len);
	if (sha256_valid) {
		memcpy(in.sha256, sha256, sizeof(sha256));
		in.sha256_valid = 1;
	}
	in.module_key = (char *)module_key;
	in.module_ns = (char *)module_ns;
	in.osize = (uint32_t)osize;
	in.input_len = 0;
	in.input_inline = NULL;
	in.input_bulk = HG_BULK_NULL;
	in.result_sink = HG_BULK_NULL;

	struct done_state st;
	memset(&st, 0, sizeof(st));
	st.sink = sink;
	st.sink_cap = (uint32_t)osize;

	rc = nkvx_front_forward(front, &in, sink, (uint32_t)osize, on_done, &st);
	if (rc != 0) {
		fprintf(stderr, "test: nkvx_front_forward failed: %d\n", rc);
		nkvx_front_fini(front);
		free(sink);
		return 1;
	}

	/* Drive progress exactly as the SPDK poller will, until the cb fires. */
	for (int i = 0; i < 200000 && !st.called; i++) {
		int prog = nkvx_front_progress(front, 100);
		if (prog < 0) {
			fprintf(stderr, "test: nkvx_front_progress failed: %d\n", prog);
			nkvx_front_fini(front);
			free(sink);
			return 1;
		}
	}

	int ret = 1;
	if (!st.called) {
		fprintf(stderr, "test: FAIL (completion never fired)\n");
		goto done;
	}
	if ((int)st.status != expect) {
		fprintf(stderr, "test: FAIL (status %d != expected %d)\n",
			(int)st.status, expect);
		goto done;
	}
	if (expect_result >= 0 && st.result_len != (uint32_t)expect_result) {
		fprintf(stderr, "test: FAIL (result_len %u != expected %d)\n",
			st.result_len, expect_result);
		goto done;
	}
	if (have_expect_sha) {
		uint32_t n = st.result_len < (uint32_t)osize ? st.result_len : (uint32_t)osize;
		unsigned char got[SHA256_DIGEST_LENGTH];

		SHA256(sink, n, got);
		if (memcmp(got, expect_sha, sizeof(got)) != 0) {
			fprintf(stderr, "test: FAIL (delivered %u bytes sha256 mismatch — "
				"torn/short bulk push?)\n", n);
			goto done;
		}
		printf("test: delivered %u bytes, sha256 verified\n", n);
	}
	printf("test: PASS (status=%d result_len=%u inline_len=%u)\n",
	       (int)st.status, st.result_len, st.result_inline_len);
	ret = 0;

done:
	nkvx_front_fini(front);
	free(sink);
	return ret;
}
