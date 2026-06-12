---
status: accepted
---

# wasmtime is dynamically loaded via its C API, with dual per-binding memory strategies

The rados-nkvx executor runs real `.wasm` on **wasmtime**, but does **not** statically
link it. `libwasmtime.so` is `dlopen`'d at runtime through wasmtime's **C API**, so the
SPDK build carries no wasmtime — and no Rust — dependency: only a vendored `wasmtime.h`
for signatures, behind a `--with-wasm` toggle that mirrors `--with-rbd`. All six
capabilities we need (custom host memory creator, pooling allocator, fuel, epoch
interruption, memory64, imports-free instantiation) are reachable through the C API
(verified against the headers, spdk-uo2 research), so no Rust shim is needed. Pin floor
**wasmtime 32.0.0** (first release exposing the pooling allocator in the C API);
target the **v36 LTS** line (24-month support).

## Considered options

- **Static-link wasmtime into `nvmf_tgt`** — rejected: bakes a heavy Rust JIT into SPDK
  core, exactly the upstream-appetite risk (plan §6). dlopen keeps SPDK core
  wasmtime-free; wasmtime is an optional, operator-provided runtime.
- **WAMR (C-native runtime)** — rejected: Rust-free and lighter, but weak on the four
  load-bearing features (epoch/fuel interruption, custom linear memory, memory64,
  pooling) the architecture is built on. We'd fight it for exactly what we need.
- **Rust C-ABI shim** — unnecessary: the C API covers all six features, so there is no
  Rust toolchain in the build at all.

## Consequences

- **Loading decouples the build, not the trust boundary.** wasm still JITs and runs in
  `nvmf_tgt`'s address space on the off-reactor worker. Safety is the *sandbox* (no
  imports + fuel/epoch/memory caps + restartable target), not dlopen.
- **Discovery & version gate.** `dlopen` by soname (loader search) with an optional
  explicit path override (kvdev RPC param / env var); load lazily on first wasm Exec;
  cache a `dlsym`'d function table. There is **no wasmtime version function** — gate by
  **symbol-presence probing** (`dlsym` the version-sensitive symbols, fail clean on
  NULL). Pooling is a compile-time `libwasmtime` feature; probe its symbols rather than
  trust the version (the official prebuilt c-api artifact enables it).
- **Graceful degradation.** Built-in modules (bytecount/identity) need no wasmtime and
  always work; a `runtime=wasm` binding fails with a distinct "runtime unavailable"
  status (never a crash) when the lib is absent/incompatible. wasm is purely additive
  over TB1.
- **Dual memory strategy (mutually exclusive per store).** wasmtime's custom host
  memory creator (zero-copy DMA backing) works **only** with the on-demand instance
  strategy and is **incompatible with the pooling allocator**. So the executor offers
  two strategies, selected per binding/namespace: **zero-copy/on-demand** for large
  immutable memory64 objects (GNN partitions — avoid copy-in) and **pooled/copy-in**
  for small hot objects (KV-transform — warm-instance reuse). WIMP (spdk-wji) starts
  with the simplest path — on-demand + a plain copy — needing neither feature;
  zero-copy custom memory is TB4, the pooled mode is a later KV-transform optimization.
  The `new_memory` callback's reserved/guard-size contract for DMA buffers needs
  empirical validation.
