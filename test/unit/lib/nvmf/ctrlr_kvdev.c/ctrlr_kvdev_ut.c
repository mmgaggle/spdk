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
/*
 * Real (not stubbed) iov<->buf copy: the mixed-SGL test (B-i V3) depends on the
 * RAM head actually being coalesced into the parse buffer, so a no-op stub would
 * leave key_len == 0 and mis-report INVALID_KEY_SIZE.
 */
void
spdk_copy_iovs_to_buf(void *buf, size_t buf_len, struct iovec *iovs, int iovcnt)
{
	uint8_t *dst = buf;
	size_t off = 0;
	int i;

	for (i = 0; i < iovcnt && off < buf_len; i++) {
		size_t n = spdk_min(iovs[i].iov_len, buf_len - off);

		memcpy(dst + off, iovs[i].iov_base, n);
		off += n;
	}
}
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
	bool		read_only;
	bool		binding_present;
	enum spdk_kv_exec_runtime runtime;
	uint32_t	input_len;
	int		rc;
} g_exec;

int
spdk_kvdev_exec(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		const void *key, uint8_t key_len, uint32_t op_id, bool read_only,
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
	g_exec.read_only = read_only;
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
 * B-i V3 (bead spdk-avu, MIXED-SGL): dma-buf result-sink Exec stub. Captures the
 * key/input parsed from the RAM head and the dma-buf sink (fd, offset, len, va) so
 * the mixed-SGL test can assert the dispatch. Mirrors the exec stub's completion.
 */
static struct {
	bool		called;
	uint8_t		key[SPDK_KVDEV_EXEC_KEY_MAX_LEN];
	uint8_t		key_len;
	uint32_t	input_len;
	int		sink_fd;
	uint64_t	sink_offset;
	uint32_t	sink_len;
	uint64_t	sink_va;
	int		rc;
} g_exec_dmabuf;

int
spdk_kvdev_exec_dmabuf(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		       const void *key, uint8_t key_len, uint32_t op_id, bool read_only,
		       const struct spdk_kv_exec_binding *binding,
		       const void *input, uint32_t input_len,
		       int sink_fd, uint64_t sink_offset, uint32_t sink_len,
		       uint64_t sink_va,
		       spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	g_exec_dmabuf.called = true;
	g_exec_dmabuf.key_len = key_len;
	if (key_len > 0) {
		memcpy(g_exec_dmabuf.key, key, key_len);
	}
	g_exec_dmabuf.input_len = input_len;
	g_exec_dmabuf.sink_fd = sink_fd;
	g_exec_dmabuf.sink_offset = sink_offset;
	g_exec_dmabuf.sink_len = sink_len;
	g_exec_dmabuf.sink_va = sink_va;
	if (g_exec_dmabuf.rc == 0) {
		cb_fn(cb_arg, SPDK_KVDEV_IO_STATUS_SUCCESS, 0);
	}
	return g_exec_dmabuf.rc;
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
 * B-i V3 (bead spdk-avu, MIXED-SGL): a fake transport exposing req_get_dmabuf_sink,
 * so a KV Exec request whose DPTR is a [RAM head][dma-buf body] mixed SGL routes to
 * spdk_kvdev_exec_dmabuf. g_dmabuf_sink describes the dma-buf segment the accessor
 * reports; set g_dmabuf_sink.present to enable the dma-buf path.
 */
static struct {
	bool		present;
	int		fd;
	uint64_t	offset;
	uint32_t	len;
	uint8_t		iovidx;
	uint64_t	iova;
} g_dmabuf_sink;

static int
fake_req_get_dmabuf_sink(struct spdk_nvmf_request *req, int *fd, uint64_t *offset,
			 uint32_t *len, uint8_t *iovidx, uint64_t *iova)
{
	if (!g_dmabuf_sink.present) {
		return -ENOENT;
	}
	if (fd) {
		*fd = g_dmabuf_sink.fd;
	}
	if (offset) {
		*offset = g_dmabuf_sink.offset;
	}
	if (len) {
		*len = g_dmabuf_sink.len;
	}
	if (iovidx) {
		*iovidx = g_dmabuf_sink.iovidx;
	}
	if (iova) {
		*iova = g_dmabuf_sink.iova;
	}
	return 0;
}

static struct spdk_nvmf_transport_ops g_fake_transport_ops = {
	.req_get_dmabuf_sink = fake_req_get_dmabuf_sink,
};
static struct spdk_nvmf_transport g_fake_transport = {
	.ops = &g_fake_transport_ops,
};
static struct spdk_nvmf_qpair g_fake_qpair = {
	.transport = &g_fake_transport,
};

/*
 * Build and dispatch a MIXED-SGL KV Exec: RAM head iov(s) carry
 * [u16 key_len][key][input]; a trailing sentinel iov stands in for the dma-buf
 * result_sink segment (its iov_base must never be read). g_dmabuf_sink describes
 * the dma-buf segment (iovidx == head_iovcnt, since it is the last segment).
 */
static enum spdk_nvmf_request_exec_status
run_kv_exec_mixed_sgl(struct spdk_nvmf_ns *ns, const uint8_t *key, uint16_t key_len,
		      const uint8_t *input, uint32_t input_len, uint32_t op_id,
		      uint8_t head_iovcnt, struct spdk_nvme_cpl *rsp_out)
{
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_request req = {};
	static uint8_t head[1024];
	/* A deliberately poisoned "VRAM" sentinel: reading it would be a bug. */
	static uint8_t sink_sentinel[1];
	uint32_t head_len = sizeof(uint16_t) + key_len + input_len;
	enum spdk_nvmf_request_exec_status status;

	SPDK_CU_ASSERT_FATAL(head_len <= sizeof(head));
	SPDK_CU_ASSERT_FATAL(head_iovcnt >= 1 && head_iovcnt + 1 <= NVMF_REQ_MAX_BUFFERS);
	memset(head, 0, sizeof(head));
	memcpy(head, &key_len, sizeof(uint16_t));
	if (key_len > 0) {
		memcpy(head + sizeof(uint16_t), key, key_len);
	}
	if (input_len > 0) {
		memcpy(head + sizeof(uint16_t) + key_len, input, input_len);
	}

	cmd.nvme_cmd.opc = SPDK_NVME_OPC_KV_EXEC;
	cmd.nvme_cmd.cdw10_bits.kv.vsize = head_len;
	cmd.nvme_cmd.cdw12_bits.kv_exec.osize = g_dmabuf_sink.len;
	cmd.nvme_cmd.cdw13_bits.kv_exec.op_id = op_id;

	req.cmd = &cmd;
	req.rsp = &rsp;
	req.qpair = &g_fake_qpair;
	req.length = head_len;
	/* RAM head: split across head_iovcnt iovs (here the first carries everything;
	 * additional head iovs are zero-length, exercising the coalesce loop). */
	req.iov[0].iov_base = head;
	req.iov[0].iov_len = head_len;
	for (uint8_t i = 1; i < head_iovcnt; i++) {
		req.iov[i].iov_base = head + head_len;
		req.iov[i].iov_len = 0;
	}
	/* Trailing dma-buf segment (sentinel VA; never read). */
	req.iov[head_iovcnt].iov_base = sink_sentinel;
	req.iov[head_iovcnt].iov_len = g_dmabuf_sink.len;
	req.iovcnt = head_iovcnt + 1;

	g_dmabuf_sink.iovidx = head_iovcnt;

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
	/* The opcode gate no longer ASSUMES Exec is non-mutating: it forwards the
	 * namespace read-only state to the backend so the invariant is enforced at
	 * the mutation point. (That the backend actually REJECTS a mutating op is
	 * proven against the real kvdev_mem backend in kvdev_mem_ut.) */
	CU_ASSERT(g_exec.read_only == true);
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

/*
 * B-i V3 (bead spdk-avu, MIXED-SGL): a KV Exec whose DPTR is [RAM head][dma-buf
 * body] routes to spdk_kvdev_exec_dmabuf with the key+input parsed from the RAM
 * head and the dma-buf segment (fd, offset, len, iova) as the result_sink.
 */
static void
test_kvdev_exec_mixed_sgl_dmabuf(void)
{
	struct spdk_kvdev *dummy_kvdev = (struct spdk_kvdev *)0x1;
	struct spdk_nvmf_ns ns = { .nsid = 1, .csi = SPDK_NVME_CSI_KV,
		       .kvdev = dummy_kvdev, .kv_read_only = false
	};
	const uint8_t key[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
	const uint8_t input[6] = { 1, 2, 3, 4, 5, 6 };
	struct spdk_nvme_cpl rsp;
	enum spdk_nvmf_request_exec_status status;

	memset(&g_exec, 0, sizeof(g_exec));
	memset(&g_exec_dmabuf, 0, sizeof(g_exec_dmabuf));
	g_allow = true;
	g_dmabuf_sink.present = true;
	g_dmabuf_sink.fd = 42;
	g_dmabuf_sink.offset = 0x1000;
	g_dmabuf_sink.len = 64 * 1024 * 1024;	/* 64 MiB VRAM result_sink */
	g_dmabuf_sink.iova = 0xdeadbeef000ull;

	/* head_iovcnt == 1: one RAM head segment, then the dma-buf segment. */
	status = run_kv_exec_mixed_sgl(&ns, key, sizeof(key), input, sizeof(input),
				       3, 1, &rsp);
	CU_ASSERT(status == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	/* The dma-buf exec op was taken, NOT the VA exec op. */
	CU_ASSERT(g_exec_dmabuf.called == true);
	CU_ASSERT(g_exec.called == false);
	/* Key+input parsed from the RAM head. */
	CU_ASSERT(g_exec_dmabuf.key_len == sizeof(key));
	CU_ASSERT(memcmp(g_exec_dmabuf.key, key, sizeof(key)) == 0);
	CU_ASSERT(g_exec_dmabuf.input_len == sizeof(input));
	/* dma-buf result_sink threaded through verbatim, output_len == sink_len. */
	CU_ASSERT(g_exec_dmabuf.sink_fd == 42);
	CU_ASSERT(g_exec_dmabuf.sink_offset == 0x1000);
	CU_ASSERT(g_exec_dmabuf.sink_len == 64 * 1024 * 1024);
	CU_ASSERT(g_exec_dmabuf.sink_va == 0xdeadbeef000ull);

	g_dmabuf_sink.present = false;
}

/*
 * Mixed-SGL shape guard: the dma-buf segment MUST be the last segment with >= 1
 * preceding RAM head segment. An all-VRAM shape (no RAM head, iovidx == 0) is
 * rejected cleanly (no key to parse), never a crash on the sentinel VA.
 */
static void
test_kvdev_exec_dmabuf_no_ram_head_rejected(void)
{
	struct spdk_kvdev *dummy_kvdev = (struct spdk_kvdev *)0x1;
	struct spdk_nvmf_ns ns = { .nsid = 1, .csi = SPDK_NVME_CSI_KV,
		       .kvdev = dummy_kvdev, .kv_read_only = false
	};
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_request req = {};
	static uint8_t sink_sentinel[1];
	enum spdk_nvmf_request_exec_status status;

	memset(&g_exec, 0, sizeof(g_exec));
	memset(&g_exec_dmabuf, 0, sizeof(g_exec_dmabuf));
	g_allow = true;
	g_dmabuf_sink.present = true;
	g_dmabuf_sink.fd = 7;
	g_dmabuf_sink.len = 4096;
	g_dmabuf_sink.iovidx = 0;	/* dma-buf is the ONLY (first) segment: no RAM head */

	cmd.nvme_cmd.opc = SPDK_NVME_OPC_KV_EXEC;
	cmd.nvme_cmd.cdw10_bits.kv.vsize = 16;
	cmd.nvme_cmd.cdw12_bits.kv_exec.osize = 4096;
	cmd.nvme_cmd.cdw13_bits.kv_exec.op_id = 1;
	req.cmd = &cmd;
	req.rsp = &rsp;
	req.qpair = &g_fake_qpair;
	req.length = 4096;
	req.iov[0].iov_base = sink_sentinel;	/* the sole (dma-buf) segment */
	req.iov[0].iov_len = 4096;
	req.iovcnt = 1;

	status = nvmf_kvdev_ctrlr_process_io_cmd(&ns, NULL, &req);
	CU_ASSERT(status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID);
	/* Neither exec op ran: rejected at the shape guard, sentinel never read. */
	CU_ASSERT(g_exec.called == false);
	CU_ASSERT(g_exec_dmabuf.called == false);

	g_dmabuf_sink.present = false;
}

/*
 * R2 (bead spdk-avu, silent-data-loss guard): a KV Exec dma-buf result_sink
 * whose declared length is <= the inline cap (SPDK_KVDEV_DMABUF_SINK_MIN_LEN)
 * would ride the result inline and be dropped on the dma-buf path (no host_out),
 * yet still complete SUCCESS. It MUST be rejected with a clean NVMe status before
 * dispatch -- neither exec op may run. Shape is valid (1 RAM head + trailing
 * dma-buf) so the reject is the length check, not the shape guard.
 */
static void
test_kvdev_exec_dmabuf_subinline_rejected(void)
{
	struct spdk_kvdev *dummy_kvdev = (struct spdk_kvdev *)0x1;
	struct spdk_nvmf_ns ns = { .nsid = 1, .csi = SPDK_NVME_CSI_KV,
		       .kvdev = dummy_kvdev, .kv_read_only = false
	};
	const uint8_t key[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
	const uint8_t input[6] = { 1, 2, 3, 4, 5, 6 };
	struct spdk_nvme_cpl rsp;
	enum spdk_nvmf_request_exec_status status;

	memset(&g_exec, 0, sizeof(g_exec));
	memset(&g_exec_dmabuf, 0, sizeof(g_exec_dmabuf));
	g_allow = true;
	g_dmabuf_sink.present = true;
	g_dmabuf_sink.fd = 42;
	g_dmabuf_sink.offset = 0;
	/* At the inline cap exactly: <= bound, so rejected (a usable sink must be
	 * STRICTLY larger to force a bulk registration). */
	g_dmabuf_sink.len = SPDK_KVDEV_DMABUF_SINK_MIN_LEN;
	g_dmabuf_sink.iova = 0xcafe000ull;

	status = run_kv_exec_mixed_sgl(&ns, key, sizeof(key), input, sizeof(input),
				       3, 1, &rsp);
	CU_ASSERT(status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	/* Clean reject, NOT a SUCCESS completion (the silent-data-loss bug). */
	CU_ASSERT(rsp.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.status.sc == SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID);
	/* Neither exec op ran. */
	CU_ASSERT(g_exec.called == false);
	CU_ASSERT(g_exec_dmabuf.called == false);

	/* One byte over the cap is accepted (the dma-buf exec op runs). */
	memset(&g_exec_dmabuf, 0, sizeof(g_exec_dmabuf));
	g_dmabuf_sink.len = SPDK_KVDEV_DMABUF_SINK_MIN_LEN + 1;
	status = run_kv_exec_mixed_sgl(&ns, key, sizeof(key), input, sizeof(input),
				       3, 1, &rsp);
	CU_ASSERT(status == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(g_exec_dmabuf.called == true);
	CU_ASSERT(g_exec_dmabuf.sink_len == SPDK_KVDEV_DMABUF_SINK_MIN_LEN + 1);

	g_dmabuf_sink.present = false;
}

/*
 * Multi-segment RAM head (head_iovcnt >= 2): exercise the
 * nvmf_kvdev_gather_ram_head coalesce loop with REAL data split across two
 * non-zero head iovs (the [u16 key_len][key] in iov[0], the input in iov[1]),
 * then a trailing dma-buf body. The key+input must be reassembled verbatim.
 */
static void
test_kvdev_exec_dmabuf_multi_head_coalesce(void)
{
	struct spdk_kvdev *dummy_kvdev = (struct spdk_kvdev *)0x1;
	struct spdk_nvmf_ns ns = { .nsid = 1, .csi = SPDK_NVME_CSI_KV,
		       .kvdev = dummy_kvdev, .kv_read_only = false
	};
	const uint8_t key[8] = { 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8 };
	const uint8_t input[10] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
	uint16_t key_len = sizeof(key);
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_request req = {};
	static uint8_t seg0[2 + sizeof(key)];	/* [u16 key_len][key] */
	static uint8_t seg1[sizeof(input)];	/* input */
	static uint8_t sink_sentinel[1];
	uint32_t head_len = sizeof(seg0) + sizeof(seg1);
	enum spdk_nvmf_request_exec_status status;

	memset(&g_exec, 0, sizeof(g_exec));
	memset(&g_exec_dmabuf, 0, sizeof(g_exec_dmabuf));
	g_allow = true;
	g_dmabuf_sink.present = true;
	g_dmabuf_sink.fd = 99;
	g_dmabuf_sink.offset = 0x2000;
	g_dmabuf_sink.len = 8 * 1024 * 1024;	/* > inline cap */
	g_dmabuf_sink.iova = 0xfeed000ull;
	g_dmabuf_sink.iovidx = 2;		/* two RAM head segments precede it */

	memcpy(seg0, &key_len, sizeof(uint16_t));
	memcpy(seg0 + sizeof(uint16_t), key, sizeof(key));
	memcpy(seg1, input, sizeof(input));

	cmd.nvme_cmd.opc = SPDK_NVME_OPC_KV_EXEC;
	cmd.nvme_cmd.cdw10_bits.kv.vsize = head_len;
	cmd.nvme_cmd.cdw12_bits.kv_exec.osize = g_dmabuf_sink.len;
	cmd.nvme_cmd.cdw13_bits.kv_exec.op_id = 5;
	req.cmd = &cmd;
	req.rsp = &rsp;
	req.qpair = &g_fake_qpair;
	req.length = head_len;
	/* Two NON-ZERO RAM head segments, then the dma-buf body. */
	req.iov[0].iov_base = seg0;
	req.iov[0].iov_len = sizeof(seg0);
	req.iov[1].iov_base = seg1;
	req.iov[1].iov_len = sizeof(seg1);
	req.iov[2].iov_base = sink_sentinel;
	req.iov[2].iov_len = g_dmabuf_sink.len;
	req.iovcnt = 3;

	status = nvmf_kvdev_ctrlr_process_io_cmd(&ns, NULL, &req);
	CU_ASSERT(status == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(g_exec_dmabuf.called == true);
	CU_ASSERT(g_exec.called == false);
	/* Key+input reassembled verbatim from the two coalesced head segments. */
	CU_ASSERT(g_exec_dmabuf.key_len == sizeof(key));
	CU_ASSERT(memcmp(g_exec_dmabuf.key, key, sizeof(key)) == 0);
	CU_ASSERT(g_exec_dmabuf.input_len == sizeof(input));
	CU_ASSERT(g_exec_dmabuf.sink_fd == 99);
	CU_ASSERT(g_exec_dmabuf.sink_offset == 0x2000);

	g_dmabuf_sink.present = false;
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
	CU_ADD_TEST(suite, test_kvdev_exec_mixed_sgl_dmabuf);
	CU_ADD_TEST(suite, test_kvdev_exec_dmabuf_no_ram_head_rejected);
	CU_ADD_TEST(suite, test_kvdev_exec_dmabuf_subinline_rejected);
	CU_ADD_TEST(suite, test_kvdev_exec_dmabuf_multi_head_coalesce);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
