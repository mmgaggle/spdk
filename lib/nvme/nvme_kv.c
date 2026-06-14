/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/nvme_kv.h"
#include "nvme_internal.h"

const struct spdk_nvme_kv_ns_data *
spdk_nvme_kv_ns_get_data(const struct spdk_nvme_ns *ns)
{
	return ns->nsdata_kv;
}

const struct spdk_nvme_kv_ctrlr_data *
spdk_nvme_kv_ctrlr_get_data(const struct spdk_nvme_ctrlr *ctrlr)
{
	return ctrlr->cdata_kv;
}

static inline void
nvme_kv_cmd_set_key(struct spdk_nvme_cmd *cmd, const void *key, uint8_t key_len)
{
	assert(key != NULL && key_len >= SPDK_NVME_KV_KEY_MIN_LEN && key_len <= SPDK_NVME_KV_KEY_MAX_LEN);

	cmd->cdw11_bits.kv.kl = key_len;

	memcpy((uint8_t *)&cmd->cdw2, key, spdk_min(key_len, 8));

	if (key_len > 8) {
		memcpy((uint8_t *)&cmd->cdw14, (const uint8_t *)key + 8,
		       spdk_min(key_len - 8, 8));
	}
}

static int
nvme_kv_cmd_with_data(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		      uint8_t opc, const void *key, uint8_t key_len,
		      void *data, uint32_t data_len, uint8_t options,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	if (key == NULL || key_len < SPDK_NVME_KV_KEY_MIN_LEN || key_len > SPDK_NVME_KV_KEY_MAX_LEN ||
	    data == NULL || data_len == 0) {
		return -EINVAL;
	}

	req = nvme_allocate_request_contig(qpair, data, data_len, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = opc;
	cmd->nsid = ns->id;

	/* CDW10: Value size (Store) or Host buffer size (Retrieve/List) */
	cmd->cdw10_bits.kv.vsize = data_len;

	/* CDW11: Key length and request options */
	cmd->cdw11_bits.kv.ro = options;

	nvme_kv_cmd_set_key(cmd, key, key_len);

	return nvme_qpair_submit_request(qpair, req);
}

static int
nvme_kv_cmd_without_data(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			 uint8_t opc, const void *key, uint8_t key_len,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	if (key == NULL || key_len < SPDK_NVME_KV_KEY_MIN_LEN || key_len > SPDK_NVME_KV_KEY_MAX_LEN) {
		return -EINVAL;
	}

	req = nvme_allocate_request_null(qpair, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = opc;
	cmd->nsid = ns->id;

	/* CDW11: Key length */
	nvme_kv_cmd_set_key(cmd, key, key_len);

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_kv_store(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		   const void *key, uint8_t key_len,
		   const void *value, uint32_t value_len,
		   spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		   uint8_t options)
{
	return nvme_kv_cmd_with_data(ns, qpair, SPDK_NVME_OPC_KV_STORE,
				     key, key_len, (void *)value, value_len, options,
				     cb_fn, cb_arg);
}

/*
 * Read the TTL (seconds) from a size-versioned ext-opts struct, honouring its
 * size field so a caller built against an older/newer header is safe. Returns 0
 * (no TTL) when opts is NULL, too small, or carries a zero ttl.
 */
static uint32_t
nvme_kv_store_ext_ttl(const struct spdk_nvme_kv_store_ext_opts *opts)
{
	if (opts == NULL ||
	    opts->size < offsetof(struct spdk_nvme_kv_store_ext_opts, ttl) + sizeof(opts->ttl)) {
		return 0;
	}
	return opts->ttl;
}

int
spdk_nvme_kv_store_ext(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       const void *key, uint8_t key_len,
		       const void *value, uint32_t value_len,
		       spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		       uint8_t options,
		       const struct spdk_nvme_kv_store_ext_opts *opts)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	uint32_t ttl = nvme_kv_store_ext_ttl(opts);

	if (key == NULL || key_len < SPDK_NVME_KV_KEY_MIN_LEN || key_len > SPDK_NVME_KV_KEY_MAX_LEN ||
	    value == NULL || value_len == 0) {
		return -EINVAL;
	}

	req = nvme_allocate_request_contig(qpair, (void *)value, value_len, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_KV_STORE;
	cmd->nsid = ns->id;

	/* CDW10: Value size. */
	cmd->cdw10_bits.kv.vsize = value_len;

	/*
	 * CDW11: key length + request options. Set the TTL Valid Store Option
	 * bit and CDW12 = TTL (vendor extension, ADR-0003) only when a non-zero
	 * TTL was requested, so this stays backward compatible with the plain
	 * store path.
	 */
	cmd->cdw11_bits.kv.ro = options;
	if (ttl != 0) {
		cmd->cdw11_bits.kv.ro |= SPDK_NVME_KV_STORE_OPT_TTL_VALID;
		cmd->cdw12_bits.kv_store.ttl = ttl;
	}

	nvme_kv_cmd_set_key(cmd, key, key_len);

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_kv_retrieve(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		      const void *key, uint8_t key_len,
		      void *value, uint32_t value_len,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		      uint8_t options)
{
	return nvme_kv_cmd_with_data(ns, qpair, SPDK_NVME_OPC_KV_RETRIEVE,
				     key, key_len, value, value_len, options,
				     cb_fn, cb_arg);
}

int
spdk_nvme_kv_delete(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		    const void *key, uint8_t key_len,
		    spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_kv_cmd_without_data(ns, qpair, SPDK_NVME_OPC_KV_DELETE,
					key, key_len, cb_fn, cb_arg);
}

int
spdk_nvme_kv_exist(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		   const void *key, uint8_t key_len,
		   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_kv_cmd_without_data(ns, qpair, SPDK_NVME_OPC_KV_EXIST,
					key, key_len, cb_fn, cb_arg);
}

int
spdk_nvme_kv_exec(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		  const void *key, uint8_t key_len, uint32_t op_id,
		  const void *input, uint32_t input_len,
		  void *output, uint32_t output_len,
		  spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	uint16_t klp = key_len;
	uint32_t payload_len;
	uint8_t *buf;

	if (key == NULL || key_len < SPDK_NVME_KV_KEY_MIN_LEN || key_len > SPDK_NVME_KV_KEY_MAX_LEN ||
	    output == NULL || output_len == 0) {
		return -EINVAL;
	}

	if (input == NULL && input_len > 0) {
		/* An input length with no input buffer would stage uninitialized bytes. */
		return -EINVAL;
	}

	if (input_len > output_len) {
		/*
		 * The input alone cannot exceed the single (output) buffer that must
		 * hold the whole staged request [u16 key_len][key][input]. Reject here,
		 * before the payload_len arithmetic below, so a pathological input_len
		 * near UINT32_MAX cannot wrap the 32-bit sum and bypass the bounds check.
		 */
		return -EINVAL;
	}

	/*
	 * KV Exec carries its key length-prefixed at the HEAD of the DPTR request
	 * payload (ADR-0014 Option 1), NOT in the inline CDW slots. The single
	 * data buffer is bidirectional: it gathers the request host->controller,
	 * then the controller scatters the output back into the same buffer from
	 * offset 0. Stage the request as [u16 key_len][key][input] into the output
	 * buffer; the total payload (header + key + input) must fit.
	 */
	payload_len = sizeof(uint16_t) + key_len + input_len;
	if (payload_len > output_len) {
		/* The single data buffer (output) must hold the staged request. */
		return -EINVAL;
	}

	buf = output;

	/*
	 * Place the input at its final offset BEFORE writing the header+key, so
	 * that an in-place caller (input aliasing output, where the input bytes
	 * currently sit at the buffer head) is not clobbered. memmove tolerates
	 * the overlap; a distinct input buffer just copies.
	 */
	if (input_len > 0) {
		if (input == output) {
			memmove(buf + sizeof(uint16_t) + key_len, buf, input_len);
		} else {
			memcpy(buf + sizeof(uint16_t) + key_len, input, input_len);
		}
	}

	/*
	 * Write the 2-byte key-length header the same way the target reads it
	 * (memcpy(&klp, data, 2) -> native byte order on this host), then the key.
	 */
	memcpy(buf, &klp, sizeof(klp));
	memcpy(buf + sizeof(uint16_t), key, key_len);

	req = nvme_allocate_request_contig(qpair, output, output_len, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_KV_EXEC;
	cmd->nsid = ns->id;

	/*
	 * CDW10: TOTAL request payload length ([u16 key_len][key][input]).
	 * CDW12: output buffer size (scatter-back bound). CDW13: operation ID.
	 * The key rides the payload head, so it does NOT go in the inline CDW
	 * slots (no nvme_kv_cmd_set_key here).
	 */
	cmd->cdw10_bits.kv.vsize = payload_len;
	cmd->cdw12_bits.kv_exec.osize = output_len;
	cmd->cdw13_bits.kv_exec.op_id = op_id;

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_kv_list(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		  const void *start_key, uint8_t start_key_len,
		  void *buffer, uint32_t buffer_len,
		  spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	const void *key;
	uint8_t key_len;

	if (buffer == NULL || buffer_len == 0) {
		return -EINVAL;
	}

	if (start_key == NULL) {
		if (start_key_len != 0) {
			return -EINVAL;
		}
		key = NULL;
		key_len = 0;
	} else {
		if (start_key_len < SPDK_NVME_KV_KEY_MIN_LEN || start_key_len > SPDK_NVME_KV_KEY_MAX_LEN) {
			return -EINVAL;
		}
		key = start_key;
		key_len = start_key_len;
	}

	req = nvme_allocate_request_contig(qpair, buffer, buffer_len, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_KV_LIST;
	cmd->nsid = ns->id;

	/* CDW10: Host buffer size */
	cmd->cdw10_bits.kv.vsize = buffer_len;

	if (key != NULL) {
		nvme_kv_cmd_set_key(cmd, key, key_len);
	}

	return nvme_qpair_submit_request(qpair, req);
}
