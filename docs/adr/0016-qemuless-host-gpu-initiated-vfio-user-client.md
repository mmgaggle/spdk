---
status: proposed
---

# QEMU-less GPU-initiated host IO: the vfio-user controller is the invariant, the client is the swappable tier

> Draft authored autonomously for ratification. Captures the deployment/transport decision
> validated end-to-end on the dev box (gfx1151 iGPU + E810), branch `rados-nkvx`.

GPU-initiated NVMe-KV was first proven inside a QEMU guest: the iGPU was passed through
(`vfio-pci`), the SPDK vfio-user controller was presented to the guest by qemu-xio, and the
GPU rang a doorbell shuttled across the guest-physical boundary by a `pci-mmio-bridge` shadow.
For bare-metal and Kubernetes (rados-nkv as a systemd service / DaemonSet) — and eventually a
DPU — QEMU has to go. This ADR fixes **how** the GPU reaches the controller without it.

## Decisions

- **The SPDK vfio-user *controller* (server) is the single invariant across all deployment
  models; the vfio-user *client* is the per-deployment swappable tier.** The controller code
  that matters (decode `0x83`, allowlist, kvdev, executor routing) is byte-identical whether
  the client is **QEMU** (hypervisor), a **thin host client** (bare metal), or a **DPU/SNAP
  real NVMe function** (host+DPU). We never re-validate the datapath per deployment — only the
  thing in front of the socket changes. This is why we did *not* fork the controller for bare
  metal (the rejected "memfd-QP" alternative).

- **On bare metal the client is a raw host vfio-user NVMe driver, not `lib/nvme` and not QEMU.**
  It links SPDK's `lib/vfio_user` (`spdk_vfio_user_setup` / `_get_bar_addr` / `_pci_bar_access`),
  owns its own admin + IO queues, brings the controller to `RDY`, and creates an IO qpair. The
  GPU then rings an already-initialized queue (the CPU does one-time bring-up, exactly as the
  guest kernel did in the QEMU testbed). Reference client: `test/nvmf/kv/vfu_host/`.

- **The GPU rings the doorbell directly via a GPU-VM mapping of the BAR0 doorbell page — this is
  `amdkfd`/`hipHostRegister`, NOT iommufd.** iommufd governs *device → memory* DMA; the doorbell
  is the opposite direction (GPU → controller, a store into a region the controller polls). The
  `pci-mmio-bridge` shadow existed only to cross the guest-physical boundary; on the host the
  client's BAR0 mmap *is* host memory, handed to the GPU. A `gfx1151` HIP kernel builds the NVMe
  SQE and writes the doorbell with `__threadfence_system()` ordering. iommufd / verbs-dma-buf
  only become load-bearing for the **RDMA-into-VRAM** leg (the two-tier executor, ADR-0015), not
  for the doorbell.

- **IOVA-as-VA, with the buffer virtual address used directly as the DMA address.** vfio-user maps
  each registered region with `iova == vaddr` (`lib/vfio_user/host/vfio_user_pci.c`), and
  `spdk_vtophys` has no physical mapping for an unprivileged vfio-user client. So the client runs
  `--iova-mode=va` and programs the buffer VA into ASQ/ACQ/SGL/PRP. This is a hard requirement,
  not a tuning knob.

- **Killing QEMU dissolves the bulk producer↔doorbell coherence cliff (spdk-5co).** The cliff was
  an artifact of the GPU and the controller observing data through *two* mappings of a guest memfd,
  unordered. On bare metal there is a single host coherence domain: a GPU-produced value plus
  `__threadfence_system()` before the doorbell lands byte-exact. Verified GPU-produced bulk
  byte-exact 4 KiB → ~2 MiB (the >2 MiB cap is an unrelated target-side mapping limit).

- **Throughput uses wavefront batching (rocm-xio `batchSize` > 1).** One `gfx1151` wavefront
  (`warpSize` = 32) stages N SQEs and rings the doorbell once; the host reaps N completions. Applies
  to both Store and two-tier Exec.

## Consequences

- Bare-metal / Kubernetes: rados-nkv runs as a host process; no hypervisor in the datapath. The
  host CPU is reduced to one-time queue bring-up; the GPU initiates each op.
- DPU/host: the DPU presents a real NVMe function (SNAP), so the client tier disappears entirely
  and the host CPU is fully out of the datapath — the strongest version of the same factoring.
- The doorbell region must be GPU-importable (`amdkfd` dma-buf import / `hipHostRegister`); the
  SPDK controller already exposes it as an fd-backed shared page (mappable BAR0 / shadow doorbells),
  so no controller change is needed.
