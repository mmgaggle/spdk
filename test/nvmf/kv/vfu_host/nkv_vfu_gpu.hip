/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Phase 2 / Milestone 2d (epic spdk-jhk, bead spdk-jhk.3): Option A --
 * GPU-initiated host NVMe-KV. The GPU builds the NVMe SQE and rings the
 * doorbell directly; SPDK is purely the target; no QEMU, no lib/nvme.
 *
 * Shares the entire driver (attach, admin queue, controller enable, IO queue
 * creation, completion polling) with the CPU variant via nkv_vfu.h. The ONLY
 * difference is nvfu_produce(): here a HIP kernel writes the 64-byte SQE into
 * the IO SQ ring (in host memory mapped to the GPU) and writes the SQ doorbell
 * (the vfio-user BAR0 page mapped to the GPU). This is exactly the rocm-xio
 * nvme-ep direct nvmeBar0Gpu path, minus the bridge.
 *
 * The CPU still performs one-time controller bring-up (like the guest kernel
 * did in the QEMU testbed); the GPU rings an already-initialized IO queue.
 *
 * Usage: nkv_vfu_gpu <traddr-dir-containing-cntrl>
 */

#include <hip/hip_runtime.h>

extern "C" {
#include "nkv_vfu.h"
#include "spdk/log.h"
}

/* GPU-visible aliases of the IO SQ and the BAR0 doorbell page. */
static struct spdk_nvme_cmd	*g_sq_dev;
static volatile uint32_t	*g_db_dev;
static int			g_use_gpu;

/*
 * One GPU thread builds the submission: copy the 64-byte SQE into the SQ slot,
 * fence to system scope so the controller sees it, then ring the doorbell and
 * fence again. __threadfence_system() is the GPU analogue of the producer
 * coherence the QEMU path could never guarantee across the memfd boundary.
 */
__global__ void
nvfu_ring_kernel(struct spdk_nvme_cmd *sq, uint16_t slot, struct spdk_nvme_cmd sqe,
		 volatile uint32_t *db, uint32_t db_idx, uint16_t new_tail)
{
	if (threadIdx.x != 0 || blockIdx.x != 0) {
		return;
	}
	sq[slot] = sqe;
	__threadfence_system();
	db[db_idx] = new_tail;
	__threadfence_system();
}

extern "C" int
nvfu_produce(struct nvfu_dev *d, struct nvfu_queue *q,
	     const struct spdk_nvme_cmd *cmd, uint16_t slot, uint16_t new_tail)
{
	hipError_t e;

	/* Admin / bring-up commands always go via the CPU. Only the IO KV op,
	 * once g_use_gpu is set, is GPU-initiated. */
	if (!g_use_gpu || q->qid != IO_QID) {
		q->sq[slot] = *cmd;
		spdk_wmb();
		d->doorbells[q->sq_db] = new_tail;
		return 0;
	}

	nvfu_ring_kernel<<<1, 1>>>(g_sq_dev, slot, *cmd, g_db_dev, q->sq_db, new_tail);
	e = hipDeviceSynchronize();
	if (e != hipSuccess) {
		fprintf(stderr, "GPU ring kernel failed: %s\n", hipGetErrorString(e));
		return -EIO;
	}
	return 0;
}

/* Map the IO SQ and the doorbell page into the GPU's address space. */
static int
gpu_map(struct nvfu_dev *d)
{
	size_t sq_bytes = (size_t)d->io.depth * sizeof(struct spdk_nvme_cmd);
	hipError_t e;

	e = hipHostRegister(d->io.sq, sq_bytes, hipHostRegisterMapped);
	if (e != hipSuccess) {
		fprintf(stderr, "hipHostRegister(IO SQ) failed: %s\n", hipGetErrorString(e));
		return -EIO;
	}
	e = hipHostGetDevicePointer((void **)&g_sq_dev, d->io.sq, 0);
	if (e != hipSuccess) {
		fprintf(stderr, "GetDevicePointer(SQ): %s\n", hipGetErrorString(e));
		return -EIO;
	}

	/* The doorbell page is a vfio-user shared mapping; try normal mapped
	 * registration first, then I/O-memory registration as a fallback. */
	e = hipHostRegister((void *)d->doorbells, 0x1000, hipHostRegisterMapped);
	if (e != hipSuccess) {
		e = hipHostRegister((void *)d->doorbells, 0x1000, hipHostRegisterIoMemory);
	}
	if (e != hipSuccess) {
		fprintf(stderr, "hipHostRegister(doorbell page) failed: %s\n",
			hipGetErrorString(e));
		return -EIO;
	}
	e = hipHostGetDevicePointer((void **)&g_db_dev, (void *)d->doorbells, 0);
	if (e != hipSuccess) {
		fprintf(stderr, "GetDevicePointer(doorbell): %s\n", hipGetErrorString(e));
		return -EIO;
	}
	printf("GPU mapped: IO SQ + BAR0 doorbell page (sq_dev=%p db_dev=%p)\n",
	       (void *)g_sq_dev, (void *)g_db_dev);
	return 0;
}

/*
 * Wavefront batch: each lane stages one 64-byte SQE into its SQ slot; after a
 * block-wide barrier + system fence (all SQEs globally visible), lane 0 rings
 * the SQ doorbell ONCE for the whole batch. With n<=32 this is exactly one
 * gfx1151 wavefront (warpSize=32); n up to the IO queue depth uses ceil(n/32)
 * wavefronts in one block, still one doorbell ring (rocm-xio batchSize > 1).
 */
__global__ void
ring_wavefront(struct spdk_nvme_cmd *sq, uint16_t base, uint16_t depth,
	       const struct spdk_nvme_cmd *cmds, uint32_t n,
	       volatile uint32_t *db, uint32_t db_idx, uint16_t new_tail)
{
	uint32_t i = threadIdx.x;

	if (i < n) {
		sq[(base + i) % depth] = cmds[i];
	}
	__threadfence_system();		/* this lane's SQE visible system-wide */
	__syncthreads();		/* all lanes have staged + fenced */
	if (i == 0) {
		db[db_idx] = new_tail;	/* one doorbell for the whole batch */
		__threadfence_system();
	}
}

/*
 * Ring n pre-built SQEs in one wavefront + one doorbell, then reap n completions.
 * Returns the number that completed SUCCESS, or -1 on a GPU/timeout error.
 * Callers fill cmds[i] (incl. cid) and any DMA buffers before calling.
 */
static int
gpu_batch_ring_reap(struct nvfu_dev *d, struct spdk_nvme_cmd *cmds, uint32_t n)
{
	struct nvfu_queue *q = &d->io;
	struct spdk_nvme_cmd *cmds_dev = NULL;
	uint16_t base = q->sq_tail;
	uint16_t new_tail = (uint16_t)((base + n) % q->depth);
	uint32_t reaped, ok = 0;
	hipError_t e;

	e = hipHostRegister(cmds, n * sizeof(*cmds), hipHostRegisterMapped);
	if (e != hipSuccess) {
		fprintf(stderr, "hipHostRegister(cmds): %s\n", hipGetErrorString(e));
		return -1;
	}
	(void)hipHostGetDevicePointer((void **)&cmds_dev, cmds, 0);

	ring_wavefront<<<1, n>>>(g_sq_dev, base, q->depth, cmds_dev, n,
				 g_db_dev, q->sq_db, new_tail);
	e = hipDeviceSynchronize();
	hipHostUnregister(cmds);
	if (e != hipSuccess) {
		fprintf(stderr, "wavefront kernel failed: %s\n", hipGetErrorString(e));
		return -1;
	}
	q->sq_tail = new_tail;

	for (reaped = 0; reaped < n; reaped++) {
		volatile struct spdk_nvme_cpl *cqe = &q->cq[q->cq_head];
		uint64_t deadline = spdk_get_ticks() + 5 * spdk_get_ticks_hz();

		while (cqe->status.p != q->cq_phase) {
			if (spdk_get_ticks() > deadline) {
				fprintf(stderr, "batch reap timeout %u/%u\n", reaped, n);
				return (int)ok;
			}
			spdk_pause();
		}
		spdk_rmb();
		if (cqe->status.sc == 0 && cqe->status.sct == 0) {
			ok++;
		}
		q->cq_head = (q->cq_head + 1) % q->depth;
		if (q->cq_head == 0) {
			q->cq_phase ^= 1;
		}
		d->doorbells[q->cq_db] = q->cq_head;
	}
	return (int)ok;
}

static const char *
wf_desc(uint32_t n)	/* "1 wavefront" / "3 wavefronts" */
{
	static char b[24];
	uint32_t w = (n + 31) / 32;

	snprintf(b, sizeof(b), "%u wavefront%s", w, (w > 1) ? "s" : "");
	return b;
}

/* n GPU-initiated KV Stores in one wavefront + one doorbell. */
static int
gpu_store_batch(struct nvfu_dev *d, const char *prefix, uint32_t n)
{
	struct nvfu_queue *q = &d->io;
	const uint32_t SLOT = 4096;	/* one page per value -> single PRP */
	struct spdk_nvme_cmd *cmds = NULL;
	char *vbuf = NULL;
	uint64_t vbuf_iova = 0;
	uint32_t i;
	int ok, rc = -EIO;

	if (n == 0 || n > q->depth || n > 1024) {
		fprintf(stderr, "batch n must be 1..%u\n", (q->depth < 1024) ? q->depth : 1024);
		return -EINVAL;
	}
	cmds = (struct spdk_nvme_cmd *)nvfu_dma_alloc(n * sizeof(*cmds), NULL);
	vbuf = (char *)nvfu_dma_alloc((size_t)n * SLOT, &vbuf_iova);
	if (!cmds || !vbuf) {
		fprintf(stderr, "batch alloc failed\n");
		goto done;
	}
	for (i = 0; i < n; i++) {
		char key[32], val[64];
		int klen = snprintf(key, sizeof(key), "%s%u", prefix, i);
		int vlen = snprintf(val, sizeof(val), "wavefront-value-%u", i);
		struct spdk_nvme_cmd *c = &cmds[i];

		memcpy(vbuf + (size_t)i * SLOT, val, vlen);
		memset(c, 0, sizeof(*c));
		c->opc = SPDK_NVME_OPC_KV_STORE;
		c->nsid = KV_NSID;
		c->dptr.prp.prp1 = vbuf_iova + (uint64_t)i * SLOT;
		c->cdw10_bits.kv.vsize = vlen;
		nvfu_kv_set_key(c, key, (uint8_t)klen);
		c->cid = q->cid++;
	}
	spdk_wmb();

	ok = gpu_batch_ring_reap(d, cmds, n);
	if (ok < 0) {
		goto done;
	}
	printf("WAVEFRONT STORE-BATCH ok: %d/%u GPU-initiated stores in ONE doorbell "
	       "(%u-lane = %s), keys %s0..%s%u\n",
	       ok, n, n, wf_desc(n), prefix, prefix, n - 1);
	rc = ((uint32_t)ok == n) ? 0 : -EIO;
done:
	spdk_dma_free(cmds);
	spdk_dma_free(vbuf);
	return rc;
}

/*
 * n GPU-initiated KV Execs (op_id) against keys <prefix>0..<prefix>(n-1) in one
 * wavefront + one doorbell. Each Exec routes through rados-nkv to the two-tier
 * rados-nkvx executor; results land back in per-Exec buffers (in-place).
 */
static int
gpu_exec_batch(struct nvfu_dev *d, const char *prefix, uint32_t op_id, uint32_t n)
{
	struct nvfu_queue *q = &d->io;
	const uint32_t SLOT = 4096;
	struct spdk_nvme_cmd *cmds = NULL;
	char *buf = NULL;
	uint64_t buf_iova = 0;
	uint32_t i;
	int ok, rc = -EIO;

	if (n == 0 || n > q->depth || n > 1024) {
		fprintf(stderr, "batch n must be 1..%u\n", (q->depth < 1024) ? q->depth : 1024);
		return -EINVAL;
	}
	cmds = (struct spdk_nvme_cmd *)nvfu_dma_alloc(n * sizeof(*cmds), NULL);
	buf = (char *)nvfu_dma_alloc((size_t)n * SLOT, &buf_iova);
	if (!cmds || !buf) {
		fprintf(stderr, "batch alloc failed\n");
		goto done;
	}
	for (i = 0; i < n; i++) {
		char key[32];
		uint8_t klen = (uint8_t)snprintf(key, sizeof(key), "%s%u", prefix, i);
		uint16_t klp = klen;
		char *slot = buf + (size_t)i * SLOT;
		struct spdk_nvme_cmd *c = &cmds[i];
		uint32_t payload_len = (uint32_t)sizeof(uint16_t) + klen;	/* input_len 0 */

		/* ADR-0014 request head: [u16 key_len][key] */
		memcpy(slot, &klp, sizeof(klp));
		memcpy(slot + sizeof(klp), key, klen);
		memset(c, 0, sizeof(*c));
		c->opc = SPDK_NVME_OPC_KV_EXEC;
		c->nsid = KV_NSID;
		c->dptr.prp.prp1 = buf_iova + (uint64_t)i * SLOT;
		c->cdw10_bits.kv.vsize = payload_len;
		c->cdw12_bits.kv_exec.osize = SLOT;
		c->cdw13_bits.kv_exec.op_id = op_id;
		c->cid = q->cid++;
	}
	spdk_wmb();

	ok = gpu_batch_ring_reap(d, cmds, n);
	if (ok < 0) {
		goto done;
	}
	spdk_rmb();
	printf("WAVEFRONT EXEC-BATCH ok: %d/%u GPU-initiated Execs (op %u) in ONE doorbell "
	       "(%u-lane = %s) -> two-tier executor, keys %s0..%s%u\n",
	       ok, n, op_id, n, wf_desc(n), prefix, prefix, n - 1);
	if (op_id == 10) {	/* nkvx:bytecount -> LE u64 per Exec, in its own buffer */
		uint64_t c0 = 0, cl = 0;

		memcpy(&c0, buf, 8);
		memcpy(&cl, buf + (size_t)(n - 1) * SLOT, 8);
		printf("  sample bytecount: %s0=%llu  %s%u=%llu\n",
		       prefix, (unsigned long long)c0, prefix, n - 1, (unsigned long long)cl);
	}
	rc = ((uint32_t)ok == n) ? 0 : -EIO;
done:
	spdk_dma_free(cmds);
	spdk_dma_free(buf);
	return rc;
}

/* GPU produces the bulk value: each lane writes its bytes of a verifiable
 * pattern, then a system fence makes the whole buffer visible to the controller. */
__global__ void
fill_pattern(uint8_t *buf, uint32_t size)
{
	uint32_t stride = blockDim.x * gridDim.x;
	uint32_t i;

	for (i = blockIdx.x * blockDim.x + threadIdx.x; i < size; i += stride) {
		buf[i] = (uint8_t)((i * 131u + 7u) & 0xffu);
	}
	__threadfence_system();
}

/*
 * Size-ladder / spdk-5co coherence test: the GPU PRODUCES a large value (kernel
 * fills the pattern), then GPU-initiated Store; CPU retrieves and verifies
 * byte-exact. This is the faithful spdk-5co scenario (GPU bulk production + the
 * doorbell) on bare metal. Uses a single contiguous SGL descriptor (no PRP
 * list / 512-page boundary), so it scales to the controller max (64 MiB).
 */
static int
gpu_store_big(struct nvfu_dev *d, const char *key, uint32_t size)
{
	uint8_t *vbuf = NULL, *rbuf = NULL, *vbuf_dev = NULL;
	uint64_t vbuf_iova = 0, rbuf_iova = 0;
	struct spdk_nvme_cpl cpl;
	uint32_t got = 0, i, bad = 0, threads = 256, blocks;
	hipError_t e;
	int rc = -EIO;

	/* Byte-exact through 511 pages with BOTH SGL and PRP; exactly 512 pages
	 * (2 MiB) and beyond fail identically -- a target-side limit (max-iovec /
	 * DMA-region boundary at 2 MiB), not a client transport choice. Capped at
	 * the proven range; >2 MiB needs target-side work. */
	if (size == 0 || size > 511u * 4096u) {
		fprintf(stderr, "store-big size must be 1..%u bytes (target caps a single "
			"KV transfer at 511 pages / ~2MiB)\n", 511u * 4096u);
		return -EINVAL;
	}
	vbuf = (uint8_t *)nvfu_dma_alloc(size, &vbuf_iova);
	rbuf = (uint8_t *)nvfu_dma_alloc(size, &rbuf_iova);
	if (!vbuf || !rbuf) {
		fprintf(stderr, "store-big alloc failed\n");
		goto done;
	}

	/* GPU produces the bulk value into the host-registered buffer. */
	e = hipHostRegister(vbuf, size, hipHostRegisterMapped);
	if (e != hipSuccess) {
		fprintf(stderr, "hipHostRegister(vbuf): %s\n", hipGetErrorString(e));
		goto done;
	}
	(void)hipHostGetDevicePointer((void **)&vbuf_dev, vbuf, 0);
	blocks = (size + threads - 1) / threads;
	if (blocks > 65535) {
		blocks = 65535;
	}
	fill_pattern<<<blocks, threads>>>(vbuf_dev, size);
	e = hipDeviceSynchronize();
	if (e != hipSuccess) {
		fprintf(stderr, "fill kernel: %s\n", hipGetErrorString(e));
		hipHostUnregister(vbuf);
		goto done;
	}

	/* GPU-initiated Store of the GPU-produced bulk (single contiguous SGL). */
	g_use_gpu = 1;
	rc = nvfu_kv_xfer_sgl(d, SPDK_NVME_OPC_KV_STORE, key, size, vbuf_iova, NULL);
	hipHostUnregister(vbuf);
	if (rc != 0) {
		goto done;
	}

	/* CPU retrieve + byte-exact verify of the GPU-produced pattern. */
	g_use_gpu = 0;
	memset(&cpl, 0, sizeof(cpl));
	if (nvfu_kv_xfer_sgl(d, SPDK_NVME_OPC_KV_RETRIEVE, key, size, rbuf_iova, &cpl) != 0) {
		rc = -EIO;
		goto done;
	}
	got = cpl.cdw0;
	for (i = 0; i < size; i++) {
		if (rbuf[i] != (uint8_t)((i * 131u + 7u) & 0xffu)) {
			if (bad < 3) {
				fprintf(stderr, "  mismatch @%u: got 0x%02x want 0x%02x\n",
					i, rbuf[i], (uint8_t)((i * 131u + 7u) & 0xffu));
			}
			bad++;
		}
	}
	printf("STORE-BIG %s: GPU produced+stored %u bytes (%u pages, SGL), retrieved %u -> %s "
	       "(%u mismatches)\n", key, size, (size + 4095) / 4096, got,
	       (got == size && bad == 0) ? "BYTE-EXACT" : "MISMATCH", bad);
	rc = (got == size && bad == 0) ? 0 : -EIO;
done:
	spdk_dma_free(vbuf);
	spdk_dma_free(rbuf);
	return rc;
}

static uint64_t
parse_size(const char *s)
{
	char *end = NULL;
	uint64_t v = strtoull(s, &end, 0);

	if (end && (*end == 'k' || *end == 'K')) {
		v *= 1024;
	} else if (end && (*end == 'm' || *end == 'M')) {
		v *= 1024u * 1024u;
	}
	return v;
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"usage:\n"
		"  %s <traddr> store      <key> <value>   GPU-initiated KV Store\n"
		"  %s <traddr> retrieve   <key>           GPU-initiated KV Retrieve\n"
		"  %s <traddr> exec       <key> <op_id>   GPU-initiated KV Exec (10=bytecount, 11=identity)\n"
		"  %s <traddr> store-batch <prefix> <N>   N GPU Stores in ONE wavefront + ONE doorbell\n"
		"  %s <traddr> exec-batch <prefix> <op_id> <N>  N GPU Execs in ONE wavefront (two-tier)\n"
		"  %s <traddr> store-big  <key> <size>    GPU produces+stores a big value (e.g. 256K, 2M), verify\n"
		"  %s <traddr> [demo]                     store+retrieve self-test\n"
		"  %s <traddr> demo-exec                  store+retrieve+exec self-test\n"
		"<traddr> is the VFIOUSER listener dir containing 'cntrl'.\n",
		prog, prog, prog, prog, prog, prog, prog, prog);
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	struct nvfu_dev d;
	char ealargs[64];
	const char *traddr, *mode, *key;
	int ndev = 0;
	int rc = 1;

	memset(&d, 0, sizeof(d));

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}
	traddr = argv[1];
	mode = (argc >= 3) ? argv[2] : "demo";

	/* Validate args per mode up front. */
	if ((!strcmp(mode, "store") && argc != 5) ||
	    (!strcmp(mode, "retrieve") && argc != 4) ||
	    (!strcmp(mode, "exec") && argc != 5) ||
	    (!strcmp(mode, "store-batch") && argc != 5) ||
	    (!strcmp(mode, "store-big") && argc != 5) ||
	    (!strcmp(mode, "exec-batch") && argc != 6)) {
		usage(argv[0]);
		return 1;
	}

	if (hipGetDeviceCount(&ndev) != hipSuccess || ndev < 1) {
		fprintf(stderr, "no HIP device available\n");
		return 1;
	}

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "nkv_vfu_gpu";
	snprintf(ealargs, sizeof(ealargs), "--iova-mode=va%s",
		 getenv("NKV_EAL_DEBUG") ? " --log-level=lib.eal:8" : "");
	opts.env_context = ealargs;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	/* CPU does one-time controller bring-up (admin queue + IO queue); the GPU
	 * then builds the SQE and rings the doorbell for each op. */
	if (nvfu_open(&d, traddr) != 0 || gpu_map(&d) != 0) {
		goto out;
	}
	g_use_gpu = 1;	/* every datapath op below is GPU-initiated */

	if (!strcmp(mode, "store")) {
		key = argv[3];
		const char *value = argv[4];
		if (nvfu_kv_store(&d, key, value, (uint32_t)strlen(value)) != 0) {
			goto out;
		}
		printf("STORE ok: GPU stored key='%s' (%zu bytes)\n", key, strlen(value));
		rc = 0;
	} else if (!strcmp(mode, "store-batch")) {
		const char *prefix = argv[3];
		uint32_t nbatch = (uint32_t)strtoul(argv[4], NULL, 0);
		if (gpu_store_batch(&d, prefix, nbatch) != 0) {
			goto out;
		}
		rc = 0;
	} else if (!strcmp(mode, "exec-batch")) {
		const char *prefix = argv[3];
		uint32_t op_id = (uint32_t)strtoul(argv[4], NULL, 0);
		uint32_t nbatch = (uint32_t)strtoul(argv[5], NULL, 0);
		if (gpu_exec_batch(&d, prefix, op_id, nbatch) != 0) {
			goto out;
		}
		rc = 0;
	} else if (!strcmp(mode, "store-big")) {
		key = argv[3];
		uint32_t size = (uint32_t)parse_size(argv[4]);
		if (gpu_store_big(&d, key, size) != 0) {
			goto out;
		}
		rc = 0;
	} else if (!strcmp(mode, "retrieve")) {
		char buf[4096];
		uint32_t got = 0;
		key = argv[3];
		if (nvfu_kv_retrieve(&d, key, buf, sizeof(buf) - 1, &got) != 0) {
			goto out;
		}
		buf[got] = '\0';
		printf("RETRIEVE ok: GPU retrieved key='%s' (%u bytes): %s\n", key, got, buf);
		rc = 0;
	} else if (!strcmp(mode, "exec")) {
		uint8_t exbuf[4096];
		uint32_t op_id = (uint32_t)strtoul(argv[4], NULL, 0);
		uint32_t exlen = 0;
		key = argv[3];
		memset(exbuf, 0, sizeof(exbuf));
		if (nvfu_kv_exec(&d, key, op_id, NULL, 0, exbuf, sizeof(exbuf), &exlen) != 0) {
			goto out;
		}
		if (op_id == 10) {	/* nkvx:bytecount -> LE u64 */
			uint64_t count = 0;
			memcpy(&count, exbuf, (exlen < sizeof(count)) ? exlen : sizeof(count));
			printf("EXEC ok: GPU exec op %u (nkvx:bytecount) key='%s' -> count=%llu\n",
			       op_id, key, (unsigned long long)count);
		} else {		/* nkvx:identity and friends -> raw bytes */
			printf("EXEC ok: GPU exec op %u key='%s' -> %u bytes: %.*s\n",
			       op_id, key, exlen, (int)exlen, (char *)exbuf);
		}
		rc = 0;
	} else {
		/* self-test: store -> retrieve (-> exec) round-trip */
		const char *k = "kvgpu01";
		const char *v = "GPU-initiated near-data KV write over vfio-user";
		char buf[256];
		uint32_t got = 0;
		int demo_exec = !strcmp(mode, "demo-exec") || !strcmp(mode, "exec");

		memset(buf, 0, sizeof(buf));
		if (nvfu_kv_store(&d, k, v, (uint32_t)strlen(v)) != 0) {
			goto out;
		}
		printf("GPU-initiated KV Store OK (key='%s', %zu bytes)\n", k, strlen(v));
		if (nvfu_kv_retrieve(&d, k, buf, sizeof(buf) - 1, &got) != 0) {
			goto out;
		}
		printf("Retrieve OK (%u bytes): '%s'\n", got, buf);
		if (got != strlen(v) || memcmp(buf, v, got) != 0) {
			fprintf(stderr, "FAIL: value mismatch\n");
			goto out;
		}
		if (demo_exec) {
			uint8_t exbuf[256];
			uint32_t exlen = 0;
			uint64_t bytecount = 0;

			if (nvfu_kv_exec(&d, k, 10, NULL, 0, exbuf, sizeof(exbuf), &exlen) != 0) {
				goto out;
			}
			memcpy(&bytecount, exbuf, (exlen < 8) ? exlen : 8);
			printf("Exec op10 nkvx:bytecount -> count=%llu (expect %zu)\n",
			       (unsigned long long)bytecount, strlen(v));
			memset(exbuf, 0, sizeof(exbuf));
			if (nvfu_kv_exec(&d, k, 11, NULL, 0, exbuf, sizeof(exbuf), &exlen) != 0) {
				goto out;
			}
			printf("Exec op11 nkvx:identity -> %u bytes: '%.*s'\n",
			       exlen, (int)exlen, (char *)exbuf);
			if (bytecount != strlen(v) || exlen != strlen(v) ||
			    memcmp(exbuf, v, exlen) != 0) {
				fprintf(stderr, "FAIL: Exec result mismatch\n");
				goto out;
			}
		}
		printf("PASS: QEMU-less GPU-initiated host IO%s complete.\n",
		       demo_exec ? " + rados-nkvx Exec" : "");
		rc = 0;
	}

out:
	if (g_sq_dev) {
		hipHostUnregister(d.io.sq);
	}
	nvfu_close(&d);
	return rc;
}
