/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/kvdev.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvmf_transport.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/util.h"

#include "nvmf_internal.h"

/*
 * Key-Value command set dispatch for NVMf.
 *
 * This is the KV analogue of ctrlr_bdev.c: it decodes the NVMe KV Store and
 * Retrieve commands (the key encoding mirrors lib/nvme/nvme_kv.c's
 * nvme_kv_cmd_set_key()) and drives the bound kvdev.
 *
 * Key encoding in the SQE (per the KV Command Set Specification):
 *   - Key length: CDW11 bits 7:0
 *   - Key bytes [7:0]:   CDW2  (bits 31:0), CDW3  (bits 63:32)
 *   - Key bytes [15:8]:  CDW14 (bits 95:64), CDW15 (bits 127:96)
 *   - Value/host buffer size: CDW10
 */

struct nvmf_kvdev_request {
	struct spdk_nvmf_request	*req;
	/* Bounce buffer used when the request payload spans multiple iovs. */
	void				*bounce;
	/* Host buffer size from CDW10 (bytes the host actually offered). The
	 * bounce buffer is only valid up to this many bytes. */
	uint32_t			xfer_len;

	/* List-only: where the return data structure is being assembled (either
	 * the single request iov or the bounce buffer) and how it is filling up. */
	uint8_t				*list_buf;
	/* Bytes of list_buf already consumed by the NRK header + emitted keys. */
	uint32_t			list_off;
	/* Total bytes available in list_buf (== host buffer size, CDW10). */
	uint32_t			list_cap;
};

/* List return data structure layout (KV spec §3.2.2.2, Figures 15/16):
 *   uint32_t Number of Returned Keys (NRK)
 *   repeated { uint16_t Key Length; key bytes; pad to 4-byte boundary }
 */
#define NVMF_KV_LIST_NRK_SIZE 4

/* Bytes occupied by one key entry (2-byte KL + key + pad to 4-byte boundary). */
static inline uint32_t
nvmf_kv_list_entry_size(uint8_t key_len)
{
	return SPDK_ALIGN_CEIL(sizeof(uint16_t) + key_len, 4);
}

void
nvmf_kvdev_ctrlr_identify_ns(struct spdk_nvmf_ns *ns, struct spdk_nvme_kv_ns_data *nsdata)
{
	const struct spdk_kvdev_caps *caps = spdk_kvdev_get_caps(ns->kvdev);

	/* One KV format (format index 0). nkvf is 0's based. */
	nsdata->nkvf = 0;
	nsdata->kvfc.kvfi = 0;

	nsdata->kvf[0].kvkml = caps->max_key_len;
	nsdata->kvf[0].kvvml = caps->max_value_len;
	nsdata->kvf[0].mnks = caps->max_num_keys;

	/* nsze/nuse are reported in bytes for KV namespaces. We do not track a
	 * fixed capacity for the in-memory backend, so advertise the max value
	 * length as a nominal namespace size. TODO: surface real capacity once
	 * a kvdev exposes it. */
	nsdata->nsze = caps->max_value_len;
	nsdata->nuse = 0;

	/* Advertise the vendor TTL extension (ADR-0003) so a host can detect
	 * that KV Store honours the TTL Valid Store Option + CDW12 TTL. The TTL
	 * is store-only (persisted, never enforced). */
	nsdata->vs_cap |= SPDK_NVME_KV_NS_VS_CAP_TTL;
}

void
nvmf_kvdev_ctrlr_identify_ctrlr(struct spdk_nvmf_ctrlr *ctrlr,
				struct spdk_nvme_kv_ctrlr_data *cdata)
{
	cdata->ver = SPDK_NVME_KV_SPEC_VER;
}

static void
nvmf_kvdev_decode_key(const struct spdk_nvme_cmd *cmd, uint8_t *key, uint8_t *key_len)
{
	uint8_t len = cmd->cdw11_bits.kv.kl;

	*key_len = len;
	if (len == 0) {
		return;
	}

	/* Low 64 bits live in CDW2/CDW3; high 64 bits in CDW14/CDW15. */
	memcpy(key, (const uint8_t *)&cmd->cdw2, spdk_min(len, 8));
	if (len > 8) {
		memcpy(key + 8, (const uint8_t *)&cmd->cdw14, spdk_min(len - 8, 8));
	}
}

static void
nvmf_kvdev_complete(struct nvmf_kvdev_request *kv_req, int kvstatus, uint32_t value_len)
{
	struct spdk_nvmf_request *req = kv_req->req;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	rsp->status.sct = SPDK_NVME_SCT_GENERIC;

	switch (kvstatus) {
	case SPDK_KVDEV_IO_STATUS_SUCCESS:
		rsp->status.sc = SPDK_NVME_SC_SUCCESS;
		/* For Retrieve the value length is returned in CQE DW0. */
		rsp->cdw0 = value_len;
		break;
	case SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST:
		rsp->status.sc = SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST;
		break;
	case SPDK_KVDEV_IO_STATUS_KEY_EXIST:
		rsp->status.sc = SPDK_NVME_SC_KEY_EXISTS;
		break;
	case SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL:
		/* Spec: device returns requested portion and reports the full
		 * value length in the CQE; the command itself succeeds. */
		rsp->status.sc = SPDK_NVME_SC_SUCCESS;
		rsp->cdw0 = value_len;
		break;
	case SPDK_KVDEV_IO_STATUS_INVALID:
		rsp->status.sc = SPDK_NVME_SC_INVALID_VALUE_SIZE;
		break;
	case SPDK_KVDEV_IO_STATUS_NOMEM:
		rsp->status.sc = SPDK_NVME_SC_CAPACITY_EXCEEDED;
		break;
	case SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED:
		/* Backend does not support this op (e.g. librados List, ADR-0002):
		 * report NVMe command-not-supported (Invalid Command Opcode). */
		rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		break;
	case SPDK_KVDEV_IO_STATUS_ABORTED:
		/* A KV Exec module hit a per-invocation resource cap (fuel/epoch/
		 * memory) and was contained (TB2). Report NVMe "command aborted";
		 * the target stays up and other traffic is unaffected. */
		rsp->status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
		break;
	case SPDK_KVDEV_IO_STATUS_READ_ONLY:
		/* A mutating Exec path was rejected because the namespace is
		 * read-only (the invariant is enforced at the mutation point,
		 * ADR-0014). Report the same Command-Specific "Attempted Write to
		 * Read Only Range" status the opcode gate uses for write ops. */
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE;
		break;
	case SPDK_KVDEV_IO_STATUS_FAILED:
	default:
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		break;
	}

	/*
	 * Slice C6c: this command is terminating; drop the abort back-pointer so a
	 * racing ABORT cannot hand a now-freed kv_req to the kvdev backend. (Both run
	 * on the same thread, so once this completion runs no abort can still target
	 * this kv_req.)
	 */
	req->kvdev_io_ctx = NULL;

	free(kv_req->bounce);
	free(kv_req);
	spdk_nvmf_request_complete(req);
}

/*
 * Slice C6c (bead spdk-v3w): tenant NVMe ABORT against a Key-Value namespace.
 * \p req_to_abort is the in-flight KV command the host asked to abort; \p ch is
 * its namespace io_channel. If it is an in-flight KV Exec (the only abortable KV
 * op) we ask the kvdev backend to cancel it via spdk_kvdev_abort, keyed by the
 * opaque kvdev cb_arg we stashed on the request at submit. The original Exec's
 * own completion (nvmf_kvdev_exec_done) still fires exactly once — with ABORTED
 * mapped to NVMe "command aborted" — so this routine MUST NOT complete it.
 *
 * \return true if the backend accepted the cancel request (the host's ABORT
 *         should report "command aborted"), false if there was nothing to abort
 *         (already completed / not an abortable KV op / backend has no abort op),
 *         in which case the caller reports "command not aborted".
 */
bool
nvmf_kvdev_ctrlr_abort_cmd(struct spdk_nvmf_ns *ns, struct spdk_io_channel *ch,
			   struct spdk_nvmf_request *req_to_abort)
{
	void *kv_io_ctx = req_to_abort->kvdev_io_ctx;
	int rc;

	if (kv_io_ctx == NULL) {
		/* Not an in-flight abortable KV op (e.g. already completed, or a
		 * short-lived op that never registered an abort ctx). */
		return false;
	}

	rc = spdk_kvdev_abort(ns->kvdev_desc, ch, kv_io_ctx);
	/* 0: backend entered cancel (its completion fires ABORTED later). -ENOTSUP:
	 * backend cannot abort. -ENOENT: it already completed (the cb_arg is stale).
	 * Any nonzero -> "command not aborted". */
	return rc == 0;
}

static void
nvmf_kvdev_store_done(void *cb_arg, int status, uint32_t value_len)
{
	nvmf_kvdev_complete(cb_arg, status, value_len);
}

/*
 * Completion for the no-data KV ops (Delete, Exist): there is no payload to
 * scatter back, so just translate the status. value_len is unused.
 */
static void
nvmf_kvdev_simple_done(void *cb_arg, int status, uint32_t value_len)
{
	nvmf_kvdev_complete(cb_arg, status, value_len);
}

static void
nvmf_kvdev_retrieve_done(void *cb_arg, int status, uint32_t value_len)
{
	struct nvmf_kvdev_request *kv_req = cb_arg;
	struct spdk_nvmf_request *req = kv_req->req;

	/* If the payload spanned multiple iovs we retrieved into a bounce
	 * buffer; scatter it back out to the request iovs. Only copy bytes the
	 * kvdev actually wrote (bounded by the host buffer size from CDW10), not
	 * req->length, so a host that advertises an SGL larger than CDW10 cannot
	 * read back uninitialized bounce-buffer bytes. */
	if (kv_req->bounce != NULL &&
	    (status == SPDK_KVDEV_IO_STATUS_SUCCESS ||
	     status == SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL)) {
		spdk_copy_buf_to_iovs(req->iov, req->iovcnt, kv_req->bounce,
				      spdk_min(value_len, kv_req->xfer_len));
	}

	nvmf_kvdev_complete(kv_req, status, value_len);
}

/*
 * Completion for KV Exec (ADR-0005). Like Retrieve, the device wrote its output
 * into the contiguous buffer (a bounce buffer when the payload is multi-iov);
 * scatter it back out to the request iovs, bounded by the bytes the kvdev
 * actually produced (value_len) and the host output buffer (xfer_len).
 */
static void
nvmf_kvdev_exec_done(void *cb_arg, int status, uint32_t value_len)
{
	struct nvmf_kvdev_request *kv_req = cb_arg;
	struct spdk_nvmf_request *req = kv_req->req;

	if (kv_req->bounce != NULL &&
	    (status == SPDK_KVDEV_IO_STATUS_SUCCESS ||
	     status == SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL)) {
		spdk_copy_buf_to_iovs(req->iov, req->iovcnt, kv_req->bounce,
				      spdk_min(value_len, kv_req->xfer_len));
	}

	nvmf_kvdev_complete(kv_req, status, value_len);
}

/*
 * Per-key callback for List. Appends one { KL, key, pad } entry to the return
 * data structure being assembled in kv_req->list_buf, immediately after the
 * 4-byte NRK header. Returns false (stop) as soon as the next whole key would
 * not fit in the host buffer, so the structure only ever contains complete
 * keys (KV spec: number returned = keys that completely fit).
 */
static bool
nvmf_kvdev_list_iter(void *cb_arg, const void *key, uint8_t key_len)
{
	struct nvmf_kvdev_request *kv_req = cb_arg;
	uint32_t entry_size = nvmf_kv_list_entry_size(key_len);
	uint8_t *p;
	uint16_t kl16 = key_len;

	if (kv_req->list_off + entry_size > kv_req->list_cap) {
		/* This whole key would not fit; stop without emitting it. */
		return false;
	}

	p = kv_req->list_buf + kv_req->list_off;
	memcpy(p, &kl16, sizeof(kl16));
	memcpy(p + sizeof(kl16), key, key_len);
	/* Zero any pad bytes between the key and the 4-byte boundary. */
	memset(p + sizeof(kl16) + key_len, 0,
	       entry_size - sizeof(kl16) - key_len);

	kv_req->list_off += entry_size;
	return true;
}

static void
nvmf_kvdev_list_done(void *cb_arg, int status, uint32_t num_keys)
{
	struct nvmf_kvdev_request *kv_req = cb_arg;
	struct spdk_nvmf_request *req = kv_req->req;
	uint32_t nrk = num_keys;

	if (status == SPDK_KVDEV_IO_STATUS_SUCCESS) {
		/* Patch the Number of Returned Keys header now that iteration is
		 * done, then scatter the assembled structure back if we built it in
		 * a bounce buffer. */
		memcpy(kv_req->list_buf, &nrk, sizeof(nrk));

		if (kv_req->bounce != NULL) {
			spdk_copy_buf_to_iovs(req->iov, req->iovcnt, kv_req->bounce,
					      kv_req->list_off);
		}
	}

	/* value_len is unused for List (status is mapped to the CQE by
	 * nvmf_kvdev_complete; SUCCESS leaves cdw0 == 0). */
	nvmf_kvdev_complete(kv_req, status, 0);
}

/*
 * Obtain a contiguous data pointer for the request payload. When the request
 * has a single iov we can use it directly; otherwise we allocate a bounce
 * buffer (gathered for store, scattered after retrieve).
 */
static void *
nvmf_kvdev_get_contig_buf(struct spdk_nvmf_request *req, bool gather,
			  struct nvmf_kvdev_request *kv_req)
{
	void *buf;

	if (req->iovcnt == 1) {
		return req->iov[0].iov_base;
	}

	buf = malloc(req->length ? req->length : 1);
	if (buf == NULL) {
		return NULL;
	}

	if (gather) {
		spdk_copy_iovs_to_buf(buf, req->length, req->iov, req->iovcnt);
	}

	kv_req->bounce = buf;
	return buf;
}

/*
 * B-i V3 (bead spdk-avu, MIXED-SGL): gather the CPU-readable HEAD of a KV Exec
 * whose DPTR is a mixed SGL -- leading RAM segment(s) followed by a trailing
 * dma-buf result_sink segment. \p head_iovcnt is the number of leading RAM iovs
 * (== the dma-buf segment's index, since it is the last segment); we coalesce
 * ONLY those into a freshly-allocated CPU buffer and must NOT touch the dma-buf
 * segment (req->iov[head_iovcnt]), whose iov_base is a non-dereferenceable
 * sentinel. The result lands entirely in the dma-buf body, so the head is
 * input-only (never scattered back). Returns the head buffer (owned via
 * kv_req->bounce) and its byte length in \p head_len, or NULL on OOM.
 */
static void *
nvmf_kvdev_gather_ram_head(struct spdk_nvmf_request *req, uint8_t head_iovcnt,
			   struct nvmf_kvdev_request *kv_req, uint32_t *head_len)
{
	uint32_t len = 0;
	uint8_t i;
	void *buf;

	for (i = 0; i < head_iovcnt; i++) {
		len += req->iov[i].iov_len;
	}

	buf = malloc(len ? len : 1);
	if (buf == NULL) {
		return NULL;
	}

	/* Coalesce only the leading RAM head iovs; the dma-buf segment is excluded. */
	spdk_copy_iovs_to_buf(buf, len, req->iov, head_iovcnt);

	kv_req->bounce = buf;
	if (head_len != NULL) {
		*head_len = len;
	}
	return buf;
}

int
nvmf_kvdev_ctrlr_process_io_cmd(struct spdk_nvmf_ns *ns, struct spdk_io_channel *ch,
				struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct nvmf_kvdev_request *kv_req;
	uint8_t key[SPDK_KVDEV_KEY_MAX_LEN];
	uint8_t key_len;
	uint32_t xfer_len;
	void *data;
	int rc;

	/*
	 * KV Exec (vendor opcode 0x83) carries its data-object key length-prefixed
	 * at the HEAD of the DPTR request payload (ADR-0014 Option 1), NOT in the
	 * spec's inline CDW2/3/14/15 slots. So the inline decode + 1..16 validation
	 * below applies only to the canonical-slot ops; the Exec case parses its own
	 * (1..255) key out of the payload after the bounce buffer is gathered.
	 */
	if (cmd->opc != SPDK_NVME_OPC_KV_EXEC) {
		nvmf_kvdev_decode_key(cmd, key, &key_len);
		/* List uses the key as a START POSITION, where length 0 means "from the
		 * beginning"; every other KV command requires a real 1..16 byte key. */
		if (key_len > SPDK_KVDEV_KEY_MAX_LEN ||
		    (key_len < SPDK_KVDEV_KEY_MIN_LEN && cmd->opc != SPDK_NVME_OPC_KV_LIST)) {
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_INVALID_KEY_SIZE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}

	/*
	 * Per-namespace read-only enforcement (ADR-0008 trust split), deny-by-default.
	 * A read-only KV namespace permits ONLY the read/lookup/compute ops that do
	 * not mutate stored values: Retrieve, Exist, List, and KV Exec. KV Exec is
	 * READ-ONLY (ADR-0014/0009/0011): it runs a sandboxed module over the object
	 * but never writes the stored value back, so it is allowed here -- still
	 * gated by the per-namespace op-ID allowlist below. Every mutating opcode --
	 * Store, Delete, and any mutating KV opcode added in the future -- is rejected
	 * here, BEFORE backend dispatch. Using an allow-list (rather than a
	 * reject-list) guarantees a new opcode cannot silently bypass the boundary.
	 * We report Command-Specific status "Attempted Write to Read Only Range"
	 * (SCT 0x1, SC 0x82) -- the same status the NVM command set uses for the
	 * namespace write-protection feature, and the most accurate fit for a
	 * write rejected because the namespace is read-only.
	 */
	if (ns->kv_read_only) {
		switch (cmd->opc) {
		case SPDK_NVME_OPC_KV_RETRIEVE:
		case SPDK_NVME_OPC_KV_EXIST:
		case SPDK_NVME_OPC_KV_LIST:
		case SPDK_NVME_OPC_KV_EXEC:
			break;
		default:
			SPDK_DEBUGLOG(nvmf, "KV opcode 0x%02x rejected on read-only nsid %u\n",
				      cmd->opc, ns->nsid);
			rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			rsp->status.sc = SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}

	kv_req = calloc(1, sizeof(*kv_req));
	if (kv_req == NULL) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	kv_req->req = req;

	/* CDW10 holds the value size (store) or host buffer size (retrieve). */
	xfer_len = cmd->cdw10_bits.kv.vsize;
	if (spdk_unlikely(xfer_len > req->length)) {
		SPDK_ERRLOG("KV transfer length %u exceeds request length %u\n",
			    xfer_len, req->length);
		free(kv_req);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	kv_req->xfer_len = xfer_len;

	switch (cmd->opc) {
	case SPDK_NVME_OPC_KV_STORE: {
		struct spdk_kvdev_store_opts opts;

		spdk_kvdev_store_opts_init(&opts, sizeof(opts));
		/* Store Option bits live in CDW11 Request Options (ro), per the KV
		 * Command Set spec (CDW11 bit 8 == ro bit 0, bit 9 == ro bit 1):
		 *   - "Don't store if key does NOT exist"  => Store-If-Key-Exists (SIKE)
		 *   - "Don't store if key DOES exist"       => Store-If-No-Key-Exists (SINKE)
		 */
		if (cmd->cdw11_bits.kv.ro & SPDK_NVME_KV_STORE_OPT_DONT_STORE_IF_KEY_NOT_EXISTS) {
			opts.flags |= SPDK_KVDEV_STORE_FLAG_SIKE;
		}
		if (cmd->cdw11_bits.kv.ro & SPDK_NVME_KV_STORE_OPT_DONT_STORE_IF_KEY_EXISTS) {
			opts.flags |= SPDK_KVDEV_STORE_FLAG_SINKE;
		}
		/* Vendor TTL extension (ADR-0003): when the TTL Valid Store Option
		 * bit is set, CDW12 carries a TTL in seconds. Pass it through to
		 * the backend, which persists but does not enforce it. */
		if (cmd->cdw11_bits.kv.ro & SPDK_NVME_KV_STORE_OPT_TTL_VALID) {
			opts.flags |= SPDK_KVDEV_STORE_F_TTL;
			opts.ttl = cmd->cdw12_bits.kv_store.ttl;
		}

		data = nvmf_kvdev_get_contig_buf(req, true, kv_req);
		if (data == NULL) {
			goto err_nomem;
		}
		rc = spdk_kvdev_store(ns->kvdev_desc, ch, key, key_len, data, xfer_len,
				      &opts, nvmf_kvdev_store_done, kv_req);
		break;
	}
	case SPDK_NVME_OPC_KV_RETRIEVE:
		data = nvmf_kvdev_get_contig_buf(req, false, kv_req);
		if (data == NULL) {
			goto err_nomem;
		}
		rc = spdk_kvdev_retrieve(ns->kvdev_desc, ch, key, key_len, data, xfer_len,
					 nvmf_kvdev_retrieve_done, kv_req);
		break;
	case SPDK_NVME_OPC_KV_DELETE:
		/* No-data command: no host payload, so bypass the bounce/value path. */
		rc = spdk_kvdev_delete(ns->kvdev_desc, ch, key, key_len,
				       nvmf_kvdev_simple_done, kv_req);
		break;
	case SPDK_NVME_OPC_KV_EXIST:
		/* No-data command: no host payload, so bypass the bounce/value path. */
		rc = spdk_kvdev_exist(ns->kvdev_desc, ch, key, key_len,
				      nvmf_kvdev_simple_done, kv_req);
		break;
	case SPDK_NVME_OPC_KV_EXEC: {
		/*
		 * Vendor KV Exec (ADR-0005 / ADR-0014). CDW10 = TOTAL request payload
		 * length (validated above against req->length as xfer_len), CDW12 =
		 * output buffer size, CDW13 = operation ID. The single DPTR buffer is
		 * bidirectional: it carries the request on the way in and is overwritten
		 * with the response on the way out.
		 *
		 * Request payload layout (ADR-0014 Option 1, key leaves the inline CDW
		 * slots): [u16 key_len (1..255)][key_len key bytes][input bytes ...].
		 */
		uint32_t output_len = cmd->cdw12_bits.kv_exec.osize;
		uint32_t op_id = cmd->cdw13_bits.kv_exec.op_id;
		struct spdk_kv_exec_binding binding;
		struct spdk_kv_exec_binding *binding_arg = NULL;
		bool has_binding = false;
		const uint8_t *exec_key;
		uint8_t exec_key_len;
		const uint8_t *input;
		uint32_t input_len;
		uint16_t klp;
		/*
		 * B-i V3 (bead spdk-avu, MIXED-SGL): dma-buf result_sink (fd>=0 marks
		 * VRAM-direct). The KV Exec DPTR is an SGL with leading RAM head
		 * segment(s) (the [u16 key_len][key][input] payload, CPU-readable) and a
		 * trailing dma-buf segment (the VRAM result_sink). kv_dmabuf_iovidx is the
		 * index of the dma-buf segment within req->iov[]; the RAM head is
		 * req->iov[0 .. kv_dmabuf_iovidx-1].
		 */
		int kv_dmabuf_fd = -1;
		uint64_t kv_dmabuf_off = 0;
		uint32_t kv_dmabuf_len = 0;
		uint8_t kv_dmabuf_iovidx = 0;
		uint64_t kv_dmabuf_iova = 0;
		uint32_t head_len = xfer_len;

		/*
		 * Per-namespace KV Exec allowlist enforcement (ADR-0005). The trust
		 * decision is per (subsystem, nsid): an op-ID not in this namespace's
		 * allowlist is rejected here, BEFORE the backend exec op runs
		 * (default-deny). Reject with INVALID_OPCODE — the same status the data
		 * path already uses for an unsupported/absent KV Exec operation.
		 * On success the allowlist lookup also yields the matching entry's
		 * structured binding (ADR-0010/0012/0014), which we forward to the
		 * backend exec op: the in-memory module ignores it; the librados module
		 * routes on binding->runtime.
		 */
		if (!nvmf_ns_kv_exec_op_allowed(ns, op_id, &binding, &has_binding)) {
			SPDK_DEBUGLOG(nvmf, "KV Exec op_id %u not in nsid %u allowlist; rejecting\n",
				      op_id, ns->nsid);
			free(kv_req);
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
		binding_arg = has_binding ? &binding : NULL;

		/* The output the device may write back is bounded by the host data
		 * buffer; clamp the advertised output size and use it as the
		 * scatter-back bound in nvmf_kvdev_exec_done(). */
		if (output_len > req->length) {
			output_len = req->length;
		}
		kv_req->xfer_len = output_len;

		/*
		 * B-i V3 (bead spdk-avu, MIXED-SGL): detect a dma-buf result_sink
		 * (exported GPU VRAM) BEFORE touching the data buffer. The KV Exec DPTR
		 * is a mixed SGL: leading RAM segment(s) (the CPU-readable [u16
		 * key_len][key][input] HEAD) followed by a trailing dma-buf segment (the
		 * VRAM result_sink, whose iov_base is a non-dereferenceable sentinel and
		 * must NOT be CPU-read). The transport accessor reports the dma-buf
		 * segment's (fd, offset, len, iovidx). The dispatch below routes such a
		 * request to spdk_kvdev_exec_dmabuf(). Non-dma-buf requests leave
		 * kv_dmabuf_fd == -1 and follow the byte-for-byte unchanged VA path.
		 */
		{
			struct spdk_nvmf_transport *tr =
				(req->qpair != NULL) ? req->qpair->transport : NULL;

			if (tr != NULL && tr->ops->req_get_dmabuf_sink != NULL) {
				(void)tr->ops->req_get_dmabuf_sink(req, &kv_dmabuf_fd,
								   &kv_dmabuf_off, &kv_dmabuf_len,
								   &kv_dmabuf_iovidx, &kv_dmabuf_iova);
			}
		}

		/*
		 * B-i V3 MIXED-SGL shape guard. Require exactly the
		 * [RAM head segment(s) ...][dma-buf body] layout:
		 *   - at least one RAM head segment precedes the dma-buf segment
		 *     (kv_dmabuf_iovidx >= 1), so there is a key+input to parse; and
		 *   - the dma-buf segment is the LAST segment
		 *     (kv_dmabuf_iovidx == req->iovcnt - 1).
		 * _map_one() already records only a SINGLE dma-buf sink per request, so a
		 * second dma-buf segment never reaches here. Reject any other shape --
		 * all-VRAM (no RAM head -> no key), a non-trailing dma-buf segment -- with
		 * a clean status, never a crash. The non-dma-buf (all-RAM) path is
		 * untouched (kv_dmabuf_fd < 0).
		 */
		if (kv_dmabuf_fd >= 0 &&
		    (kv_dmabuf_iovidx < 1 || kv_dmabuf_iovidx != req->iovcnt - 1)) {
			SPDK_ERRLOG("KV Exec dma-buf sink: unsupported SGL shape "
				    "(iovidx=%u iovcnt=%u); require [RAM head][dma-buf body]\n",
				    kv_dmabuf_iovidx, req->iovcnt);
			free(kv_req);
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		/*
		 * B-i V3 sub-inline dma-buf sink reject (R2, silent-data-loss guard). A
		 * two-tier backend returns a result that fits the RPC inline cap WITHOUT
		 * registering a bulk MR; the front then has only host_out to copy the
		 * inline bytes into. The dma-buf path has NO host_out (the result must be
		 * RDMA-WRITTEN into device memory via a registered bulk), so a sink whose
		 * declared length is <= the inline cap (SPDK_KVDEV_DMABUF_SINK_MIN_LEN)
		 * would ride inline, be DROPPED by the front, yet still complete SUCCESS --
		 * the guest believes VRAM was written but nothing was. sink_len is
		 * guest-controlled, so reject it here with a clean status BEFORE dispatch
		 * rather than silently lose data. A sub-inline dma-buf sink is unsupported
		 * by design; fail it loudly.
		 */
		if (kv_dmabuf_fd >= 0 && kv_dmabuf_len <= SPDK_KVDEV_DMABUF_SINK_MIN_LEN) {
			SPDK_ERRLOG("KV Exec dma-buf sink length %u <= inline cap %u: "
				    "result would ride inline and be dropped; rejecting\n",
				    kv_dmabuf_len, (uint32_t)SPDK_KVDEV_DMABUF_SINK_MIN_LEN);
			free(kv_req);
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		/*
		 * Obtain a CPU-readable buffer for the key+input parse.
		 *
		 * Non-dma-buf: gather the input into a contiguous buffer (bounce when
		 * multi-iov); the device overwrites it with output, then we scatter it
		 * back -- unchanged behavior. Parsed over the full payload (head_len ==
		 * xfer_len).
		 *
		 * dma-buf result_sink (V3 mixed SGL): coalesce ONLY the leading RAM head
		 * iov(s) (req->iov[0 .. kv_dmabuf_iovidx-1]) into a CPU buffer -- these
		 * are real host VAs carrying [u16 key_len][key][input]. The dma-buf
		 * segment (req->iov[kv_dmabuf_iovidx]) is excluded; its sentinel VA is
		 * never read. The result lands ENTIRELY in the dma-buf body, so the head
		 * is input-only (never scattered back). head_len is the RAM head byte
		 * length, which bounds the key+input parse instead of xfer_len.
		 */
		if (kv_dmabuf_fd >= 0) {
			data = nvmf_kvdev_gather_ram_head(req, kv_dmabuf_iovidx, kv_req,
							  &head_len);
			if (data == NULL) {
				goto err_nomem;
			}
		} else {
			data = nvmf_kvdev_get_contig_buf(req, true, kv_req);
			if (data == NULL) {
				goto err_nomem;
			}
		}

		/*
		 * Parse the payload-head key: [u16 key_len][key][input]. The header
		 * (2 bytes) and the declared key must fit within the parse bound
		 * (head_len, == xfer_len for the all-RAM path; the RAM head byte length
		 * for the mixed-SGL dma-buf path). Anything malformed is INVALID_KEY_SIZE
		 * before dispatch.
		 */
		if (head_len < sizeof(uint16_t)) {
			free(kv_req->bounce);
			free(kv_req);
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_INVALID_KEY_SIZE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
		memcpy(&klp, data, sizeof(klp));
		if (klp < SPDK_KVDEV_KEY_MIN_LEN || klp > SPDK_KVDEV_EXEC_KEY_MAX_LEN ||
		    (uint32_t)sizeof(uint16_t) + klp > head_len) {
			free(kv_req->bounce);
			free(kv_req);
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_INVALID_KEY_SIZE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
		exec_key_len = (uint8_t)klp;
		exec_key = (const uint8_t *)data + sizeof(uint16_t);
		input = exec_key + exec_key_len;
		input_len = head_len - sizeof(uint16_t) - exec_key_len;

		/*
		 * Slice C6c: record the kvdev cb_arg on the request so a later tenant
		 * NVMe ABORT targeting THIS command can ask the backend to cancel the
		 * in-flight Exec (nvmf_kvdev_ctrlr_abort_cmd -> spdk_kvdev_abort). Set
		 * BEFORE submit so a synchronous completion (which clears it) is ordered
		 * correctly. KV Exec is the only long-running KV op that needs this.
		 */
		req->kvdev_io_ctx = kv_req;

		/*
		 * B-i V3 (bead spdk-avu, MIXED-SGL): VRAM-direct result sink. When the
		 * transport resolved this request's DPTR to a [RAM head][dma-buf body]
		 * mixed SGL (kv_dmabuf_fd >= 0 from the detection above), route to the
		 * dma-buf exec op so a two-tier backend RDMA-WRITEs the result straight
		 * into VRAM (nkvx_front_forward_dmabuf) instead of into a host VA. The
		 * key+input were parsed from the RAM head above; the result lands ENTIRELY
		 * in the dma-buf segment, so output_len == the dma-buf segment length
		 * (kv_dmabuf_len). A non-dma-buf sink (kv_dmabuf_fd < 0) falls through to
		 * the unchanged VA path.
		 *
		 * NOTE: kv_req->bounce holds the RAM head copy ONLY (input). The result is
		 * never copied back to a host VA, so nvmf_kvdev_exec_done must NOT scatter
		 * the bounce back into req->iov[] for the dma-buf path -- it would hit the
		 * dma-buf segment's sentinel VA. kv_req->xfer_len is left at 0 below so the
		 * completion's scatter-back is a no-op (the guarded copy only runs when
		 * xfer_len > 0).
		 */
		if (kv_dmabuf_fd >= 0) {
			/* The result is VRAM-only: disable host scatter-back in the
			 * completion (the bounce is the input head, not an output sink). */
			kv_req->xfer_len = 0;
			rc = spdk_kvdev_exec_dmabuf(ns->kvdev_desc, ch, exec_key,
						    exec_key_len, op_id, ns->kv_read_only,
						    binding_arg, input, input_len,
						    kv_dmabuf_fd, kv_dmabuf_off,
						    kv_dmabuf_len, kv_dmabuf_iova,
						    nvmf_kvdev_exec_done, kv_req);
			if (rc == -ENOTSUP) {
				/* Backend cannot target a dma-buf sink (single-tier, or no
				 * two-tier front): fail rather than write the result to the
				 * wrong place. */
				req->kvdev_io_ctx = NULL;
				free(kv_req->bounce);
				free(kv_req);
				rsp->status.sct = SPDK_NVME_SCT_GENERIC;
				rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
				return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
			}
			break;
		}

		rc = spdk_kvdev_exec(ns->kvdev_desc, ch, exec_key, exec_key_len, op_id,
				     ns->kv_read_only, binding_arg, input, input_len,
				     data, output_len, nvmf_kvdev_exec_done, kv_req);
		if (rc == -ENOTSUP) {
			/* Backend has no exec op (e.g. librados until KVX-3): report
			 * an NVMe not-supported status. No completion will fire. */
			req->kvdev_io_ctx = NULL;
			free(kv_req->bounce);
			free(kv_req);
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
		break;
	}
	case SPDK_NVME_OPC_KV_LIST: {
		const void *start_key = key_len > 0 ? key : NULL;

		/* The host buffer (CDW10) must hold at least the 4-byte NRK header. */
		if (xfer_len < NVMF_KV_LIST_NRK_SIZE) {
			free(kv_req);
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		/* Assemble the return structure in a contiguous buffer (bounce when
		 * the payload is multi-iov). list_off starts past the NRK header,
		 * which nvmf_kvdev_list_done() patches once the count is known. */
		data = nvmf_kvdev_get_contig_buf(req, false, kv_req);
		if (data == NULL) {
			goto err_nomem;
		}
		kv_req->list_buf = data;
		kv_req->list_cap = xfer_len;
		kv_req->list_off = NVMF_KV_LIST_NRK_SIZE;

		rc = spdk_kvdev_list(ns->kvdev_desc, ch, start_key, key_len,
				     nvmf_kvdev_list_iter, kv_req,
				     nvmf_kvdev_list_done, kv_req);
		break;
	}
	default:
		SPDK_ERRLOG("Unsupported KV opcode 0x%02x\n", cmd->opc);
		free(kv_req);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (rc != 0) {
		/* The kvdev could not even accept the request; no completion will
		 * fire, so complete it here. Clear the abort ctx (the Exec path set
		 * req->kvdev_io_ctx = kv_req before submit) so a later ABORT on this
		 * reused request slot cannot match the freed kv_req pointer. */
		req->kvdev_io_ctx = NULL;
		free(kv_req->bounce);
		free(kv_req);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* The in-memory kvdev completes synchronously, but the contract is async:
	 * the completion callback has already (or will have) completed the
	 * request via spdk_nvmf_request_complete(). */
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;

err_nomem:
	free(kv_req->bounce);
	free(kv_req);
	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}
