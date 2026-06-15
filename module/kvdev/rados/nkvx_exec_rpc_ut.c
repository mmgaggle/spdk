/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation.
 *   All rights reserved.
 */

/*
 * Standalone round-trip unit test for the Slice C3 Exec RPC contract.
 *
 * Builds directly against Mercury (no SPDK unit-test framework, no reactor): it
 * round-trips a populated request and response through the hg_proc serializers
 * using an in-memory Mercury proc (hg_proc_create_set with HG_ENCODE, then
 * HG_DECODE over the same buffer) and asserts field-for-field equality. It also
 * exercises the wire-status table (all 10 enum values + unknown->FAILED) and the
 * input/result inline edge cases (empty, <4 KiB, exactly the 4 KiB boundary,
 * over-boundary => bulk) and truncation/result_len semantics.
 *
 * Built+run by: make -f Makefile.ut (see that file for the Mercury flags).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <mercury.h>
#include <mercury_proc.h>

#include "nkvx_exec_rpc.h"

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do {						\
	g_checks++;							\
	if (!(cond)) {							\
		g_failures++;						\
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);	\
		fprintf(stderr, __VA_ARGS__);				\
		fprintf(stderr, "\n");					\
	}								\
} while (0)

/* A throwaway HG class on the null/sm transport just to mint hg_proc objects.
 * hg_proc needs a class for checksum/extra-buf bookkeeping; "na+sm" inits with
 * no network. */
static hg_class_t *g_hg_class;

/*
 * Encode `src` into a fixed scratch buffer, then decode it back into `dst`,
 * exercising the real Mercury in-memory proc both ways. The largest payload
 * that is ever serialized is the 4 KiB inline cap plus small fixed fields (the
 * 64 MiB cases ride bulk handles, so only lengths/handles are encoded), so a
 * 64 KiB scratch buffer is comfortably sufficient.
 *
 * `procfn` is the hg_proc routine; `srctmp`/`dst` are caller-typed buffers.
 */
#define PROC_SCRATCH_SIZE (64u * 1024u)

static void
proc_roundtrip(hg_return_t (*procfn)(hg_proc_t, void *), void *src_tmp,
	       void *dst, size_t struct_size)
{
	static unsigned char buf[PROC_SCRATCH_SIZE];
	hg_proc_t proc;
	hg_return_t ret;
	hg_size_t used;

	memset(buf, 0, sizeof(buf));

	/* Encode src_tmp into buf (procfn may mutate src_tmp on FREE/ENCODE; the
	 * caller passes a throwaway copy). */
	ret = hg_proc_create_set(g_hg_class, buf, sizeof(buf), HG_ENCODE, HG_NOHASH, &proc);
	assert(ret == HG_SUCCESS);
	ret = procfn(proc, src_tmp);
	assert(ret == HG_SUCCESS);
	ret = hg_proc_flush(proc);
	assert(ret == HG_SUCCESS);
	used = hg_proc_get_size_used(proc);
	assert(used <= sizeof(buf));
	hg_proc_free(proc);

	/* Decode into dst. */
	memset(dst, 0, struct_size);
	ret = hg_proc_create_set(g_hg_class, buf, sizeof(buf), HG_DECODE, HG_NOHASH, &proc);
	assert(ret == HG_SUCCESS);
	ret = procfn(proc, dst);
	assert(ret == HG_SUCCESS);
	hg_proc_free(proc);
}

static void
proc_roundtrip_in(const nkvx_exec_in_t *src, nkvx_exec_in_t *dst)
{
	nkvx_exec_in_t tmp = *src;	/* throwaway copy for the encode pass */

	proc_roundtrip(hg_proc_nkvx_exec_in_t, &tmp, dst, sizeof(*dst));
}

static void
proc_roundtrip_out(const nkvx_exec_out_t *src, nkvx_exec_out_t *dst)
{
	nkvx_exec_out_t tmp = *src;

	proc_roundtrip(hg_proc_nkvx_exec_out_t, &tmp, dst, sizeof(*dst));
}

static void
test_request_roundtrip(uint32_t input_len, const char *label)
{
	nkvx_exec_in_t in, out;
	uint8_t *input = NULL;
	uint32_t i;
	int expect_inline = (input_len <= NKVX_INLINE_MAX);

	memset(&in, 0, sizeof(in));
	in.op_id = 0xDEADBEEF;
	in.read_only = 1;
	in.runtime = SPDK_KV_EXEC_RUNTIME_WASM;
	in.caps = 3;	/* LARGE tier */
	in.key_len = 200;	/* exercise a >16-byte exec key up to 255 */
	for (i = 0; i < in.key_len; i++) {
		in.key[i] = (uint8_t)(i * 7 + 1);
	}
	for (i = 0; i < SPDK_KV_EXEC_SHA256_LEN; i++) {
		in.sha256[i] = (uint8_t)(0xA0 + i);
	}
	in.sha256_valid = 1;
	in.module_key = strdup("kvcache.wasm");
	in.module_ns = strdup("nkvx-pool/modules");
	in.osize = 64u * 1024u * 1024u;	/* 64 MiB host output buffer */
	in.input_len = input_len;

	if (input_len > 0) {
		input = malloc(input_len);
		assert(input != NULL);
		for (i = 0; i < input_len; i++) {
			input[i] = (uint8_t)(i & 0xFF);
		}
	}
	if (expect_inline) {
		in.input_inline = input;	/* inline carries the bytes */
		in.input_bulk = HG_BULK_NULL;
	} else {
		in.input_inline = NULL;		/* large: bytes ride the bulk handle */
		in.input_bulk = HG_BULK_NULL;	/* handle creation is Slice C7; NULL here */
	}
	in.result_sink = HG_BULK_NULL;

	proc_roundtrip_in(&in, &out);

	CHECK(out.op_id == in.op_id, "[%s] op_id", label);
	CHECK(out.read_only == in.read_only, "[%s] read_only", label);
	CHECK(out.runtime == in.runtime, "[%s] runtime", label);
	CHECK(out.caps == in.caps, "[%s] caps", label);
	CHECK(out.key_len == in.key_len, "[%s] key_len", label);
	CHECK(memcmp(out.key, in.key, in.key_len) == 0, "[%s] key bytes", label);
	CHECK(memcmp(out.sha256, in.sha256, SPDK_KV_EXEC_SHA256_LEN) == 0, "[%s] sha256", label);
	CHECK(out.sha256_valid == in.sha256_valid, "[%s] sha256_valid", label);
	CHECK(out.module_key && strcmp(out.module_key, in.module_key) == 0, "[%s] module_key", label);
	CHECK(out.module_ns && strcmp(out.module_ns, in.module_ns) == 0, "[%s] module_ns", label);
	CHECK(out.osize == in.osize, "[%s] osize", label);
	CHECK(out.input_len == in.input_len, "[%s] input_len (TRUE len preserved)", label);

	if (expect_inline) {
		if (input_len == 0) {
			CHECK(out.input_inline == NULL, "[%s] empty input -> NULL inline", label);
		} else {
			CHECK(out.input_inline != NULL, "[%s] inline buf present", label);
			CHECK(memcmp(out.input_inline, input, input_len) == 0,
			      "[%s] inline input bytes match", label);
		}
	} else {
		/* Over the 4 KiB boundary: NOT carried inline; handle would carry it. */
		CHECK(out.input_inline == NULL, "[%s] >4KiB input not inlined", label);
	}

	nkvx_exec_in_free(&out);
	free(input);
	free(in.module_key);	/* free the strdup'd source strings (decoded copy freed above) */
	free(in.module_ns);
}

static void
test_response_roundtrip(int32_t status, uint32_t result_len, uint32_t osize,
			int use_sink, const char *label)
{
	nkvx_exec_out_t in, out;
	uint8_t *result = NULL;
	uint32_t delivered = (result_len < osize) ? result_len : osize; /* min */
	uint32_t inline_len;
	int inline_path = (!use_sink && delivered <= NKVX_INLINE_MAX);
	uint32_t i;

	memset(&in, 0, sizeof(in));
	in.status = status;
	in.result_len = result_len;	/* TRUE length, even when truncated */

	if (inline_path && delivered > 0) {
		inline_len = delivered;
		result = malloc(inline_len);
		assert(result != NULL);
		for (i = 0; i < inline_len; i++) {
			result[i] = (uint8_t)(0x5A ^ (i & 0xFF));
		}
		in.result_inline = result;
		in.result_inline_len = inline_len;
	} else {
		in.result_inline = NULL;
		in.result_inline_len = 0;
	}

	proc_roundtrip_out(&in, &out);

	CHECK(out.status == in.status, "[%s] status int32 preserved", label);
	CHECK(out.result_len == in.result_len, "[%s] result_len TRUE len preserved", label);
	CHECK(out.result_inline_len == in.result_inline_len, "[%s] result_inline_len", label);
	if (in.result_inline_len > 0) {
		CHECK(out.result_inline != NULL && memcmp(out.result_inline, in.result_inline,
				in.result_inline_len) == 0, "[%s] inline result bytes", label);
	} else {
		CHECK(out.result_inline == NULL, "[%s] no inline result", label);
	}

	nkvx_exec_out_free(&out);
	free(result);
}

static void
test_status_table(void)
{
	struct { enum spdk_kvdev_io_status e; int32_t wire; } tbl[] = {
		{ SPDK_KVDEV_IO_STATUS_SUCCESS,          0 },
		{ SPDK_KVDEV_IO_STATUS_FAILED,          -1 },
		{ SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST,   -2 },
		{ SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL,-3 },
		{ SPDK_KVDEV_IO_STATUS_INVALID,         -4 },
		{ SPDK_KVDEV_IO_STATUS_NOMEM,           -5 },
		{ SPDK_KVDEV_IO_STATUS_KEY_EXIST,       -6 },
		{ SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED,   -7 },
		{ SPDK_KVDEV_IO_STATUS_ABORTED,         -8 },
		{ SPDK_KVDEV_IO_STATUS_READ_ONLY,       -9 },
	};
	size_t n = sizeof(tbl) / sizeof(tbl[0]);
	size_t i;

	CHECK(n == 10, "status table covers all 10 enum values");

	for (i = 0; i < n; i++) {
		int32_t w = nkvx_status_to_wire(tbl[i].e);
		enum spdk_kvdev_io_status back = nkvx_status_from_wire(w);
		CHECK(w == tbl[i].wire, "to_wire(%d) == %d", (int)tbl[i].e, (int)tbl[i].wire);
		CHECK(back == tbl[i].e, "from_wire round-trip for %d", (int)tbl[i].e);
	}

	/* Unknown wire values -> FAILED (design §3). */
	CHECK(nkvx_status_from_wire(1) == SPDK_KVDEV_IO_STATUS_FAILED, "unknown +1 -> FAILED");
	CHECK(nkvx_status_from_wire(-100) == SPDK_KVDEV_IO_STATUS_FAILED, "unknown -100 -> FAILED");
	CHECK(nkvx_status_from_wire(0x7FFFFFFF) == SPDK_KVDEV_IO_STATUS_FAILED, "INT32_MAX -> FAILED");
}

int
main(void)
{
	hg_return_t ret;

	/* "na+sm" inits Mercury with the shared-memory NA plugin and no real
	 * network — enough to mint hg_proc objects for in-memory (de)serialization. */
	g_hg_class = HG_Init("na+sm://", HG_TRUE);
	if (g_hg_class == NULL) {
		fprintf(stderr, "HG_Init(na+sm) failed; trying na+sm without listen\n");
		g_hg_class = HG_Init("na+sm://", HG_FALSE);
	}
	assert(g_hg_class != NULL);

	printf("== status table ==\n");
	test_status_table();

	printf("== request round-trip: input edge cases ==\n");
	test_request_roundtrip(0, "empty input");
	test_request_roundtrip(1, "1-byte input");
	test_request_roundtrip(100, "100-byte input");
	test_request_roundtrip(NKVX_INLINE_MAX - 1, "4KiB-1 (just under boundary)");
	test_request_roundtrip(NKVX_INLINE_MAX, "exactly 4KiB (inline boundary)");
	test_request_roundtrip(NKVX_INLINE_MAX + 1, "4KiB+1 (over -> bulk)");
	test_request_roundtrip(64u * 1024u * 1024u, "64 MiB (over -> bulk)");

	printf("== response round-trip: result/truncation cases ==\n");
	/* SUCCESS, small inline result. */
	test_response_roundtrip(0, 256, 1u << 20, 0, "success small inline");
	/* SUCCESS, result exactly at the 4KiB inline boundary. */
	test_response_roundtrip(0, NKVX_INLINE_MAX, 1u << 20, 0, "success 4KiB inline boundary");
	/* SUCCESS, empty result. */
	test_response_roundtrip(0, 0, 1u << 20, 0, "success empty result");
	/* BUFFER_TOO_SMALL: TRUE result_len (10) exceeds osize (4); delivered=4 inline,
	 * but result_len must still carry the TRUE length 10. */
	test_response_roundtrip(-3, 10, 4, 0, "truncated: result_len > osize");
	/* Large result truncated to a small osize -> still inline-delivered min(). */
	test_response_roundtrip(-3, 1u << 20, 100, 0, "truncated 1MiB->100");
	/* Large result via WRITE-bulk sink: no inline bytes, result_len is TRUE len. */
	test_response_roundtrip(0, 64u * 1024u * 1024u, 64u * 1024u * 1024u, 1, "64MiB via sink");
	/* A non-success status with no inline payload. */
	test_response_roundtrip(-2, 0, 1u << 20, 0, "KEY_NOT_EXIST");

	HG_Finalize(g_hg_class);

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures == 0) {
		printf("PASS\n");
		return 0;
	}
	printf("FAIL\n");
	(void)ret;
	return 1;
}
