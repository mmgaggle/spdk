/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "spdk_internal/mock.h"

#include "nvmf/ctrlr_kvdev.c"

SPDK_LOG_REGISTER_COMPONENT(nvmf)

/*
 * Externals referenced by ctrlr_kvdev.c. The enforcement (reject) paths under
 * test return before any of these are reached, but they must resolve at link
 * time. spdk_kvdev_store_opts_init() is a static inline in the header, so it
 * needs no stub.
 */
DEFINE_STUB(spdk_nvmf_request_complete, int, (struct spdk_nvmf_request *req), 0);
DEFINE_STUB(spdk_kvdev_get_caps, const struct spdk_kvdev_caps *,
	    (const struct spdk_kvdev *kvdev), NULL);
DEFINE_STUB(spdk_kvdev_store, int,
	    (struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch, const void *key,
	     uint8_t key_len, const void *value, uint32_t value_len,
	     const struct spdk_kvdev_store_opts *opts, spdk_kvdev_io_completion_cb cb_fn,
	     void *cb_arg), 0);
DEFINE_STUB(spdk_kvdev_retrieve, int,
	    (struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch, const void *key,
	     uint8_t key_len, void *value_buf, uint32_t buf_len,
	     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_kvdev_delete, int,
	    (struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch, const void *key,
	     uint8_t key_len, spdk_kvdev_io_completion_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_kvdev_exist, int,
	    (struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch, const void *key,
	     uint8_t key_len, spdk_kvdev_io_completion_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_kvdev_list, int,
	    (struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch, const void *start_key,
	     uint8_t start_key_len, spdk_kvdev_list_cb iter_cb, void *iter_arg,
	     spdk_kvdev_list_done_cb done_cb, void *done_arg), 0);
DEFINE_STUB_V(spdk_copy_iovs_to_buf, (void *buf, size_t buf_len, struct iovec *iovs, int iovcnt));
DEFINE_STUB_V(spdk_copy_buf_to_iovs, (struct iovec *iovs, int iovcnt, void *buf, size_t buf_len));

/*
 * spdk_kvdev_exec capture stub. The Exec datapath (ADR-0014) parses the
 * data-object key out of the payload head and the structured binding out of the
 * allowlist before calling this; we capture both so the test can assert them.
 */
static struct {
	bool		called;
	uint8_t		key[SPDK_KVDEV_EXEC_KEY_MAX_LEN];
	uint8_t		key_len;
	uint32_t	op_id;
	bool		binding_present;
	enum spdk_kv_exec_runtime runtime;
	uint32_t	input_len;
	int		rc;
} g_exec;

int
spdk_kvdev_exec(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		const void *key, uint8_t key_len, uint32_t op_id,
		const struct spdk_kv_exec_binding *binding,
		const void *input, uint32_t input_len,
		void *output_buf, uint32_t output_buf_len,
		spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	g_exec.called = true;
	g_exec.key_len = key_len;
	if (key_len > 0) {
		memcpy(g_exec.key, key, key_len);
	}
	g_exec.op_id = op_id;
	g_exec.binding_present = binding != NULL;
	g_exec.runtime = binding ? binding->runtime : SPDK_KV_EXEC_RUNTIME_NONE;
	g_exec.input_len = input_len;
	/* Mirror a real backend: when the request is accepted (rc == 0), fire the
	 * completion, which frees the per-request state (kv_req). The completion
	 * scatters output back via the stubbed spdk_copy_buf_to_iovs. */
	if (g_exec.rc == 0) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_SUCCESS, 0);
	}
	return g_exec.rc;
}

/*
 * nvmf_ns_kv_exec_op_allowed stub. By default it allows any op-ID with a wasm
 * binding; a test can flip g_allow to exercise the deny path.
 */
static bool g_allow = true;

bool
nvmf_ns_kv_exec_op_allowed(const struct spdk_nvmf_ns *ns, uint32_t op_id,
			   struct spdk_kv_exec_binding *binding_out, bool *has_binding)
{
	if (!g_allow) {
		return false;
	}
	if (has_binding != NULL) {
		*has_binding = true;
	}
	if (binding_out != NULL) {
		memset(binding_out, 0, sizeof(*binding_out));
		binding_out->runtime = SPDK_KV_EXEC_RUNTIME_WASM;
		binding_out->module_key = "bytecount";
	}
	return true;
}

/* Run one (non-Exec) KV command through the dispatcher and return the exec status. */
static enum spdk_nvmf_request_exec_status
run_kv_cmd(struct spdk_nvmf_ns *ns, uint8_t opc, uint8_t key_len, uint32_t vsize,
	   uint32_t req_len, struct spdk_nvme_cpl *rsp_out)
{
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_request req = {};
	enum spdk_nvmf_request_exec_status status;

	cmd.nvme_cmd.opc = opc;
	cmd.nvme_cmd.cdw11_bits.kv.kl = key_len;
	cmd.nvme_cmd.cdw10_bits.kv.vsize = vsize;
	/* Provide some key bytes so a valid key_len decodes to a real key. */
	cmd.nvme_cmd.cdw2 = 0x04030201;
	cmd.nvme_cmd.cdw3 = 0x08070605;

	req.cmd = &cmd;
	req.rsp = &rsp;
	req.length = req_len;

	status = nvmf_kvdev_ctrlr_process_io_cmd(ns, NULL, &req);
	*rsp_out = rsp.nvme_cpl;
	return status;
}

/*
 * Run a KV Exec through the dispatcher with a payload-head key (ADR-0014):
 * [u16 key_len][key bytes][input]. payload_len is CDW10 (total request length);
 * output_size is CDW12. The single iov backs the bidirectional buffer.
 */
static enum spdk_nvmf_request_exec_status
run_kv_exec(struct spdk_nvmf_ns *ns, const uint8_t *key, uint16_t key_len,
	    const uint8_t *input, uint32_t input_len, uint32_t output_size,
	    uint32_t op_id, struct spdk_nvme_cpl *rsp_out)
{
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_request req = {};
	static uint8_t buf[1024];
	uint32_t payload_len = sizeof(uint16_t) + key_len + input_len;
	enum spdk_nvmf_request_exec_status status;

	SPDK_CU_ASSERT_FATAL(payload_len <= sizeof(buf));
	memset(buf, 0, sizeof(buf));
	memcpy(buf, &key_len, sizeof(uint16_t));
	if (key_len > 0) {
		memcpy(buf + sizeof(uint16_t), key, key_len);
	}
	if (input_len > 0) {
		memcpy(buf + sizeof(uint16_t) + key_len, input, input_len);
	}

	cmd.nvme_cmd.opc = SPDK_NVME_OPC_KV_EXEC;
	cmd.nvme_cmd.cdw10_bits.kv.vsize = payload_len;
	cmd.nvme_cmd.cdw12_bits.kv_exec.osize = output_size;
	cmd.nvme_cmd.cdw13_bits.kv_exec.op_id = op_id;

	req.cmd = &cmd;
	req.rsp = &rsp;
	req.length = sizeof(buf);
	req.iov[0].iov_base = buf;
	req.iov[0].iov_len = sizeof(buf);
	req.iovcnt = 1;

	status = nvmf_kvdev_ctrlr_process_io_cmd(ns, NULL, &req);
	*rsp_out = rsp.nvme_cpl;
	return status;
}

/*
 * ADR-0008: a read-only KV namespace rejects mutating opcodes (Store, Delete)
 * deny-by-default, but ADR-0014 PERMITS KV Exec (it is read-only).
 */
static void
test_kvdev_read_only_rejects_writes(void)
{
	struct spdk_kvdev *dummy_kvdev = (struct spdk_kvdev *)0x1;
	struct spdk_nvmf_ns ns = { .nsid = 1, .csi = SPDK_NVME_CSI_KV,
		       .kvdev = dummy_kvdev, .kv_read_only = true
	};
	const uint8_t writes[] = {
		SPDK_NVME_OPC_KV_STORE,
		SPDK_NVME_OPC_KV_DELETE,
	};
	struct spdk_nvme_cpl rsp;
	enum spdk_nvmf_request_exec_status status;
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(writes); i++) {
		status = run_kv_cmd(&ns, writes[i], 4, 0, 0, &rsp);
		CU_ASSERT(status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
		CU_ASSERT(rsp.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
		CU_ASSERT(rsp.status.sc == SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE);
	}
}

/* ADR-0014 delta 2: KV Exec is permitted on a read-only namespace (it never
 * mutates the stored value), still gated by the allowlist. */
static void
test_kvdev_read_only_permits_exec(void)
{
	struct spdk_kvdev *dummy_kvdev = (struct spdk_kvdev *)0x1;
	struct spdk_nvmf_ns ns = { .nsid = 1, .csi = SPDK_NVME_CSI_KV,
		       .kvdev = dummy_kvdev, .kv_read_only = true
	};
	const uint8_t key[4] = { 0xaa, 0xbb, 0xcc, 0xdd };
	struct spdk_nvme_cpl rsp;
	enum spdk_nvmf_request_exec_status status;

	memset(&g_exec, 0, sizeof(g_exec));
	g_allow = true;

	status = run_kv_exec(&ns, key, sizeof(key), NULL, 0, 64, 1, &rsp);
	/* Not rejected with write-to-RO; the backend exec stub was reached. */
	CU_ASSERT(rsp.status.sc != SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE);
	CU_ASSERT(status == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(g_exec.called == true);
}

/* Key length must be 1..16 bytes for non-Exec ops; otherwise INVALID_KEY_SIZE. */
static void
test_kvdev_key_size_validation(void)
{
	struct spdk_kvdev *dummy_kvdev = (struct spdk_kvdev *)0x1;
	struct spdk_nvmf_ns ns = { .nsid = 1, .csi = SPDK_NVME_CSI_KV,
		       .kvdev = dummy_kvdev, .kv_read_only = false
	};
	struct spdk_nvme_cpl rsp;
	enum spdk_nvmf_request_exec_status status;

	/* key_len == 0 on a Store is rejected. */
	status = run_kv_cmd(&ns, SPDK_NVME_OPC_KV_STORE, 0, 0, 0, &rsp);
	CU_ASSERT(status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.status.sc == SPDK_NVME_SC_INVALID_KEY_SIZE);

	/* key_len > SPDK_KVDEV_KEY_MAX_LEN (16) is rejected. */
	status = run_kv_cmd(&ns, SPDK_NVME_OPC_KV_STORE, 17, 0, 0, &rsp);
	CU_ASSERT(status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.status.sc == SPDK_NVME_SC_INVALID_KEY_SIZE);
}

/* ADR-0014 delta 1: the Exec data-object key rides length-prefixed in the
 * payload and may exceed the 16-byte inline cap (here 32 bytes, a content hash).
 * The dispatcher must pass that full key (and the remaining input) to the
 * backend, NOT decode it from the inline CDW slots. */
static void
test_kvdev_exec_key_in_payload(void)
{
	struct spdk_kvdev *dummy_kvdev = (struct spdk_kvdev *)0x1;
	struct spdk_nvmf_ns ns = { .nsid = 1, .csi = SPDK_NVME_CSI_KV,
		       .kvdev = dummy_kvdev, .kv_read_only = false
	};
	uint8_t key[32];
	const uint8_t input[5] = { 1, 2, 3, 4, 5 };
	struct spdk_nvme_cpl rsp;
	enum spdk_nvmf_request_exec_status status;
	uint32_t i;

	for (i = 0; i < sizeof(key); i++) {
		key[i] = (uint8_t)(0x40 + i);
	}

	memset(&g_exec, 0, sizeof(g_exec));
	g_allow = true;

	status = run_kv_exec(&ns, key, sizeof(key), input, sizeof(input), 128, 7, &rsp);
	CU_ASSERT(status == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	SPDK_CU_ASSERT_FATAL(g_exec.called == true);
	/* The full 32-byte payload-head key reached the backend verbatim. */
	CU_ASSERT(g_exec.key_len == sizeof(key));
	CU_ASSERT(memcmp(g_exec.key, key, sizeof(key)) == 0);
	/* The bytes after the key header are the input. */
	CU_ASSERT(g_exec.input_len == sizeof(input));
	CU_ASSERT(g_exec.op_id == 7);
	/* The structured binding was threaded through (delta 3). */
	CU_ASSERT(g_exec.binding_present == true);
	CU_ASSERT(g_exec.runtime == SPDK_KV_EXEC_RUNTIME_WASM);
}

/* A malformed payload-head key (declared length overruns the payload) is
 * rejected INVALID_KEY_SIZE before the backend runs. */
static void
test_kvdev_exec_bad_key_len(void)
{
	struct spdk_kvdev *dummy_kvdev = (struct spdk_kvdev *)0x1;
	struct spdk_nvmf_ns ns = { .nsid = 1, .csi = SPDK_NVME_CSI_KV,
		       .kvdev = dummy_kvdev, .kv_read_only = false
	};
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_request req = {};
	uint8_t buf[64] = {};
	uint16_t klen = 200; /* declared key length far exceeds the payload */
	enum spdk_nvmf_request_exec_status status;

	memset(&g_exec, 0, sizeof(g_exec));
	g_allow = true;

	memcpy(buf, &klen, sizeof(klen));

	cmd.nvme_cmd.opc = SPDK_NVME_OPC_KV_EXEC;
	cmd.nvme_cmd.cdw10_bits.kv.vsize = 10; /* payload only 10 bytes */
	cmd.nvme_cmd.cdw13_bits.kv_exec.op_id = 1;
	req.cmd = &cmd;
	req.rsp = &rsp;
	req.length = sizeof(buf);
	req.iov[0].iov_base = buf;
	req.iov[0].iov_len = sizeof(buf);
	req.iovcnt = 1;

	status = nvmf_kvdev_ctrlr_process_io_cmd(&ns, NULL, &req);
	CU_ASSERT(status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_KEY_SIZE);
	CU_ASSERT(g_exec.called == false);
}

/* An op-ID not in the allowlist is rejected INVALID_OPCODE before dispatch. */
static void
test_kvdev_exec_allowlist_deny(void)
{
	struct spdk_kvdev *dummy_kvdev = (struct spdk_kvdev *)0x1;
	struct spdk_nvmf_ns ns = { .nsid = 1, .csi = SPDK_NVME_CSI_KV,
		       .kvdev = dummy_kvdev, .kv_read_only = false
	};
	const uint8_t key[4] = { 1, 2, 3, 4 };
	struct spdk_nvme_cpl rsp;
	enum spdk_nvmf_request_exec_status status;

	memset(&g_exec, 0, sizeof(g_exec));
	g_allow = false;

	status = run_kv_exec(&ns, key, sizeof(key), NULL, 0, 64, 9, &rsp);
	CU_ASSERT(status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.status.sc == SPDK_NVME_SC_INVALID_OPCODE);
	CU_ASSERT(g_exec.called == false);

	g_allow = true;
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();
	suite = CU_add_suite("nvmf_kvdev", NULL, NULL);

	CU_ADD_TEST(suite, test_kvdev_read_only_rejects_writes);
	CU_ADD_TEST(suite, test_kvdev_read_only_permits_exec);
	CU_ADD_TEST(suite, test_kvdev_key_size_validation);
	CU_ADD_TEST(suite, test_kvdev_exec_key_in_payload);
	CU_ADD_TEST(suite, test_kvdev_exec_bad_key_len);
	CU_ADD_TEST(suite, test_kvdev_exec_allowlist_deny);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
