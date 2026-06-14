/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_internal/cunit.h"

#include "nvme/nvme_kv.c"

#include "common/lib/test_env.c"
#include "common/lib/nvme/cmd_ut_common.h"

/*
 * KV Exec carries its key length-prefixed at the HEAD of the DPTR request
 * payload (ADR-0014 Option 1): [u16 key_len][key][input]. CDW10 (kv.vsize)
 * holds the TOTAL request payload length, NOT just the input length, and the
 * key does NOT ride the inline CDW2/3/14/15 slots. These tests pin that wire
 * encoding so it cannot silently regress against the target parser in
 * lib/nvmf/ctrlr_kvdev.c (case SPDK_NVME_OPC_KV_EXEC).
 */

static struct nvme_request *g_submitted_req;
static int g_submit_rc;

int
nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	g_submitted_req = req;
	return g_submit_rc;
}

/* Recover the staged DPTR buffer the controller will gather then overwrite. */
static uint8_t *
exec_payload(struct nvme_request *req)
{
	return req->payload.contig_or_cb_arg;
}

static void
test_kv_exec_payload_head_encoding(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair qpair = {};
	struct spdk_nvme_ns ns = {};
	const uint8_t key[] = {0xAA, 0xBB, 0xCC, 0xDD};
	const uint8_t input[] = {0x10, 0x20, 0x30};
	uint8_t output[64];
	struct nvme_request *req;
	uint16_t klp;
	int rc;

	ns.id = 7;
	ut_qpair_init(&qpair, &ctrlr);

	memset(output, 0, sizeof(output));
	g_submitted_req = NULL;
	g_submit_rc = 0;

	rc = spdk_nvme_kv_exec(&ns, &qpair, key, sizeof(key), 0x42,
			       input, sizeof(input), output, sizeof(output),
			       NULL, NULL);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_submitted_req != NULL);
	req = g_submitted_req;

	/* Single bidirectional DPTR is the caller's output buffer, full length. */
	CU_ASSERT(exec_payload(req) == output);
	CU_ASSERT(req->payload.size == sizeof(output));

	/* Opcode + nsid. */
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_KV_EXEC);
	CU_ASSERT(req->cmd.nsid == 7);

	/* CDW10 == TOTAL payload (u16 header + key + input), per the target ABI. */
	CU_ASSERT(req->cmd.cdw10_bits.kv.vsize ==
		  sizeof(uint16_t) + sizeof(key) + sizeof(input));
	/* CDW12 == output buffer size; CDW13 == op_id. */
	CU_ASSERT(req->cmd.cdw12_bits.kv_exec.osize == sizeof(output));
	CU_ASSERT(req->cmd.cdw13_bits.kv_exec.op_id == 0x42);

	/* The key must NOT ride the inline CDW slots for Exec. */
	CU_ASSERT(req->cmd.cdw2 == 0);
	CU_ASSERT(req->cmd.cdw3 == 0);
	CU_ASSERT(req->cmd.cdw14 == 0);
	CU_ASSERT(req->cmd.cdw15 == 0);
	CU_ASSERT(req->cmd.cdw11_bits.kv.kl == 0);

	/* DPTR head: [u16 key_len][key][input], matching target memcpy(&klp,data,2). */
	memcpy(&klp, output, sizeof(klp));
	CU_ASSERT(klp == sizeof(key));
	CU_ASSERT(memcmp(output + sizeof(uint16_t), key, sizeof(key)) == 0);
	CU_ASSERT(memcmp(output + sizeof(uint16_t) + sizeof(key), input,
			 sizeof(input)) == 0);

	ut_qpair_cleanup(&qpair);
}

/* In-place caller: input aliases output (input bytes initially at buffer head). */
static void
test_kv_exec_input_aliases_output(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair qpair = {};
	struct spdk_nvme_ns ns = {};
	const uint8_t key[] = {0x01, 0x02};
	const uint8_t input[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x55};
	uint8_t buf[32];
	struct nvme_request *req;
	uint16_t klp;
	int rc;

	ns.id = 1;
	ut_qpair_init(&qpair, &ctrlr);

	memset(buf, 0, sizeof(buf));
	memcpy(buf, input, sizeof(input)); /* caller stages input at the head */

	g_submitted_req = NULL;
	g_submit_rc = 0;

	/* input == output == buf, mirroring test/nvmf/kv/kv_host. */
	rc = spdk_nvme_kv_exec(&ns, &qpair, key, sizeof(key), 9,
			       buf, sizeof(input), buf, sizeof(buf),
			       NULL, NULL);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_submitted_req != NULL);
	req = g_submitted_req;

	CU_ASSERT(req->cmd.cdw10_bits.kv.vsize ==
		  sizeof(uint16_t) + sizeof(key) + sizeof(input));

	/* Input must have been relocated (not clobbered by the header+key). */
	memcpy(&klp, buf, sizeof(klp));
	CU_ASSERT(klp == sizeof(key));
	CU_ASSERT(memcmp(buf + sizeof(uint16_t), key, sizeof(key)) == 0);
	CU_ASSERT(memcmp(buf + sizeof(uint16_t) + sizeof(key), input,
			 sizeof(input)) == 0);

	ut_qpair_cleanup(&qpair);
}

/* No input: just [u16 key_len][key]; CDW10 == 2 + key_len. */
static void
test_kv_exec_zero_input(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair qpair = {};
	struct spdk_nvme_ns ns = {};
	const uint8_t key[] = {0x11, 0x22, 0x33};
	uint8_t output[16];
	struct nvme_request *req;
	uint16_t klp;
	int rc;

	ns.id = 2;
	ut_qpair_init(&qpair, &ctrlr);

	memset(output, 0xFF, sizeof(output));
	g_submitted_req = NULL;
	g_submit_rc = 0;

	rc = spdk_nvme_kv_exec(&ns, &qpair, key, sizeof(key), 3,
			       NULL, 0, output, sizeof(output), NULL, NULL);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_submitted_req != NULL);
	req = g_submitted_req;

	CU_ASSERT(req->cmd.cdw10_bits.kv.vsize == sizeof(uint16_t) + sizeof(key));
	CU_ASSERT(req->cmd.cdw12_bits.kv_exec.osize == sizeof(output));

	memcpy(&klp, output, sizeof(klp));
	CU_ASSERT(klp == sizeof(key));
	CU_ASSERT(memcmp(output + sizeof(uint16_t), key, sizeof(key)) == 0);

	ut_qpair_cleanup(&qpair);
}

/* Bounds: 2 + key_len + input_len must fit in output_len. */
static void
test_kv_exec_bounds(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair qpair = {};
	struct spdk_nvme_ns ns = {};
	const uint8_t key[] = {0x01, 0x02, 0x03, 0x04};
	uint8_t input[8];
	uint8_t output[8];
	int rc;

	ns.id = 3;
	ut_qpair_init(&qpair, &ctrlr);
	memset(input, 0, sizeof(input));

	/* 2 + 4 + 8 = 14 > output_len 8 -> reject. */
	g_submitted_req = NULL;
	rc = spdk_nvme_kv_exec(&ns, &qpair, key, sizeof(key), 0,
			       input, sizeof(input), output, sizeof(output),
			       NULL, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_submitted_req == NULL);

	/* Exactly fits: 2 + 4 + 2 = 8 == output_len 8 -> accept. */
	g_submitted_req = NULL;
	g_submit_rc = 0;
	rc = spdk_nvme_kv_exec(&ns, &qpair, key, sizeof(key), 0,
			       input, 2, output, sizeof(output), NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_submitted_req != NULL);

	ut_qpair_cleanup(&qpair);
}

/*
 * A pathological input_len near UINT32_MAX must be rejected outright, NOT wrap
 * the 32-bit payload_len sum (sizeof(u16) + key_len + input_len) into a tiny
 * value that bypasses the bounds check and drives an out-of-bounds memmove.
 */
static void
test_kv_exec_input_len_overflow(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair qpair = {};
	struct spdk_nvme_ns ns = {};
	const uint8_t key[] = {0x01, 0x02, 0x03, 0x04};
	uint8_t output[8];
	int rc;

	ns.id = 5;
	ut_qpair_init(&qpair, &ctrlr);

	/*
	 * input_len = UINT32_MAX with a small output buffer. The naive sum
	 * 2 + 4 + 0xFFFFFFFF wraps to 5, which would pass payload_len > output_len.
	 * The guard rejects on input_len > output_len before any arithmetic, so
	 * no request is staged or submitted.
	 */
	g_submitted_req = NULL;
	rc = spdk_nvme_kv_exec(&ns, &qpair, key, sizeof(key), 0,
			       output, UINT32_MAX, output, sizeof(output),
			       NULL, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_submitted_req == NULL);

	/* input_len just over output_len is also rejected (no wrap involved). */
	g_submitted_req = NULL;
	rc = spdk_nvme_kv_exec(&ns, &qpair, key, sizeof(key), 0,
			       output, sizeof(output) + 1, output, sizeof(output),
			       NULL, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_submitted_req == NULL);

	ut_qpair_cleanup(&qpair);
}

/* Argument validation that does not depend on a submit. */
static void
test_kv_exec_invalid_args(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair qpair = {};
	struct spdk_nvme_ns ns = {};
	const uint8_t key[] = {0x01, 0x02};
	uint8_t output[16];
	int rc;

	ns.id = 4;
	ut_qpair_init(&qpair, &ctrlr);

	/* NULL key. */
	rc = spdk_nvme_kv_exec(&ns, &qpair, NULL, sizeof(key), 0,
			       NULL, 0, output, sizeof(output), NULL, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* NULL output. */
	rc = spdk_nvme_kv_exec(&ns, &qpair, key, sizeof(key), 0,
			       NULL, 0, NULL, sizeof(output), NULL, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* output_len == 0. */
	rc = spdk_nvme_kv_exec(&ns, &qpair, key, sizeof(key), 0,
			       NULL, 0, output, 0, NULL, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* input_len > 0 with NULL input. */
	rc = spdk_nvme_kv_exec(&ns, &qpair, key, sizeof(key), 0,
			       NULL, 4, output, sizeof(output), NULL, NULL);
	CU_ASSERT(rc == -EINVAL);

	ut_qpair_cleanup(&qpair);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("nvme_kv", NULL, NULL);

	CU_ADD_TEST(suite, test_kv_exec_payload_head_encoding);
	CU_ADD_TEST(suite, test_kv_exec_input_aliases_output);
	CU_ADD_TEST(suite, test_kv_exec_zero_input);
	CU_ADD_TEST(suite, test_kv_exec_bounds);
	CU_ADD_TEST(suite, test_kv_exec_input_len_overflow);
	CU_ADD_TEST(suite, test_kv_exec_invalid_args);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
