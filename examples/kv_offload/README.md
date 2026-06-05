# Co-located KV-cache offload target provisioning

`kv_offload_provision.sh` brings up a **co-located SPDK NVMe-oF target** that
exposes **per-tenant librados KV namespaces** over vfio-user. It is the
data-plane bring-up an initContainer or sidecar runs on a node so a co-located
host (e.g. a vLLM worker offloading its KV-cache) can attach a KV-cache offload
namespace over a node-local vfio-user socket — no network round-trip.

The tenancy mapping follows
[ADR-0006](../../docs/adr/0006-kv-cache-offload-tenancy-mapping.md):

```
one NVMe-oF subsystem   ->  one rados pool          (the whole offload tier)
one NVMe KV namespace   ->  one rados namespace      per tenant tuple
        nsid 1..N       ->  rados-ns_1 .. rados-ns_N (in tenant order)
```

Each tenant gets its own `kvdev_rados` device bound to its rados namespace, and
each kvdev is bound to a KV namespace (nsid) under the single subsystem. Because
each tenant owns a distinct rados namespace, two tenants that happen to produce
the same key never collide, and per-tenant reclaim is a single namespace drop.

## What the script does

1. Starts (or connects to) `nvmf_tgt` and creates the `VFIOUSER` transport.
2. `kvdev_rados_register_cluster` once (one librados cluster handle for the tier).
3. For **each** tenant `TENANT:RADOS_NS`:
   - `kvdev_rados_create KvOff_<tenant> <cluster> <pool> --namespace <rados-ns>`
   - `nvmf_subsystem_add_kv_ns <nqn> KvOff_<tenant> -n <i>` → nsid `i`
4. Creates the subsystem and a single vfio-user listener.
5. Prints the resulting **nsid → tenant → rados-namespace** map and the
   vfio-user socket path a host attaches to.

It is idempotent-ish (re-running against a live RPC socket reuses the target and
tolerates already-created resources) and installs an `EXIT`/`INT`/`TERM` trap
that tears down only what *this* invocation started (unless `--keep`).

## Running it

```bash
./kv_offload_provision.sh \
    --ceph-conf  /etc/ceph/ceph.conf \
    --keyring    /etc/ceph/ceph.client.kvcache.keyring \
    --ceph-user  kvcache \
    --pool       kvpool \
    --domain-dir /run/kv-offload \
    --keep \
    modelA-tp0:kvns_modelA_tp0 \
    modelB-tp0:kvns_modelB_tp0
```

Each positional argument is `LOGICAL_TENANT:RADOS_NAMESPACE`. The script assigns
nsids in argument order. Run `./kv_offload_provision.sh --help` for all options.

Example output (two tenants):

```
=== KV offload target ready (ADR-0006 tenancy) ===
subsystem NQN : nqn.2026-06.io.spdk:kv-offload0
rados pool    : kvpool
RPC socket    : /run/kv-offload/rpc.sock
vfio-user sock: /run/kv-offload/domain/muser0/0   (a co-located host attaches to THIS path)

nsid -> (tenant) -> rados namespace:
  nsid   tenant               rados-namespace
  1      modelA-tp0           kvns_modelA_tp0
  2      modelB-tp0           kvns_modelB_tp0
==================================================
```

## How a co-located host attaches

The vfio-user listener creates a control socket at
`<domain-dir>/domain/muser0/0/cntrl`. A host attaches to the **directory that
contains it** — the `vfio-user sock` path printed above. With the in-process KV
host shim (`test/nvmf/kv_shim`), that path is the `vfu_addr`:

```c
struct kv_host_shim_opts opts = {
    .opts_size = sizeof(opts),
    .name      = "vllm-kv-offload",
    .vfu_addr  = "/run/kv-offload/domain/muser0/0",
    .nsid      = 0,        /* 0 == attach the first KV (CSI) namespace */
    .init_env  = true,
};
```

In a pod, the sidecar and the host share the domain dir via a shared volume (an
`emptyDir`), so the host process can reach the socket the sidecar created.

## Hugepages + VFIO prerequisites (production)

SPDK's DPDK environment and the vfio-user data path normally need **hugepages**
and access to **`/dev/vfio`**, with the memory pinned (`IPC_LOCK`). For a
co-located pod (see `k8s/` for a reference skeleton):

- **Hugepages**: reserve them on the node (kernel `hugepages=...` /
  `vm.nr_hugepages`, or the Kubernetes hugepages feature) and mount a
  hugepage-backed volume into the pod:
  ```yaml
  volumes:
    - name: hugepage
      emptyDir:
        medium: HugePages
  resources:
    limits:
      hugepages-2Mi: 512Mi
      memory: 512Mi
  ```
  The target then runs with hugepages (the default — do **not** pass
  `--no-huge`).

- **VFIO / pinned memory**: give the provisioning container `/dev/vfio` and the
  ability to lock memory:
  ```yaml
  securityContext:
    capabilities:
      add: ["IPC_LOCK", "SYS_ADMIN"]
    # privileged: true   # simplest; tighten in production
  volumeMounts:
    - name: dev-vfio
      mountPath: /dev/vfio
  volumes:
    - name: dev-vfio
      hostPath: { path: /dev/vfio, type: Directory }
  ```

- **Ceph credentials**: mount `ceph.conf` + a cephx keyring (scoped to the
  offload pool) as a `Secret`/`ConfigMap`.

### `--no-huge` (unprivileged / CI only)

For an unprivileged bring-up (no hugepages, no `/dev/vfio` reservation for the
target's own DPDK env — e.g. CI on a shared box), pass:

```bash
./kv_offload_provision.sh --no-huge -s 1024 ...
```

This runs `nvmf_tgt --no-huge -s <MB>`. It is **not** for production: it forgoes
hugepage-backed performance and is intended only for functional testing. The
end-to-end verification in this repo uses `--no-huge -s 1024`.

## Files

- `kv_offload_provision.sh` — the provisioning script (tested here).
- `k8s/` — **reference-only, untested** Kubernetes manifest skeleton showing the
  initContainer/sidecar + hugepages + VFIO/securityContext wiring.
