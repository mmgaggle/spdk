# Slice B-i V4 — GPU-initiated VRAM-direct two-tier KV Exec CAPSTONE (HITL runbook)

**Goal (end-state).** The GPU (`gfx1151`, vfio-passthrough at `0000:be:00.0`)
issues a **KV Exec** (opcode `0x83`, op11 *identity*) via rocm-xio's
`--exec-vram` **mixed-SGL** path. `qemu-xio` (V1, `x-dmabuf-export=on`) exports
the GPU VRAM BAR segment as a **dma-buf** and ships it over vfio-user. SPDK
`nvmf_tgt` (V2-A + R-fixes) parses the RAM head, registers the dma-buf
**`result_sink`**, and forwards the Exec over Mercury **`ofi+verbs;ofi_rxm`** to a
standalone `nkvx_service` executor on the E810 DAC. The executor **RDMA-WRITEs**
the **64 MiB** identity result straight into GPU VRAM. We then verify the VRAM
body is **bit-exact**.

> ## SAFETY — this is a STAGING doc; a separate LIVE rig may be running
> - **Never** touch/signal qemu **pid 23630**, nvmf_tgt **263206**, nkvx_service
>   **262272**, or any `/tmp/cgi-*` socket/file. The V4 rig uses a **separate**
>   `/tmp/cgi4-*` namespace and `bringup-v4-vram-direct.sh` **hard-refuses** to
>   run if the live rig is detected.
> - Do **not** run `sync_rocmxio_to_guest.sh` or boot any guest while the live
>   guest holds the qcow2 (single-writer → corruption).
> - The V4 run **requires a host reboot** into the GPU-passthrough GRUB entry,
>   so by definition it happens in a dedicated window with the live rig **down**.

---

## Topology & the two deltas vs the live rig

Reverse-engineered live launch (read-only from `/proc/<pid>/cmdline`):

| process | live launch (today) |
|---|---|
| qemu (pid 23630) | `qemu-xio` + `-device vfio-pci,host=0000:be:00.0` + `pci-mmio-bridge,shadow-gpa=0x80000000,shadow-size=8192,poll-interval-ns=10000` + `-device {"driver":"vfio-user-pci","id":"vfukv","bus":"pcie-vfu.1","socket":{"path":"/tmp/cgi-muser/kv/cntrl","type":"unix"}}` + memfd 32 GiB + qcow2 + hostfwd 2222 + qmp `/tmp/cgi-qmp.sock` |
| nvmf_tgt (263206) | `nvmf_tgt -r /tmp/cgi-spdk.sock -m 0x6 --no-huge -s 1024`; RPCs (`cgi-stage-exec-host.sh`): `nvmf_create_transport -t VFIOUSER -q 1024 -m 16`; `kvdev_rados_register_cluster ceph0`; `kvdev_rados_create KvRados0 ceph0 kvpool`; `nvmf_create_subsystem … -a`; `nvmf_subsystem_add_kv_ns`; `nvmf_subsystem_add_listener -t VFIOUSER -a /tmp/cgi-muser/kv -s 0` |
| executor (262272) | `nkvx_service --listen na+sm:// --addr-file /tmp/cgi-exec.addr --rados-pool kvpool …` |

**V4 deltas (only two):**
1. **qemu:** add `x-dmabuf-export=true` on the `vfio-user-pci` device JSON (V1
   contract — `qemu-xio/DMABUF-EXPORT-V1-NOTES.md`). Default-off; opt-in.
2. **two-tier transport:** executor `--listen na+sm://` → **`ofi+verbs;ofi_rxm://10.110.0.1`**,
   and the SPDK front bridges to it via `kvdev_rados_create … --remote-executor
   "<executor verbs self-addr>"`. The live rig is single-tier in-process
   (no `--remote-executor`); V4 forwards over verbs (proven in slice **C8**,
   `docs/runbooks/slice-c8-verbs-acceptance.md`).

Build provenance the V4 nvmf_tgt MUST carry: **V2-A** (dma-buf result_sink
consumption) **+ R-fixes**:
- **R1** — `_map_one()` only emits the dma-buf sentinel when the command is
  `SPDK_NVME_OPC_KV_EXEC` **AND** the ns CSI is `SPDK_NVME_CSI_KV`
  (`lib/nvmf/vfio_user.c:1725-1730`). A vendor 0x83 against an NVM ns falls back
  to NULL — no sentinel escapes to a bdev consumer.
- **R2** — reject a sub-4 KiB dma-buf sink (in-flight in parallel; confirm the
  reject guard is present before relying on the runbook's go/no-go).

Key source anchors:
- dma-buf sink mapping + log line: `lib/nvmf/vfio_user.c:1713-1758` →
  `SPDK_DEBUGLOG(nvmf_vfio, "dma-buf result_sink: iova=%#lx len=%#lx fd=%d offset=%#lx")` (`:1748`).
- mixed-SGL `[RAM head][dma-buf body]` shape parse/reject:
  `lib/nvmf/ctrlr_kvdev.c:548-645`.
- front `--remote-executor` plumbing: `module/kvdev/rados/kvdev_rados_rpc.c:234`,
  `module/kvdev/rados/kvdev_rados.c:2545-2554`; CLI flag `--remote-executor`
  in `python/spdk/cli/kvdev.py:103`.
- producer `--exec-vram`: rocm-xio `wip-v3-mixed-sgl`,
  `src/tester/xio-cli-options.cpp:200`, `src/endpoints/nvme-ep/nvme-ep.hip:2648-`,
  `KV exec OK: op_id=%u returned output length = %u bytes` at `:1282`.

---

## 0. Pre-window prep (do this BEFORE the reboot; safe with the live rig up only for the build steps that don't disturb it)

> These touch only files/binaries, never the live processes. The build is the
> same worktree the live target runs from — if you rebuild, you relink the
> binary on disk but the running pid 263206 keeps its old image until restarted.
> Prefer to build into a copy or simply **trust the already-built binaries**
> (they carry V2-A + R1). `bringup-v4-vram-direct.sh --build` will rebuild.

- [ ] **R-fixed + V2-A nvmf_tgt** built: confirm
      `grep "ns->csi == SPDK_NVME_CSI_KV" lib/nvmf/vfio_user.c` (R1) and the
      `dma-buf result_sink` debuglog are in the tree. (R2 reject present.)
- [ ] **V1 qemu** built: `/home/kyle/src/qemu-xio/build/qemu-system-x86_64`
      supports the `x-dmabuf-export` device property (see V1 NOTES).
- [ ] **nkvx_service** built:
      `make -C module/kvdev/rados/nkvx_service` →
      `module/kvdev/rados/nkvx_service/nkvx_service`.
- [ ] **Guest baked with V3**: the guest qcow2 must contain rocm-xio
      `wip-v3-mixed-sgl` (the `--exec-vram` tester + the per-page
      `GET_BUFFER_PAGES` kernel module). **Document only** — the durable bake is
      `sync_rocmxio_to_guest.sh`; **DO NOT run it while the live guest holds the
      qcow2.** In-window you can instead deploy live with
      `/home/kyle/nkvx-repro/deploy-guest-rocmxio.sh` after the V4 guest boots
      (host rocm-xio tree must be on `wip-v3-mixed-sgl`).
- [ ] **E810 / RoCEv2 up**: f0 `10.110.0.1` (root ns, `irdma0`); f1
      `10.110.0.2` (netns `nvmf_e810`, `irdma1`). `fi_info -p "verbs;ofi_rxm"`
      shows an irdma domain.
- [ ] **memlock raised** for the launching shell (irdma CQ/QP pin):
      `ulimit -l unlimited` (or durable
      `module/kvdev/rados/nkvx_service/deploy/99-nkvx-memlock.conf`). Verify
      `ulimit -l` prints `unlimited` (or ≥ 1 GiB).
- [ ] **/dev/udmabuf** accessible (root:kvm 0660) — only needed for the
      **CPU-only dma-buf smoke** (step 4), not the GPU shot. Join `kvm` or run
      the smoke with privilege.
- [ ] **IOMMU on** + the GPU-passthrough GRUB entry exists (`vfio-pci.ids=…`).

---

## 1. HOST REBOOT → select the GPU-passthrough GRUB entry — **USER ACTION**

**Reboot the host and, at the GRUB menu, MANUALLY select the GPU-passthrough
entry.** Never `grub2-reboot`/`grub2-set-default` it (operator preference: the
human picks the entry by hand). After boot, confirm:

```bash
grep -o 'vfio-pci.ids=[^ ]*' /proc/cmdline      # passthrough entry booted
lspci -nn | grep -i be:00.0                       # 1002:1586 present
```

> **GPU one-shot budget:** you get **ONE clean GPU shot per reboot**. A faulting
> shot dirties the SMU and subsequent shots are unreliable until the next
> reboot. Do **not** spend it until every go/no-go gate below is green.

---

## 2. Bring up the V4 rig

```bash
ulimit -l unlimited; ulimit -l           # MUST read unlimited (or >=1GiB)
bash /home/kyle/nkvx-repro/bringup-v4-vram-direct.sh
#   add --build to force a full rebuild first
```

What it does (and the go/no-go it enforces):
- **GUARD 0** — hard-refuses if pid 23630, any qemu, the live `/tmp/cgi-*`
  sockets, or a foreign nvmf_tgt/nkvx_service are alive.
- binds the GPU to `vfio-pci`; vstart Ceph + `kvpool` (skips if already up).
- **starts the verbs executor** and asserts its published self-addr contains
  `verbs` (the na+sm→verbs delta). **Go/no-go:** addr-file must be non-empty and
  verbs; a CQ `-12`/ENOMEM here means memlock is still capped → go back to step 0.
- **starts nvmf_tgt**, wires VFIOUSER + `kvdev_rados_create … --remote-executor
  <verbs addr>` + the op10/op11 allowlist. **Go/no-go:** `cntrl` socket appears.
- **launches qemu-xio V1 with `x-dmabuf-export=true`** on the `vfukv` device +
  the GPU. **Go/no-go:** qemu pid present, guest ssh up, `/dev/nvme0` +
  `/dev/rocm-xio` in the guest.

All V4 artifacts live under `/tmp/cgi4-*` (logs: `cgi4-nvmf_tgt.log`,
`cgi4-nkvx-service.log`, `cgi4-gpu-vm-*.log`).

---

## 3. Bake / deploy the guest with V3 (if not pre-baked)

If `ssh -p 2222 kyle@localhost '~/rocm-xio/build/xio-tester --help | grep -- --exec-vram'`
is empty, deploy V3 into the running V4 guest:

```bash
# host rocm-xio tree MUST be on wip-v3-mixed-sgl
( cd /home/kyle/src/rocm-xio && git rev-parse --abbrev-ref HEAD )   # -> wip-v3-mixed-sgl
bash /home/kyle/nkvx-repro/deploy-guest-rocmxio.sh
```

(The durable, reboot-surviving bake is `sync_rocmxio_to_guest.sh` — **not run in
this window**; it needs the guest qcow2 not otherwise open.)

---

## 4. **CPU-only dma-buf smoke FIRST** — de-risk WITHOUT spending the GPU shot

Prove the entire dma-buf `result_sink` + verbs + executor path end-to-end with a
**HOST udmabuf** sink (same registration path the GPU uses, minus the GPU):

```bash
make -C /home/kyle/src/spdk/.claude/worktrees/e810-test/module/kvdev/rados/nkvx_service
/home/kyle/src/spdk/.claude/worktrees/e810-test/module/kvdev/rados/nkvx_service/nkvx_dmabuf_test.sh \
  "ofi+verbs;ofi_rxm://10.110.0.1"
```

**Go/no-go:** must print
`RESULT: PASS — identity result RDMA-written BIT-EXACT into the HOST udmabuf
result_sink over verbs`. If this fails, **do not fire the GPU** — the verbs/
dma-buf/executor plumbing is broken independent of the GPU.

> **Caveat (record in the bead):** S2 proved `ibv_reg_dmabuf_mr` over a **udmabuf**.
> The GPU shot is the first proof of `ibv_reg_dmabuf_mr` over an **irdma** MR on
> a **vfio_pci BAR** dma-buf. See Risks §irdma-BAR below for a GPU-free way to
> de-risk that exact primitive if a spare vfio-pci device is available.

---

## 5. **THE GPU SHOT** — `--exec-vram` 64 MiB, then verify

```bash
bash /home/kyle/nkvx-repro/exec-vram-validate.sh
```

Exact producer command it runs in the guest (op11 identity, 64 MiB, mixed SGL):

```bash
sudo LD_LIBRARY_PATH=/opt/rocm/lib HSA_FORCE_FINE_GRAIN_PCIE=1 \
  ~/rocm-xio/build/xio-tester -m 8 --pci-mmio-bridge nvme-ep --controller /dev/nvme0 \
  --kv-op exec --op-id 11 --key vramcap \
  --input-size 0 --value-size 67108864 --data-buffer-size 67108864 \
  --write-io 1 --read-io 1 --lfsr-seed 0xC0FFEE --exec-vram --verify
```
(A 4 KiB store of `vramcap` precedes it so identity has bytes to echo. `-m 8`
[VRAM data buffer] is **mandatory** for `--exec-vram`.)

**Three go/no-go gates the validator asserts:**
- **[A] dma-buf result_sink engaged** — `cgi4-nvmf_tgt.log` shows
  `dma-buf result_sink: iova=… len=… fd=… offset=…` for THIS run. This is the
  binary proof `x-dmabuf-export` engaged and the VRAM fd reached SPDK.
  *(If the line is absent, enable the debuglog: relaunch nvmf_tgt with
  `-L nvmf_vfio`.)*
- **[B] CQE SUCCESS + DW0** — `KV exec OK: op_id=11 returned output length =
  67108864 bytes` (DW0 == value-size, no truncation).
- **[C] VRAM body bit-exact** — the in-tester `--verify` D2H readback PASS
  (`Verify Passed: N` / `VERIFY PASS`). **Cross-check:** identity echoes the
  stored object, so the rados object sha is the bit-exact target.

> **[C] caveat:** on some `wip-v3-mixed-sgl` tips the *exec-result* readback
> verify is supplied by the **instrumented `nvme-ep.hip`** that
> `exec-scatter-validate.sh` scp's in (the committed verify-readback block gates
> on `read-io>0 && write-io>0` and exec is write-only). If the tester prints no
> `Verify` line, deploy that instrumented `nvme-ep.hip` first, or rely on
> [A]+[B]+rados-sha as the proof set.

---

## Rollback / abort

- Any pre-GPU gate red → **abort the shot**, fix, re-smoke (step 4). The GPU
  budget is untouched as long as you don't run step 5.
- Tear down V4 cleanly (does not touch the live rig — different namespace):
  ```bash
  pkill -f 'nvmf_tgt -r /tmp/cgi4-spdk.sock'
  pkill -f 'nkvx_service .*cgi4-exec.addr'
  sudo pkill -f 'qemu-system-x86_64.*cgi4-qmp.sock'
  rm -rf /tmp/cgi4-*
  ```
- A faulted GPU shot → **reboot** to clear the SMU before retrying (budget spent).

---

## Risks / unknowns the operator must watch

1. **irdma `ibv_reg_dmabuf_mr` over a vfio_pci BAR dma-buf — UNPROVEN.** S2 only
   proved the registration over a **udmabuf** (host RAM). The GPU shot is the
   first time irdma registers an MR over a **device-MMIO/P2P** dma-buf exported
   from a vfio_pci BAR. *GPU-free de-risk:* if a **spare vfio-pci device** with a
   ≥64 MiB MMIO BAR is available, export that BAR via the same V1
   `VFIO_DEVICE_FEATURE_DMA_BUF` path and attempt `ibv_reg_dmabuf_mr` +
   RDMA-WRITE into it — this isolates the irdma-on-BAR-dma-buf primitive from the
   GPU without spending the budget. If it `EOPNOTSUPP`s, the capstone is blocked
   on driver support, not on our stack.
2. **Single contiguous IOVA for the 64 MiB VRAM body.** V1 exports **one**
   dma-buf per section with `nr_ranges = 1` (single contiguous BAR sub-range;
   V1 NOTES §Risks), and SPDK records exactly **one** dma-buf sink as the
   trailing SGL segment (`ctrlr_kvdev.c:618-630`). The 64 MiB VRAM result body
   must map as a **single contiguous** guest-IOVA range. If rocm-xio's VRAM
   write buffer is physically fragmented (multi-segment `GET_BUFFER_PAGES`), the
   mixed SGL would present >1 dma-buf segment → SPDK rejects the shape
   (`"unsupported SGL shape"`). Confirm the 64 MiB VRAM buffer is allocated
   contiguously (the V3 per-page fix exists precisely for the bulk case — watch
   for a multi-segment export).
3. **GPU one-shot budget.** ONE clean shot per reboot; a faulting shot dirties
   the SMU. Spend it only after [A]-gate equivalents (the CPU-only smoke) and the
   wiring gates are all green. Optionally `WARMUP=1` runs a 4 KiB shot first —
   but that **also** consumes from the same budget; prefer the CPU-only smoke.
4. **Page-size alignment / sub-4 KiB sink (R2).** The shared vfio listener drops
   unaligned BAR sub-ranges; R2 additionally rejects a sub-4 KiB dma-buf sink.
   The 64 MiB body is fine; a tiny warm-up value (<4 KiB) would be rejected by
   R2 — keep the warm-up ≥ 4 KiB.
5. **Debuglog gating.** The `dma-buf result_sink` proof line is a
   `SPDK_DEBUGLOG(nvmf_vfio)` — if nvmf_tgt was started without `-L nvmf_vfio`,
   gate [A] cannot be observed even on success. Start nvmf_tgt with `-L
   nvmf_vfio` for the capstone run (add it to the bringup if you want [A]
   guaranteed visible).
6. **Front bridge verbs init / intra-host rxm loopback.** If the SPDK front and
   the executor end up on the same GID, `ofi_rxm` can deadlock intra-host
   (slice-C8 §2). The bringup runs the executor on f0; if you see a hang on the
   first Exec, move the executor into netns `nvmf_e810` on f1 `10.110.0.2`
   (Topology 2) and re-point `--remote-executor`.

---

### Quick go/no-go checklist
- [ ] booted GPU-passthrough GRUB entry (`vfio-pci.ids=` in `/proc/cmdline`) — USER ACTION
- [ ] `ulimit -l` == unlimited in the bringup shell
- [ ] `bringup-v4-vram-direct.sh` GUARD 0 passed (no live rig)
- [ ] executor self-addr is `ofi+verbs;ofi_rxm://10.110.0.1:<port>`
- [ ] front created with `--remote-executor <verbs addr>`; `cntrl` socket up
- [ ] qemu launched with `x-dmabuf-export=true`; guest `/dev/nvme0` + `/dev/rocm-xio`
- [ ] guest xio-tester has `--exec-vram` (V3-baked)
- [ ] **CPU-only dma-buf smoke PASS** (`nkvx_dmabuf_test.sh` over verbs) — BEFORE the GPU shot
- [ ] **GPU shot:** [A] result_sink log + [B] CQE SUCCESS/DW0==64 MiB + [C] verify bit-exact
- [ ] live rig (pid 23630 / `/tmp/cgi-*`) untouched throughout
