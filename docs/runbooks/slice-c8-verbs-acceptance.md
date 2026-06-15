# Slice C8 — RDMA-over-DAC (ofi+verbs) acceptance RUNBOOK

Bead: **spdk-xmu.8**. Validates the full two-tier Exec RPC over the verbs
provider on the E810 DAC — same code as the sm/tcp dev rung, provider swapped.

This runbook is what the operator runs **on the rig**. It assumes the other
agent's deliverables already exist in the tree:
- `nkvx_c8_test.sh` (the C8 acceptance harness), and
- an **ASan build** of `nkvx_service` + the front client test binary.

> ## SAFETY — a live rig is running on this host
> - **Do NOT touch / signal qemu pid 23630** (GPU-passthrough VM) or any
>   running `nvmf_tgt`.
> - Do not disturb the E810 data-plane config, the existing irdma devices, the
>   `10.110.0.x` addressing, or netns `nvmf_e810` beyond the read-only / f1
>   usage described below.
> - The memlock change affects only **new** sessions/units for the service user;
>   it does not touch already-running processes.

---

## 0. Prerequisites (verify, do not change)

1. The DAC / RoCEv2 fabric the data plane already validated is up. f0 =
   `10.110.0.1` in the root namespace on `irdma0`; f1 = `10.110.0.2` lives in
   netns `nvmf_e810` on `irdma1` (a *distinct GID*).
2. Mercury links the **system** libfabric (`ldd .../libna.so` ->
   `/lib64/libfabric.so.1`, 2.3.1) which enumerates `verbs;ofi_rxm` on the
   irdma devices. Confirm the provider is visible (read-only):
   ```bash
   fi_info -p "verbs;ofi_rxm" | grep -E 'provider|domain|fabric'
   ```
   Expect at least one `verbs;ofi_rxm` entry on an irdma domain.
3. For the **rados-backed** executor run you need a reachable Ceph cluster and a
   KV pool (the `kvpool` / `--rados-pool`). The C2-skeleton (no `--rados-pool`)
   path replies `NOT_SUPPORTED` and is only useful for an init/round-trip smoke,
   not for under-load correctness.

---

## 1. Raise locked memory for the service user

The verbs CQ/QP/doorbell pinning by irdma needs locked memory above the default
8 MB cap; otherwise `HG_Init()` over verbs fails with CQ `-12` (ENOMEM). See
bead spdk-xmu.8 and `module/kvdev/rados/nkvx_service/deploy/`.

### Option A — interactive run (simplest for acceptance)
In the shell that will launch the executor/front:
```bash
ulimit -l unlimited      # requires privilege to raise the HARD cap
ulimit -l                # MUST print: unlimited   (do not proceed if it says 8192)
```
If `ulimit -l unlimited` is refused, the hard cap is still 8 MB for this user —
install the durable config (Option B) and start a fresh login session.

### Option B — durable (recommended)
```bash
sudo install -m 0644 \
  module/kvdev/rados/nkvx_service/deploy/99-nkvx-memlock.conf \
  /etc/security/limits.d/99-nkvx-memlock.conf
# edit the file first if the service user is not "nkvx" (e.g. set it to your login)
# then open a FRESH login shell for that user and verify:
ulimit -l                # expect: unlimited
```
(For a systemd-managed executor use the drop-in
`deploy/nkvx-executor.service.d/10-memlock.conf` and verify via
`/proc/<MainPID>/limits` — see the file header. Not required for an interactive
acceptance run.)

**Gate:** do not start any verbs step until `ulimit -l` reads `unlimited` in the
launching shell (or the unit's `Max locked memory` is `unlimited`).

---

## 2. Choose the two-node topology

The executor and the front need **distinct fabric addresses / GIDs** so the
RC connection setup over verbs is a real cross-endpoint path, not a self-loop.
Two options:

### Topology 1 — executor in a storage VM (preferred for the true acceptance gate)
- Executor runs inside the storage VM with its own SR-IOV VF / GID on the DAC;
  front runs on the host (or the other VM).
- **Pros:** exercises the real deployment shape (separate address spaces, real
  cross-DAC RC path, independent memlock domain, independent Ceph client).
  This is the on-hardware proof the slice is gated on.
- **Cons:** needs the storage VM + its VF + Ceph reachable from inside it; more
  moving parts to stand up.
- **Do NOT** stand this up by touching qemu pid 23630 — that is the GPU VM. Use
  a *separate* storage VM. If no separate storage VM is available, use Topology 2.

### Topology 2 — netns `nvmf_e810` f1 = 10.110.0.2 (distinct GID, single host)
- Executor binds the f1 GID inside netns `nvmf_e810` (`10.110.0.2`); front binds
  f0 (`10.110.0.1`) in the root ns. Two distinct GIDs on the same box.
- Run the executor inside the netns, e.g.:
  ```bash
  sudo ip netns exec nvmf_e810 \
    env -i PATH="$PATH" HOME="$HOME" bash -lc '
      ulimit -l unlimited; ulimit -l
      ./nkvx_service --listen "ofi+verbs;ofi_rxm://10.110.0.2" \
        --addr-file /tmp/nkvx_c8.addr \
        --rados-pool kvpool   # add --rados-conf/--rados-user as needed
    '
  ```
  > Note: `ip netns exec` needs privilege and re-enters a shell — re-apply
  > `ulimit -l unlimited` *inside* the netns shell and re-verify. Do not disturb
  > the existing tenants of netns `nvmf_e810`.
- **Pros:** single host, no extra VM; gives the two distinct GIDs the verbs RC
  path needs.
- **Cons:** both stacks share the box and the same Ceph client host; not the
  real VM deployment shape; you must not collide with the data plane already
  using `nvmf_e810`.

Either way the executor's published self-address will look like
`ofi+verbs;ofi_rxm://10.110.0.1:<port>` (or `...0.2:<port>`).

---

## 3. Start the executor (target)

Pick the topology from §2. For the host/root-ns executor on f0:
```bash
# in a memlock-raised shell (ulimit -l == unlimited)
./nkvx_service \
  --listen "ofi+verbs;ofi_rxm://10.110.0.1" \
  --addr-file /tmp/nkvx_c8.addr \
  --rados-pool kvpool        # + --rados-namespace/--rados-conf/--rados-user as needed
```
Expect on stdout:
```
nkvx_service: listening, self addr = ofi+verbs;ofi_rxm://10.110.0.1:<port>
```
- If you instead see a CQ `-12` / ENOMEM failure at init, **memlock is still
  capped** — go back to §1.
- The `--addr-file` is the front bootstrap (design OQ-8). The front derives its
  NA init string from the address prefix automatically (`ofi+verbs;ofi_rxm://`),
  so no extra front provider flag is needed.

The listen string is exactly: **`ofi+verbs;ofi_rxm://10.110.0.1`**
(`ofi_rxm` is the required RxM utility provider over the verbs core provider —
it supplies the reliable-datagram/connection management Mercury expects).

---

## 4. Run the acceptance harness (front, over verbs)

In a second memlock-raised shell (or VM/netns front per topology), point the C8
harness at the published address and the ASan build:
```bash
ulimit -l                          # confirm unlimited here too
./nkvx_c8_test.sh \
  --addr-file /tmp/nkvx_c8.addr \
  --listen-prefix "ofi+verbs;ofi_rxm://10.110.0.1"   # front origin GID
  # (use the harness's actual flags; the front NA init is derived from the addr)
```
The harness must cover, over the verbs provider:
1. **Under-load correctness** — many concurrent Exec forwards; every result is
   **bytewise-correct** vs the expected output, including the large (64 MiB)
   bulk-RMA result over RoCE (the C7 push path on real hardware).
2. **Abort / cancel, ASan-clean** — issue Exec then abort/cancel; the C6/C6b
   teardown (do-not-PUSH + ACK handshake) must complete with **no ASan report**
   (no UAF / leak) and exactly-once `ABORTED`, outstanding -> 0, executor stays up.

---

## 5. What PASS looks like

- Executor `HG_Init` over verbs **succeeds** and publishes
  `ofi+verbs;ofi_rxm://10.110.0.1:<port>`.
- Every under-load Exec returns the **correct bytes** (small inline + the
  64 MiB bulk result), no truncation, no corruption — over RDMA.
- Abort/cancel path is **ASan-clean** (0 errors, 0 leaks), exactly-once
  `ABORTED`, outstanding count returns to 0, executor remains alive and serving.
- Harness exit code 0.

## 6. Observations to capture (record in the bead)

- **Reactor-not-blocked invariant**: the front's poller-driven manual progress
  (`nkvx_front_progress(front, 0)`, non-blocking, design §4.2) never stalls the
  SPDK reactor under real verbs completion semantics. Capture reactor
  busy/idle / poller timing showing the reactor is not blocked during in-flight
  verbs Exec.
- **Latency**: end-to-end Exec latency over verbs vs the sm/tcp dev rung; note
  RC connection-setup cost (first Exec to a fresh peer is heavier — design §5).
- **Two-stack coexistence**: SPDK-verbs data plane + libfabric-verbs Exec hop
  on the same E810 both healthy concurrently (no MR-domain / device contention
  regressions).
- The memlock value that was in force (`ulimit -l`) and the topology used.

## 7. Out of scope / gated

- **No-stale-read criterion is GATED on bead spdk-xmu.9 (C5b, still OPEN)** —
  the cross-hop cache-invalidation path. Do **not** claim no-stale-read PASS
  from this runbook; record under-load correctness + abort-ASan-clean only, and
  defer the no-stale-read assertion to C5b.
- The fabric authN / trust gap (OQ-4) is testbed-OK only, not closed here.

---

### Quick checklist
- [ ] `ulimit -l` == `unlimited` in every shell that runs a verbs binary
- [ ] `fi_info -p "verbs;ofi_rxm"` shows an irdma domain
- [ ] Ceph `kvpool` reachable (rados-backed executor)
- [ ] Distinct GIDs (storage VM, or netns f1=10.110.0.2)
- [ ] Executor publishes `ofi+verbs;ofi_rxm://10.110.0.1:<port>`
- [ ] Under-load: all results bytewise-correct (incl. 64 MiB)
- [ ] Abort/cancel: ASan-clean, exactly-once ABORTED, outstanding->0
- [ ] Reactor-not-blocked + latency recorded
- [ ] no-stale-read explicitly deferred to spdk-xmu.9 (C5b)
- [ ] qemu pid 23630 untouched throughout
