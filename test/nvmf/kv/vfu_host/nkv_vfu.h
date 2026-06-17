/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Shared raw vfio-user NVMe-KV driver for the Option-A host client
 * (epic spdk-jhk, bead spdk-jhk.3). Used by both:
 *   nkv_vfu_host.c  -- CPU builds the SQE + rings the doorbell  (2c)
 *   nkv_vfu_gpu.hip -- GPU builds the SQE + rings the doorbell  (2d)
 *
 * The only difference between the two is nvfu_produce(): the producer step that
 * writes the SQE into the SQ ring and rings the SQ doorbell. Each main provides
 * its own (CPU or GPU). Everything else -- attach, admin queue, controller
 * enable, IO queue creation, completion polling -- is shared and identical.
 */

#ifndef NKV_VFU_H
#define NKV_VFU_H

#include "spdk/stdinc.h"
#include "spdk/barrier.h"
#include "spdk/env.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvme_kv.h"
#include "spdk/vfio_user_pci.h"

#include <linux/vfio.h>		/* VFIO_PCI_*_REGION_INDEX */

#define ADMIN_Q_ENTRIES	16
#define IO_Q_ENTRIES	128
#define IO_QID		1
#define NVME_DB_OFFSET	0x1000
#define KV_NSID		1

struct nvfu_queue {
	struct spdk_nvme_cmd	*sq;
	struct spdk_nvme_cpl	*cq;
	uint64_t		sq_iova;
	uint64_t		cq_iova;
	uint16_t		qid;
	uint16_t		depth;
	uint16_t		sq_tail;
	uint16_t		cq_head;
	uint8_t			cq_phase;
	uint16_t		cid;
	uint32_t		sq_db;
	uint32_t		cq_db;
};

struct nvfu_dev {
	struct vfio_device	*dev;
	volatile uint32_t	*doorbells;
	uint32_t		db_stride_u32;
	struct nvfu_queue	admin;
	struct nvfu_queue	io;
};

/*
 * Producer step (write SQE into SQ slot, ring SQ doorbell). Provided by each
 * main: CPU stores in nkv_vfu_host.c, a GPU kernel in nkv_vfu_gpu.hip. `slot`
 * is the SQ index to write; `new_tail` is the doorbell value to ring.
 */
int nvfu_produce(struct nvfu_dev *d, struct nvfu_queue *q,
		 const struct spdk_nvme_cmd *cmd, uint16_t slot, uint16_t new_tail);

static inline int
nvfu_bar(struct nvfu_dev *d, uint32_t region, uint32_t off, size_t len,
	 void *buf, bool is_write)
{
	return spdk_vfio_user_pci_bar_access(d->dev, region, off, len, buf, is_write);
}

static inline int nvfu_reg_get4(struct nvfu_dev *d, uint32_t off, uint32_t *v)
{ return nvfu_bar(d, VFIO_PCI_BAR0_REGION_INDEX, off, 4, v, false); }
static inline int nvfu_reg_get8(struct nvfu_dev *d, uint32_t off, uint64_t *v)
{ return nvfu_bar(d, VFIO_PCI_BAR0_REGION_INDEX, off, 8, v, false); }
static inline int nvfu_reg_set4(struct nvfu_dev *d, uint32_t off, uint32_t v)
{ return nvfu_bar(d, VFIO_PCI_BAR0_REGION_INDEX, off, 4, &v, true); }
static inline int nvfu_reg_set8(struct nvfu_dev *d, uint32_t off, uint64_t v)
{ return nvfu_bar(d, VFIO_PCI_BAR0_REGION_INDEX, off, 8, &v, true); }

#define REG_OFF(field) ((uint32_t)offsetof(struct spdk_nvme_registers, field))

/* iova == vaddr for vfio-user (IOVA-as-VA); see nkv_vfu_host.c notes. */
static inline void *
nvfu_dma_alloc(size_t size, uint64_t *iova)
{
	void *p = spdk_dma_zmalloc(size, 0x1000, NULL);

	if (p && iova) {
		*iova = (uint64_t)(uintptr_t)p;
	}
	return p;
}

static inline void
nvfu_queue_init_db(struct nvfu_dev *d, struct nvfu_queue *q)
{
	q->sq_db = (2u * q->qid) * d->db_stride_u32;
	q->cq_db = (2u * q->qid + 1u) * d->db_stride_u32;
	q->sq_tail = q->cq_head = 0;
	q->cq_phase = 1;
	q->cid = 0;
}

static inline int
nvfu_submit_poll(struct nvfu_dev *d, struct nvfu_queue *q,
		 struct spdk_nvme_cmd *cmd, struct spdk_nvme_cpl *out_cpl)
{
	volatile struct spdk_nvme_cpl *cqe;
	uint64_t deadline;
	uint16_t slot, new_tail;

	cmd->cid = q->cid++;
	slot = q->sq_tail;
	new_tail = (q->sq_tail + 1) % q->depth;

	nvfu_produce(d, q, cmd, slot, new_tail);	/* CPU or GPU */
	q->sq_tail = new_tail;

	cqe = &q->cq[q->cq_head];
	deadline = spdk_get_ticks() + 5 * spdk_get_ticks_hz();
	while (cqe->status.p != q->cq_phase) {
		if (spdk_get_ticks() > deadline) {
			fprintf(stderr, "qid:%u opc=0x%x TIMEOUT\n", q->qid, cmd->opc);
			return -ETIMEDOUT;
		}
		spdk_pause();
	}
	spdk_rmb();
	if (out_cpl) {
		*out_cpl = *(struct spdk_nvme_cpl *)(uintptr_t)cqe;
	}
	q->cq_head = (q->cq_head + 1) % q->depth;
	if (q->cq_head == 0) {
		q->cq_phase ^= 1;
	}
	d->doorbells[q->cq_db] = q->cq_head;
	return cqe->status.sc | (cqe->status.sct << 8);
}

static inline int
nvfu_pci_enable_dma(struct nvfu_dev *d)
{
	uint16_t cmd_reg = 0;

	if (nvfu_bar(d, VFIO_PCI_CONFIG_REGION_INDEX, 4, 2, &cmd_reg, false)) {
		return -EIO;
	}
	cmd_reg |= 0x404;	/* bus master + INTx disable */
	return nvfu_bar(d, VFIO_PCI_CONFIG_REGION_INDEX, 4, 2, &cmd_reg, true);
}

static inline int
nvfu_controller_enable(struct nvfu_dev *d)
{
	union spdk_nvme_cap_register cap;
	union spdk_nvme_cc_register cc;
	union spdk_nvme_csts_register csts;
	union spdk_nvme_aqa_register aqa;
	struct nvfu_queue *q = &d->admin;
	uint64_t to_us, deadline;

	if (nvfu_reg_get8(d, REG_OFF(cap), &cap.raw)) {
		return -EIO;
	}
	d->db_stride_u32 = 1u << cap.bits.dstrd;

	q->qid = 0;
	q->depth = ADMIN_Q_ENTRIES;
	q->sq = (struct spdk_nvme_cmd *)nvfu_dma_alloc(q->depth * sizeof(*q->sq), &q->sq_iova);
	q->cq = (struct spdk_nvme_cpl *)nvfu_dma_alloc(q->depth * sizeof(*q->cq), &q->cq_iova);
	if (!q->sq || !q->cq) {
		fprintf(stderr, "admin queue DMA alloc failed\n");
		return -ENOMEM;
	}
	nvfu_queue_init_db(d, q);

	aqa.raw = 0;
	aqa.bits.asqs = q->depth - 1;
	aqa.bits.acqs = q->depth - 1;
	if (nvfu_reg_set4(d, REG_OFF(aqa.raw), aqa.raw) ||
	    nvfu_reg_set8(d, REG_OFF(asq), q->sq_iova) ||
	    nvfu_reg_set8(d, REG_OFF(acq), q->cq_iova)) {
		return -EIO;
	}

	cc.raw = 0;
	cc.bits.en = 1;
	cc.bits.css = SPDK_NVME_CC_CSS_IOCS;
	cc.bits.iosqes = 6;
	cc.bits.iocqes = 4;
	if (nvfu_reg_set4(d, REG_OFF(cc.raw), cc.raw)) {
		return -EIO;
	}

	to_us = (uint64_t)cap.bits.to * 500ULL * 1000ULL;
	deadline = spdk_get_ticks() + (to_us * spdk_get_ticks_hz()) / 1000000ULL;
	do {
		if (nvfu_reg_get4(d, REG_OFF(csts.raw), &csts.raw)) {
			return -EIO;
		}
		if (csts.bits.cfs) {
			fprintf(stderr, "controller fatal status during enable\n");
			return -EIO;
		}
		if (csts.bits.rdy) {
			return 0;
		}
		usleep(1000);
	} while (spdk_get_ticks() < deadline);

	fprintf(stderr, "timeout waiting for CSTS.RDY\n");
	return -ETIMEDOUT;
}

static inline int
nvfu_identify_controller(struct nvfu_dev *d)
{
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_ctrlr_data *cdata;
	uint64_t iova;
	int status;

	memset(&cmd, 0, sizeof(cmd));
	cdata = (struct spdk_nvme_ctrlr_data *)nvfu_dma_alloc(4096, &iova);
	if (!cdata) {
		return -ENOMEM;
	}
	cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	cmd.dptr.prp.prp1 = iova;
	cmd.cdw10 = 0x01;
	status = nvfu_submit_poll(d, &d->admin, &cmd, NULL);
	if (status != 0) {
		fprintf(stderr, "Identify Controller failed: status=0x%x\n", status);
		spdk_dma_free(cdata);
		return -EIO;
	}
	printf("Identify Controller OK: Model '%.40s' NN=%u\n", cdata->mn, cdata->nn);
	spdk_dma_free(cdata);
	return 0;
}

static inline int
nvfu_create_io_queue(struct nvfu_dev *d)
{
	struct nvfu_queue *q = &d->io;
	struct spdk_nvme_cmd cmd;
	int status;

	q->qid = IO_QID;
	q->depth = IO_Q_ENTRIES;
	q->sq = (struct spdk_nvme_cmd *)nvfu_dma_alloc(q->depth * sizeof(*q->sq), &q->sq_iova);
	q->cq = (struct spdk_nvme_cpl *)nvfu_dma_alloc(q->depth * sizeof(*q->cq), &q->cq_iova);
	if (!q->sq || !q->cq) {
		fprintf(stderr, "IO queue DMA alloc failed\n");
		return -ENOMEM;
	}
	nvfu_queue_init_db(d, q);

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_CREATE_IO_CQ;
	cmd.dptr.prp.prp1 = q->cq_iova;
	cmd.cdw10 = ((uint32_t)(q->depth - 1) << 16) | q->qid;
	cmd.cdw11 = 0x1;	/* PC=1, IEN=0 */
	status = nvfu_submit_poll(d, &d->admin, &cmd, NULL);
	if (status != 0) {
		fprintf(stderr, "Create IO CQ failed: status=0x%x\n", status);
		return -EIO;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_CREATE_IO_SQ;
	cmd.dptr.prp.prp1 = q->sq_iova;
	cmd.cdw10 = ((uint32_t)(q->depth - 1) << 16) | q->qid;
	cmd.cdw11 = ((uint32_t)q->qid << 16) | 0x1;
	status = nvfu_submit_poll(d, &d->admin, &cmd, NULL);
	if (status != 0) {
		fprintf(stderr, "Create IO SQ failed: status=0x%x\n", status);
		return -EIO;
	}
	printf("IO queue qid:%u created (depth=%u, SQ doorbell idx=%u).\n",
	       q->qid, q->depth, q->sq_db);
	return 0;
}

static inline void
nvfu_kv_set_key(struct spdk_nvme_cmd *cmd, const char *key, uint8_t key_len)
{
	cmd->cdw11_bits.kv.kl = key_len;
	memcpy((uint8_t *)&cmd->cdw2, key, spdk_min(key_len, 8));
	if (key_len > 8) {
		memcpy((uint8_t *)&cmd->cdw14, key + 8, (size_t)(key_len - 8));
	}
}

static inline int
nvfu_kv_store(struct nvfu_dev *d, const char *key, const void *value, uint32_t value_len)
{
	struct spdk_nvme_cmd cmd;
	void *buf;
	uint64_t iova;
	int status;

	memset(&cmd, 0, sizeof(cmd));
	buf = nvfu_dma_alloc(spdk_max(value_len, 4096), &iova);
	if (!buf) {
		return -ENOMEM;
	}
	memcpy(buf, value, value_len);
	spdk_wmb();
	cmd.opc = SPDK_NVME_OPC_KV_STORE;
	cmd.nsid = KV_NSID;
	cmd.dptr.prp.prp1 = iova;
	cmd.cdw10_bits.kv.vsize = value_len;
	nvfu_kv_set_key(&cmd, key, (uint8_t)strlen(key));
	status = nvfu_submit_poll(d, &d->io, &cmd, NULL);
	spdk_dma_free(buf);
	if (status != 0) {
		fprintf(stderr, "KV Store failed: status=0x%x\n", status);
		return -EIO;
	}
	return 0;
}

static inline int
nvfu_kv_retrieve(struct nvfu_dev *d, const char *key, void *out, uint32_t out_len,
		 uint32_t *got_len)
{
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_cpl cpl;
	void *buf;
	uint64_t iova;
	int status;

	memset(&cmd, 0, sizeof(cmd));
	memset(&cpl, 0, sizeof(cpl));
	buf = nvfu_dma_alloc(spdk_max(out_len, 4096), &iova);
	if (!buf) {
		return -ENOMEM;
	}
	cmd.opc = SPDK_NVME_OPC_KV_RETRIEVE;
	cmd.nsid = KV_NSID;
	cmd.dptr.prp.prp1 = iova;
	cmd.cdw10_bits.kv.vsize = out_len;
	nvfu_kv_set_key(&cmd, key, (uint8_t)strlen(key));
	status = nvfu_submit_poll(d, &d->io, &cmd, &cpl);
	if (status != 0) {
		fprintf(stderr, "KV Retrieve failed: status=0x%x\n", status);
		spdk_dma_free(buf);
		return -EIO;
	}
	spdk_rmb();
	*got_len = spdk_min(cpl.cdw0, out_len);
	memcpy(out, buf, *got_len);
	spdk_dma_free(buf);
	return 0;
}

/*
 * KV Exec (ADR-0014, opcode 0x83): near-data compute. The op runs an
 * allowlisted module (selected by op_id) against the value stored under `key`,
 * read-only. The request rides the DPTR payload head as [u16 key_len][key]
 * [input]; the result comes back in the same buffer, its length in cpl.cdw0.
 * The key does NOT go in the inline CDW slots.
 */
static inline int
nvfu_kv_exec(struct nvfu_dev *d, const char *key, uint32_t op_id,
	     const void *input, uint32_t input_len,
	     void *out, uint32_t out_len, uint32_t *result_len)
{
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_cpl cpl;
	void *buf;
	uint64_t iova;
	uint8_t key_len = (uint8_t)strlen(key);
	uint16_t klp = key_len;
	uint32_t payload_len = (uint32_t)sizeof(uint16_t) + key_len + input_len;
	uint32_t bufsz = spdk_max(out_len, 4096);
	int status;

	memset(&cmd, 0, sizeof(cmd));
	memset(&cpl, 0, sizeof(cpl));
	if (payload_len > bufsz) {
		return -EINVAL;
	}
	buf = nvfu_dma_alloc(bufsz, &iova);
	if (!buf) {
		return -ENOMEM;
	}
	/* Stage [u16 key_len][key][input] at the buffer head. */
	memcpy(buf, &klp, sizeof(klp));
	memcpy((char *)buf + sizeof(klp), key, key_len);
	if (input_len) {
		memcpy((char *)buf + sizeof(klp) + key_len, input, input_len);
	}
	spdk_wmb();

	cmd.opc = SPDK_NVME_OPC_KV_EXEC;
	cmd.nsid = KV_NSID;
	cmd.dptr.prp.prp1 = iova;
	cmd.cdw10_bits.kv.vsize = payload_len;		/* request payload length */
	cmd.cdw12_bits.kv_exec.osize = out_len;		/* output buffer size */
	cmd.cdw13_bits.kv_exec.op_id = op_id;

	status = nvfu_submit_poll(d, &d->io, &cmd, &cpl);
	if (status != 0) {
		fprintf(stderr, "KV Exec op %u failed: status=0x%x\n", op_id, status);
		spdk_dma_free(buf);
		return -EIO;
	}
	spdk_rmb();
	*result_len = spdk_min(cpl.cdw0, out_len);
	memcpy(out, buf, *result_len);
	spdk_dma_free(buf);
	return 0;
}

/* Shared attach + bring-to-ready + IO queue. Returns 0 on success. */
static inline int
nvfu_open(struct nvfu_dev *d, const char *traddr_dir)
{
	char cntrl_path[PATH_MAX];
	void *db;

	snprintf(cntrl_path, sizeof(cntrl_path), "%s/cntrl", traddr_dir);
	if (access(cntrl_path, F_OK) != 0) {
		fprintf(stderr, "cntrl socket not found: %s\n", cntrl_path);
		return -ENOENT;
	}
	d->dev = spdk_vfio_user_setup(cntrl_path);
	if (d->dev == NULL) {
		fprintf(stderr, "spdk_vfio_user_setup(%s) failed\n", cntrl_path);
		return -EIO;
	}
	printf("Attached to vfio-user controller at %s (no QEMU, no lib/nvme)\n",
	       cntrl_path);

	db = spdk_vfio_user_get_bar_addr(d->dev, 0, NVME_DB_OFFSET, 0x1000);
	if (!db) {
		fprintf(stderr, "failed to map BAR0 doorbell page\n");
		return -EIO;
	}
	d->doorbells = (volatile uint32_t *)db;

	if (nvfu_pci_enable_dma(d) != 0 || nvfu_controller_enable(d) != 0) {
		return -EIO;
	}
	printf("Controller ENABLED (CSTS.RDY=1) via our own admin queue.\n");

	if (nvfu_identify_controller(d) != 0 || nvfu_create_io_queue(d) != 0) {
		return -EIO;
	}
	return 0;
}

static inline void
nvfu_close(struct nvfu_dev *d)
{
	spdk_dma_free(d->admin.sq);
	spdk_dma_free(d->admin.cq);
	spdk_dma_free(d->io.sq);
	spdk_dma_free(d->io.cq);
	if (d->dev) {
		spdk_vfio_user_release(d->dev);
	}
}

#endif /* NKV_VFU_H */
