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

static void
usage(const char *prog)
{
	fprintf(stderr,
		"usage:\n"
		"  %s <traddr> store    <key> <value>   GPU-initiated KV Store\n"
		"  %s <traddr> retrieve <key>           GPU-initiated KV Retrieve\n"
		"  %s <traddr> exec     <key> <op_id>   GPU-initiated KV Exec (10=bytecount, 11=identity)\n"
		"  %s <traddr> [demo]                   store+retrieve self-test\n"
		"  %s <traddr> demo-exec                store+retrieve+exec self-test\n"
		"<traddr> is the VFIOUSER listener dir containing 'cntrl'.\n",
		prog, prog, prog, prog, prog);
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
	    (!strcmp(mode, "exec") && argc != 5)) {
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
