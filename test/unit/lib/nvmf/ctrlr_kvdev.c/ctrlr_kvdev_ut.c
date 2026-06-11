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
DEFINE_STUB(spdk_kvdev_exec, int,
	    (struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch, const void *key,
	     uint8_t key_len, uint32_t op_id, const char *binding, const void *input,
	     uint32_t input_len, void *output_buf, uint32_t output_buf_len,
	     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(nvmf_ns_kv_exec_op_allowed, bool,
	    (const struct spdk_nvmf_ns *ns, uint32_t op_id, const char **binding), false);
DEFINE_STUB_V(spdk_copy_iovs_to_buf, (void *buf, size_t buf_len, struct iovec *iovs, int iovcnt));
DEFINE_STUB_V(spdk_copy_buf_to_iovs, (struct iovec *iovs, int iovcnt, void *buf, size_t buf_len));

/* Run one KV command through the dispatcher and return the exec status. */
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

/* ADR-0008: a read-only KV namespace rejects every mutating opcode (deny-by-default). */
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
		SPDK_NVME_OPC_KV_EXEC,
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

/* Key length must be 1..16 bytes for non-List ops; otherwise INVALID_KEY_SIZE. */
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

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();
	suite = CU_add_suite("nvmf_kvdev", NULL, NULL);

	CU_ADD_TEST(suite, test_kvdev_read_only_rejects_writes);
	CU_ADD_TEST(suite, test_kvdev_key_size_validation);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
